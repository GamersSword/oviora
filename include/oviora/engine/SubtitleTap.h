#pragma once
// Oviora: a small CPU copy of one region of the processed real frames, a few times a second,
// for the in-game subtitle translation (src/platform/subtitle). The engine thread only records a
// region copy into a readback buffer after a frame was processed and later notices (by polling
// the fence, never waiting) that the copy is done; the consumer maps and copies the buffer on its
// own thread. One copy is in flight or waiting at a time, so a slow consumer only lowers the rate.
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace oviora::gfx { class D3D12DeviceContext; class CommandSlotRing; }

namespace oviora::engine {

struct SubtitleSample {
    uint64_t serial=0;
    uint32_t frameWidth=0,frameHeight=0;   // the whole processed frame
    uint32_t x=0,y=0,width=0,height=0;     // the region inside it, pixels
    uint32_t format=0;                     // DXGI_FORMAT of the pixels
    uint32_t bytesPerPixel=4;
    std::vector<uint8_t> pixels;           // tight rows, width * bytesPerPixel each
};

// Shared between the engine thread (SubtitleTap) and the consumer (any thread).
class SubtitleTapShared {
public:
    // Region in 0..1 of the frame; interval between copies.
    void configure(bool enabled,float x,float y,float w,float h,unsigned intervalMs);
    // The newest finished copy, if any; false otherwise. Any thread except the engine's.
    bool take(SubtitleSample& out);

private:
    friend class SubtitleTap;
    struct Ready {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t offset=0,rowPitch=0;
        SubtitleSample meta;
    };
    std::mutex mutex_;
    bool enabled_=false;float x_=0,y_=0,w_=0,h_=0;unsigned intervalMs_=250;
    bool hasReady_=false,consumerBusy_=false;
    Ready ready_;
};

// Engine thread side: lives in EngineController::run.
class SubtitleTap {
public:
    explicit SubtitleTap(std::shared_ptr<SubtitleTapShared> shared):shared_(std::move(shared)){}
    // After a real frame was processed: publishes a finished copy, may start the next one.
    void service(gfx::D3D12DeviceContext&,gfx::CommandSlotRing&,ID3D12Resource* frame,DXGI_FORMAT format);
    // Session end: waits (bounded) for a copy still in flight and withdraws the published one.
    void finish(gfx::D3D12DeviceContext&);
    ~SubtitleTap();

private:
    std::shared_ptr<SubtitleTapShared> shared_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    uint64_t bufferBytes_=0,fence_=0,serial_=0;
    uint64_t lastStartMs_=0;
    uint64_t offset_=0,rowPitch_=0;   // layout of the copy in flight
    SubtitleSample meta_;             // its frame and region
    bool logged_=false;
};

} // namespace oviora::engine
