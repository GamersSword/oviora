#include "oviora/engine/SubtitleTap.h"

#include "oviora/gfx/CommandSlotRing.h"
#include "oviora/gfx/D3D12DeviceContext.h"
#include "oviora/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace oviora::engine {

void SubtitleTapShared::configure(bool enabled,float x,float y,float w,float h,unsigned intervalMs){
    std::lock_guard lock(mutex_);
    enabled_=enabled&&std::isfinite(x)&&std::isfinite(y)&&std::isfinite(w)&&std::isfinite(h)&&w>0&&h>0;
    x_=std::clamp(x,0.0f,1.0f);y_=std::clamp(y,0.0f,1.0f);
    w_=std::clamp(w,0.0f,1.0f-x_);h_=std::clamp(h,0.0f,1.0f-y_);
    intervalMs_=std::clamp(intervalMs,50u,5000u);
    if(!enabled_&&!consumerBusy_){hasReady_=false;ready_={};}
}

bool SubtitleTapShared::take(SubtitleSample& out){
    Ready ready;
    {
        std::lock_guard lock(mutex_);
        if(!hasReady_||consumerBusy_)return false;
        consumerBusy_=true;ready=ready_;
    }
    // Mapped outside the lock: the engine does not touch the buffer while it is published.
    bool ok=false;
    uint8_t* data=nullptr;
    const uint64_t rowBytes=uint64_t(ready.meta.width)*ready.meta.bytesPerPixel;
    const uint64_t total=ready.offset+ready.rowPitch*(ready.meta.height?ready.meta.height-1:0)+rowBytes;
    D3D12_RANGE range{0,SIZE_T(total)};
    if(ready.buffer&&SUCCEEDED(ready.buffer->Map(0,&range,reinterpret_cast<void**>(&data)))){
        out=ready.meta;
        out.pixels.resize(size_t(rowBytes)*ready.meta.height);
        for(uint32_t y=0;y<ready.meta.height;++y)
            std::memcpy(out.pixels.data()+size_t(y)*rowBytes,data+ready.offset+size_t(y)*ready.rowPitch,size_t(rowBytes));
        D3D12_RANGE none{0,0};ready.buffer->Unmap(0,&none);
        ok=true;
    }
    std::lock_guard lock(mutex_);
    consumerBusy_=false;
    // Taken: the engine may record the next copy into the same buffer
    if(ready_.meta.serial==ready.meta.serial){hasReady_=false;ready_={};}
    return ok;
}

static uint32_t bytesPerPixelOf(DXGI_FORMAT f){
    switch(f){
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:case DXGI_FORMAT_R10G10B10A2_UNORM:return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
    default:return 0;
    }
}

void SubtitleTap::service(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,ID3D12Resource* frame,DXGI_FORMAT format){
    // 1) A copy in flight that the GPU finished: hand it to the consumer (no wait)
    if(fence_){
        if(ctx.fenceCompletedValue()<fence_)return;
        fence_=0;
        std::lock_guard lock(shared_->mutex_);
        if(!shared_->enabled_)return;
        shared_->ready_.buffer=buffer_;shared_->ready_.offset=offset_;shared_->ready_.rowPitch=rowPitch_;
        shared_->ready_.meta=meta_;shared_->hasReady_=true;
        return;
    }
    // 2) Start the next copy when it is time and the last one was taken
    float x,y,w,h;
    {
        std::lock_guard lock(shared_->mutex_);
        if(!shared_->enabled_||shared_->hasReady_||shared_->consumerBusy_)return;
        const uint64_t now=GetTickCount64();
        if(now-lastStartMs_<shared_->intervalMs_)return;
        lastStartMs_=now;
        x=shared_->x_;y=shared_->y_;w=shared_->w_;h=shared_->h_;
    }
    if(!frame)return;
    const uint32_t bpp=bytesPerPixelOf(format);
    const D3D12_RESOURCE_DESC desc=frame->GetDesc();
    if(!bpp||desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1){
        if(!logged_){logged_=true;oviora::log::warn("subtitle-tap",std::format("unsupported frame format={} dim={}",unsigned(format),unsigned(desc.Dimension)));}
        return;
    }
    const uint32_t fw=uint32_t(desc.Width),fh=desc.Height;
    const uint32_t x0=std::min(fw-1,uint32_t(std::lround(x*fw))),y0=std::min(fh-1,uint32_t(std::lround(y*fh)));
    const uint32_t cw=std::clamp<uint32_t>(uint32_t(std::lround(w*fw)),1u,fw-x0),ch=std::clamp<uint32_t>(uint32_t(std::lround(h*fh)),1u,fh-y0);
    D3D12_RESOURCE_DESC region=desc;region.Width=cw;region.Height=ch;region.MipLevels=1;region.DepthOrArraySize=1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 rowBytes=0,total=0;
    ctx.device()->GetCopyableFootprints(&region,0,1,0,&fp,&rows,&rowBytes,&total);
    if(!total)return;
    if(!buffer_||bufferBytes_<total){
        buffer_.Reset();bufferBytes_=0;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=1;bd.DepthOrArraySize=1;
        bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer_)))){
            buffer_.Reset();
            if(!logged_){logged_=true;oviora::log::warn("subtitle-tap",std::format("readback buffer bytes={} failed",total));}
            return;
        }
        bufferBytes_=total;
    }
    Status st=Status::Ok;uint32_t slot=0;
    auto* list=ring.acquireNext(slot,st);
    if(!list)return;
    ring.tag(slot,"subtitle-tap");
    // The processed frame rests in COMMON between passes (the presenter and the screenshot
    // read it from there); a copy promotes it to COPY_SOURCE implicitly and it decays back
    // when the list ends, so no barrier is recorded.
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=frame;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    dst.pResource=buffer_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    const D3D12_BOX box{x0,y0,0,x0+cw,y0+ch,1};
    list->CopyTextureRegion(&dst,0,0,0,&src,&box);
    if(!ring.submitAndSignal(slot))return;
    fence_=ring.lastSignaledValue();
    offset_=fp.Offset;rowPitch_=fp.Footprint.RowPitch;
    meta_={};meta_.serial=++serial_;meta_.frameWidth=fw;meta_.frameHeight=fh;meta_.x=x0;meta_.y=y0;meta_.width=cw;meta_.height=ch;
    meta_.format=uint32_t(format);meta_.bytesPerPixel=bpp;
    if(serial_==1)oviora::log::info("subtitle-tap",std::format("first region copy frame={}x{} region={},{} {}x{} format={}",fw,fh,x0,y0,cw,ch,unsigned(format)));
}

void SubtitleTap::finish(gfx::D3D12DeviceContext& ctx){
    if(fence_&&ctx.fenceCompletedValue()<fence_)(void)ctx.waitForFenceValue(fence_,1000);
    fence_=0;
    std::lock_guard lock(shared_->mutex_);
    if(!shared_->consumerBusy_){shared_->hasReady_=false;shared_->ready_={};}
}

SubtitleTap::~SubtitleTap()=default;

} // namespace oviora::engine
