#pragma once
#include "Types.h"
#include <deque>
#include <string>
#include <vector>
struct SDL_Gamepad;
struct SDL_AudioStream;
namespace oviora::remoteplay {
// SDL calls stay on the Win32 UI owner, independently of GPU processing.
class ControllerInput {
public:
    ~ControllerInput(){stop();shutdown();}
    // SDL's gamepad subsystem takes ~1.3 s to start (device enumeration), on this (the UI) thread:
    // started once while the app opens and kept for the app's life, so Bağlan no longer froze the
    // connecting screen for that long. start() only takes the pads then.
    bool warmUp();
    bool start(uint32_t preferredDevice=0);
    void stop(); // the pads go, the subsystem stays (warmUp); also turns follow mode off
    void shutdown();
    // 谁按键就用谁 (PS5 手柄控制): every pad is opened and the one whose button went
    // down last is the one read, so a streaming tool's virtual pad that appears
    // after a local pad still works. Off (default): the first pad found.
    void setFollowLastPressed(bool on);
    ControllerState poll(bool focused);
    void feedback(ControllerFeedback,bool focused);
    // Settings → Kumandalar → Titreşim: rumble multiplier (0 off … 5, 1 as sent) and the
    // DualSense haptics gain (1 as the console sends it).
    void setFeedbackScale(float rumble,float haptics){rumbleScale_=rumble;hapticScale_=haptics;}
    void releaseEffects();
    bool calibrate();
    bool connected()const{return gamepad_!=nullptr;}
    struct Capabilities {bool connected=false,gyro=false,accel=false,touch=false,triggers=false,haptics=false,calibrating=false;std::string name;};
    Capabilities capabilities()const;
private:
    void activate(SDL_Gamepad*);
    void deactivate();
    bool opened(uint32_t id)const;
    SDL_Gamepad* gamepad_=nullptr;
    std::vector<SDL_Gamepad*> others_; // follow mode: open, not read
    bool follow_=false;
    bool initialized_=false;
    HostTime enumerateAt_=0;uint32_t preferredDevice_=0;
    std::array<int8_t,2> touchIds_{-1,-1};uint8_t nextTouchId_=0;
    bool gyro_=false,accel_=false,effectsActive_=false,audioInitialized_=false,hapticAttempted_=false;
    SDL_AudioStream* hapticStream_=nullptr;
    std::array<ControllerState::Touch,2> touchState_{};
    std::deque<std::array<ControllerState::Touch,2>> touchQueue_;
    std::array<float,3> gyroBias_{},biasSum_{},sensorGyro_{},sensorAccel_{0,1,0};
    unsigned calibrationSamples_=0;bool calibrating_=false,haveGyro_=false,haveAccel_=false;
    HostTime calibrationStart_=0;uint64_t sensorStamp_=0;
    float rumbleScale_=1.f,hapticScale_=1.f;


};
}
