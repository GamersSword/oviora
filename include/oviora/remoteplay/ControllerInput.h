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
    // SDL's gamepad subsystem is started once while the app opens and kept for the app's life, so
    // Bağlan does not wait for it. start() only takes the pads then. Almost all of its start-up time
    // (~1.3 s on a PC with many USB devices, on the UI thread) was the DirectInput scan, which only old
    // generic pads need (DualSense, DS4, Xbox and Switch pads come through HIDAPI / XInput / RawInput):
    // off (start-up then ~8 ms) unless Ayarlar > Kumandalar > Eski kumandalar turns it on (next start).
    bool warmUp();
    void setLegacyPads(bool on){legacyPads_=on;}
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
    // Ayarlar > Kumandalar > Jiroskopla nişan: 0 off, 1 while L2 is held, 2 always; sensitivity 0.2-4 (1 normal).
    // The pad's turn speed moves the right stick, for games without gyro aim of their own.
    void setGyroAim(int mode,float sensitivity){gyroAimMode_=mode;gyroAimSens_=sensitivity;}
    void releaseEffects();
    bool calibrate();
    bool connected()const{return gamepad_!=nullptr;}
    // battery: percent (-1 unknown or wired without a battery), charging: on the cable and charging
    struct Capabilities {bool connected=false,gyro=false,accel=false,touch=false,triggers=false,haptics=false,calibrating=false;std::string name;int battery=-1;bool charging=false;};
    Capabilities capabilities()const;
private:
    void activate(SDL_Gamepad*);
    void deactivate();
    bool opened(uint32_t id)const;
    SDL_Gamepad* gamepad_=nullptr;
    std::vector<SDL_Gamepad*> others_; // follow mode: open, not read
    bool follow_=false;
    bool initialized_=false;
    bool legacyPads_=false;
    int gyroAimMode_=0;float gyroAimSens_=1.f;
    // The console's light bar and player number, kept for a pad that comes later
    bool haveLed_=false;std::array<uint8_t,3> ledColor_{};int playerIndex_=-1;
    void applyLights();
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
