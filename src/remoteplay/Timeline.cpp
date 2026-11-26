// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/remoteplay/Timeline.h"
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
namespace oviora::remoteplay {
Sequence16Extender::Result Sequence16Extender::observe(std::uint16_t n) noexcept {
    if(!initialized_){initialized_=true;previous_=n;extended_=n;return {extended_,true,true,0};}
    const auto delta=static_cast<std::uint16_t>(n-previous_);
    if(delta==0 || delta>=0x8000u)return {extended_,false,false,0};
    if(extended_>std::numeric_limits<std::uint64_t>::max()-delta)return {};
    extended_+=delta;previous_=n;return {extended_,true,false,static_cast<std::uint32_t>(delta-1)};
}
namespace {
std::optional<std::int64_t> rescale(std::uint64_t n,std::uint32_t den) noexcept {
    if(den==0)return {};
    constexpr auto max=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto whole=n/den,rem=n%den;
    if(whole>max/10000000u)return {};
    const auto a=whole*10000000u,b=rem*10000000u/den;
    if(a>max-b) return {};
    return static_cast<std::int64_t>(a+b);
}
}
void RemotePlayClock::reset(HostTime origin,std::uint32_t fps) {
    if(origin<0 || (fps!=30&&fps!=60))throw std::invalid_argument("Invalid local clock origin/rate");
    *this={};origin_=origin;fps_=fps;
}
std::optional<EstimatedStamp> RemotePlayClock::video(std::uint64_t index,HostTime arrival) {
    if(!fps_ || arrival<origin_)return {};
    if(anchored_ && (index<=lastIndex_ || arrival<lastArrival_))return {};
    bool gap=false;
    if(!anchored_){videoAnchor_=arrival-origin_;firstIndex_=index;anchored_=true;}
    else if(arrival-lastArrival_>2000000 || index-lastIndex_>fps_/2u){
        videoAnchor_=arrival-origin_;firstIndex_=index;gap=true;
        phaseWindow_.clear();phase_=0;
    }
    const auto delta=rescale(index-firstIndex_,fps_);
    if(!delta || videoAnchor_>std::numeric_limits<std::int64_t>::max()-*delta)return {};
    const auto nominal=videoAnchor_+*delta;
    const auto error=(arrival-origin_)-nominal;
    while(!phaseWindow_.empty()&&phaseWindow_.front().arrival<=arrival-10000000)phaseWindow_.pop_front();
    while(!phaseWindow_.empty()&&phaseWindow_.back().error>=error)phaseWindow_.pop_back();
    phaseWindow_.push_back({arrival,error});
    const auto step=10000000/static_cast<std::int64_t>(fps_);
    // Even the least late frame of the last second is a quarter second off the
    // estimate: the console sent fewer (or more) frames than the profile's rate
    // for a while, as under heavy loss. The 0.5% slew below would need minutes,
    // and the sound follows these stamps meanwhile (1.5 s behind the picture in
    // a field log, 2026-11-26): start again from this frame, as after a gap.
    if(!gap&&index!=firstIndex_&&std::llabs(phaseWindow_.front().error-phase_)>2500000){
        videoAnchor_=arrival-origin_;firstIndex_=index;
        phaseWindow_.clear();phase_=0;
        lastArrival_=arrival;lastIndex_=index;lastPts_=videoAnchor_;
        return EstimatedStamp{videoAnchor_,step,true};
    }
    // At most 0.5% slew; even after a jitter burst successive PTS cannot
    // reverse or abruptly jump. A real network gap still resets above. More
    // than 100 ms off for a whole second is a rate change, not jitter: up to
    // half a frame per frame then, so a stream running as slow as 40 fps is
    // caught up without a discontinuity.
    const auto lag=phaseWindow_.front().error-phase_;
    const auto maxSlew=std::llabs(lag)>1000000?step/2:step/200;
    phase_+=std::clamp(phaseWindow_.front().error-phase_,-maxSlew,maxSlew);
    if(phase_>0&&nominal>std::numeric_limits<std::int64_t>::max()-phase_)return {};
    auto pts=nominal+phase_;
    if(index!=firstIndex_&&!gap)pts=std::max(pts,lastPts_+1);
    lastArrival_=arrival;lastIndex_=index;lastPts_=pts;
    return EstimatedStamp{pts,step,gap};
}
std::optional<std::int64_t> RemotePlayClock::audioPts(std::uint64_t first,std::uint32_t rate,HostTime start) const noexcept {
    if(!fps_ || start<origin_ || rate<8000 || rate>192000)return {};
    const auto delta=rescale(first,rate);const auto base=start-origin_;
    if(!delta || base>std::numeric_limits<std::int64_t>::max()-*delta) return {};
    return base+*delta;
}
} // namespace oviora::remoteplay
