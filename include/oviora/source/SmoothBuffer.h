#pragma once
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>

namespace oviora::source {
// Yayın → Akıcılık tamponu: the timing of a small playout buffer for internet PS5 streams.
// Decoded frames wait about one frame interval and leave on an even cadence, so arrival
// jitter on a long line (receive-age spikes of 30..230 ms) is not passed on to the screen.
// The hold grows to two frames when the line is rough and shrinks back when it calms.
// Pure timing in 100 ns host units; the caller owns the frames and its own lock.
class SmoothBuffer {
public:
    static constexpr unsigned kMinFrames=1,kMaxFrames=2;
    void start(double fps){
        interval_=fps>0?int64_t(1e7/fps):333333;
        frames_=kMinFrames;restart();
    }
    // A reconnect or a reset: the next frame starts a new cadence; the learned size stays.
    void restart(){next_=0;lastArrival_=0;rough_.clear();}
    unsigned frames()const{return frames_;}
    double holdMs()const{return double(frames_*interval_)/10000;}
    // At most this many frames wait; the caller drops the oldest beyond it.
    std::size_t capacity()const{return frames_+2;}
    // A decoded frame entered the queue. A gap far from the frame interval is jitter.
    void arrived(int64_t now){
        if(lastArrival_&&std::llabs(now-lastArrival_-interval_)>interval_*3/4)rough(now);
        lastArrival_=now;
    }
    // Whether the oldest waiting frame (decoded at `oldest`) may leave now.
    bool due(int64_t now,int64_t oldest){
        if(!next_)next_=oldest+int64_t(frames_)*interval_; // first frame: hold it the target time
        return now>=next_;
    }
    // A frame left; `waiting` frames are still queued behind it.
    void released(int64_t now,std::size_t waiting){
        // Waited for a late frame: the screen already stalled, start the cadence from here
        if(now-next_>interval_/2){next_=now;rough(now);}
        next_+=interval_;
        // More waiting than the target: catch up gently (an eighth of a frame per frame)
        if(waiting>frames_)next_-=interval_/8;
        // The line calmed down: back to one frame (the catch-up above drains the extra one)
        if(frames_>kMinFrames&&now-lastRough_>kCalm100ns){frames_=kMinFrames;change_="calm";}
    }
    // Set once after the size changed: why (jitter / calm); the caller logs it.
    std::string takeChange(){return std::exchange(change_,std::string{});}
private:
    static constexpr int64_t kWindow100ns=100000000;  // 10 s
    static constexpr int64_t kCalm100ns=200000000;    // 20 s without a rough moment
    static constexpr std::size_t kRoughToGrow=3;      // rough moments in the window to grow
    void rough(int64_t now){
        lastRough_=now;
        rough_.push_back(now);
        while(!rough_.empty()&&now-rough_.front()>kWindow100ns)rough_.pop_front();
        if(frames_<kMaxFrames&&rough_.size()>=kRoughToGrow){
            ++frames_;if(next_)next_+=interval_; // one frame more of hold from the next release on
            rough_.clear();change_="jitter";
        }
    }
    int64_t interval_=333333,next_=0,lastArrival_=0,lastRough_=0;
    unsigned frames_=kMinFrames;
    std::deque<int64_t> rough_;
    std::string change_;
};
}
