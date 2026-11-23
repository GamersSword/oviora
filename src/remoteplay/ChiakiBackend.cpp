// SPDX-License-Identifier: GPL-3.0-only
// Uses chiaki-ng's AGPL-3.0-only + OpenSSL-exception APIs; retain both licenses.
// Windows native initialization and source regression evidence: see execution log.
#include "oviora/remoteplay/ChiakiBackend.h"
#include "oviora/Log.h"
#ifdef OVIORA_HAS_IZLET
#include "WatchReceiver.h"
#endif
#include <format>
#include <chiaki/session.h>
#if !defined(CHIAKI_OVIORA_VIDEO_METADATA_API) || CHIAKI_OVIORA_VIDEO_METADATA_API != 1
#error The reviewed Chiaki video metadata patch is required
#endif
#include <chiaki/opusdecoder.h>
#include <chiaki/opusencoder.h>
#include <chiaki/controller.h>
#include <chiaki/orientation.h>
#include <cmath>
#include <chiaki/log.h>
#include <chiaki/remote/holepunch.h>
#include <condition_variable>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
namespace oviora::remoteplay {
static_assert(CHIAKI_PSN_ACCOUNT_ID_SIZE == 8);
static_assert(CHIAKI_SESSION_AUTH_SIZE == 16);
namespace {
void wipe(void* ptr,std::size_t n) noexcept {
    auto* p=static_cast<volatile unsigned char*>(ptr);
    while(n--) *p++=0;
}
BackendResult result(ChiakiErrorCode code,const char* operation) {
    return {code==CHIAKI_ERR_SUCCESS,static_cast<int>(code),operation};
}
// Oviora: leaving a PSN session can take seconds; it runs in the background and the
// next PSN connect waits for it, PSN refuses a new session while the old one is there.
std::mutex psnReleaseMutex;
std::condition_variable psnReleaseDone;
int psnReleasesPending=0;
void psnReleaseLog(ChiakiLogLevel,const char*,void*) noexcept {}
ChiakiLog psnQuietLog{CHIAKI_LOG_ERROR,psnReleaseLog,nullptr};
void releasePsnAsync(ChiakiHolepunchSession hp,std::shared_ptr<PsnRelay> relay){
    chiaki_holepunch_session_set_log(hp,&psnQuietLog);
    {std::lock_guard lock(psnReleaseMutex);++psnReleasesPending;}
    std::thread([hp,relay]{
        chiaki_holepunch_session_fini(hp);chiaki_holepunch_session_free(hp);
        std::lock_guard lock(psnReleaseMutex);--psnReleasesPending;psnReleaseDone.notify_all();
    }).detach();
}
void waitPsnReleases(std::chrono::milliseconds timeout){
    std::unique_lock lock(psnReleaseMutex);
    psnReleaseDone.wait_for(lock,timeout,[]{return psnReleasesPending==0;});
}
struct RelayTrampoline {
    static int http(void* u,const char* op,const char* sid,const char* body,size_t len,char** resp,size_t* respLen){return static_cast<PsnRelay*>(u)->http(op,sid,body,len,resp,respLen);}
    static bool open(void* u,const char* pushctx){return static_cast<PsnRelay*>(u)->open(pushctx);}
    static int poll(void* u,int64_t after,char*** events,size_t* count,int64_t* last){return static_cast<PsnRelay*>(u)->poll(after,events,count,last);}
    static void close(void* u){static_cast<PsnRelay*>(u)->close();}
};
bool hexToBytes(std::string_view hex,std::vector<std::uint8_t>& out){
    if(hex.empty()||hex.size()%2)return false;
    auto nibble=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    out.clear();
    for(size_t i=0;i<hex.size();i+=2){const int a=nibble(hex[i]),b=nibble(hex[i+1]);if(a<0||b<0)return false;out.push_back(std::uint8_t(a*16+b));}
    return true;
}
}
PairingCredentials::PairingCredentials(PairingCredentials&& other) noexcept { *this=std::move(other); }
PairingCredentials& PairingCredentials::operator=(PairingCredentials&& other) noexcept {
    if(this==&other) return *this;
    registrationKey=other.registrationKey;sessionKey=other.sessionKey;accountId=other.accountId;
    wipe(other.registrationKey.data(),other.registrationKey.size());
    wipe(other.sessionKey.data(),other.sessionKey.size());wipe(other.accountId.data(),other.accountId.size());
    return *this;
}
PairingCredentials::~PairingCredentials(){wipe(registrationKey.data(),registrationKey.size());wipe(sessionKey.data(),sessionKey.size());wipe(accountId.data(),accountId.size());}
BackendResult initializeChiaki(){
    static std::once_flag flag;
    static ChiakiErrorCode code=CHIAKI_ERR_UNKNOWN;
    std::call_once(flag,[]{code=chiaki_lib_init();});
    return result(code,"chiaki_lib_init");
}
namespace { std::atomic<int> g_connectStage{0}; std::atomic<bool> g_cantDisplay{false}; }
ConnectStage connectStage(){return ConnectStage(g_connectStage.load());}
bool consoleCantDisplay(){return g_cantDisplay.load();}
// The connecting steps go to the log (field report 2026-11-18: a friend's PSN connection ended
// with "0 / 0" and nothing showed which step it stopped at)
static void setStage(ConnectStage s){
    const int old=g_connectStage.exchange(int(s));
    if(old!=int(s)){
        static constexpr const char* names[]={"idle","psn-reach","psn-punch-ctrl","opening","psn-punch-data","connected"};
        oviora::log::info("remoteplay-stage",std::format("{} -> {}",names[std::clamp(old,0,5)],names[std::clamp(int(s),0,5)]));
    }
}
// An upstream warning/error for the log, with nothing secret left in it: long hex runs (keys,
// nonces), long digit runs (account ids) and IPv4 addresses are masked, then cut to 160 chars.
static std::string sanitizedUpstream(std::string_view text){
    // Byte dumps ("01 a3 ff ...") are dropped whole: mostly hex digits between spaces
    size_t hex=0,other=0;
    for(const char c:text){if(c==' ')continue;if((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))++hex;else ++other;}
    if(hex+other>=16&&hex*10>=(hex+other)*6)return "[veri dökümü gizlendi]";
    std::string out;out.reserve(std::min<size_t>(text.size(),200));
    size_t i=0;
    const auto isHex=[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');};
    while(i<text.size()&&out.size()<160){
        const char c=text[i];
        if(isHex(c)){
            size_t j=i;bool digits=true;
            while(j<text.size()&&isHex(text[j])){digits=digits&&text[j]>='0'&&text[j]<='9';++j;}
            const size_t run=j-i;
            if(run>=16||(digits&&run>=10)){out+="[gizli]";i=j;continue;}
            // a.b.c.d
            if(digits&&run<=3&&j<text.size()&&text[j]=='.'){
                size_t k=i;int parts=0;
                while(k<text.size()&&parts<4){size_t d=k;while(d<text.size()&&text[d]>='0'&&text[d]<='9')++d;if(d==k||d-k>3)break;++parts;k=d;if(parts<4){if(k<text.size()&&text[k]=='.')++k;else break;}}
                if(parts==4){out+="[ip]";i=k;continue;}
            }
            out.append(text.substr(i,run));i=j;continue;
        }
        out+=(static_cast<unsigned char>(c)<0x20?' ':c);++i;
    }
    return out;
}
// The on-screen keyboard: its state, and the running session the PC's text goes to (one at a time)
// Two locks: the state one is taken by the session's event callback, the session one is held while a
// call into the session runs (so stop() cannot free it underneath) - never both at once.
namespace { std::mutex g_keyboardMutex,g_keyboardSessionMutex; ConsoleKeyboard g_keyboard; ChiakiSession* g_keyboardSession=nullptr; }
static void keyboardEvent(bool open,const char* text){
    std::lock_guard lock(g_keyboardMutex);
    g_keyboard.open=open;g_keyboard.text=text?std::string(text):std::string();++g_keyboard.revision;
}
static bool keyboardOpen(bool close){
    std::lock_guard lock(g_keyboardMutex);
    if(!g_keyboard.open)return false;
    if(close){g_keyboard.open=false;++g_keyboard.revision;}
    return true;
}
template<class F> static bool withKeyboardSession(F f){
    std::lock_guard lock(g_keyboardSessionMutex);
    return g_keyboardSession&&f(g_keyboardSession)==CHIAKI_ERR_SUCCESS;
}
ConsoleKeyboard consoleKeyboard(){std::lock_guard lock(g_keyboardMutex);return g_keyboard;}
bool consoleKeyboardSetText(const std::string& utf8){
    if(!keyboardOpen(false))return false;
    const std::string text=utf8.substr(0,CHIAKI_KEYBOARD_TEXT_MAX_BYTES);
    return withKeyboardSession([&](ChiakiSession* s){return chiaki_session_keyboard_set_text(s,text.c_str());});
}
bool consoleKeyboardAccept(){
    return keyboardOpen(true)&&withKeyboardSession([](ChiakiSession* s){return chiaki_session_keyboard_accept(s);});
}
bool consoleKeyboardReject(){
    return keyboardOpen(true)&&withKeyboardSession([](ChiakiSession* s){return chiaki_session_keyboard_reject(s);});
}
struct ChiakiBackend::Impl {
    explicit Impl(SessionInbox::Token t):token(std::move(t)),owner(std::this_thread::get_id()){}
    SessionInbox::Token token;
    std::thread::id owner;
    ChiakiSession session{};
    ChiakiOpusDecoder opus{};
    // Oviora: the microphone (old Oviora's StreamSession: 2 channels, 16 bit, 48 kHz, 480-frame
    // blocks into the lib's Opus encoder and audio sender). Owner thread only.
    ChiakiOpusEncoder mic{};
    bool micInitialized=false,micReady=false,micConnected=false,micOn=false;
    ChiakiLog log{};
    VideoProfile profile;
    std::string host;
    ChiakiHolepunchSession holepunch=nullptr;
    std::shared_ptr<PsnRelay> relay;
    bool initialized=false,opusInitialized=false;
    std::atomic<bool> active=false,started=false,connected=false;
    std::atomic<std::uint64_t> warnings=0,errors=0,videoCallbacks=0,audioCallbacks=0;
    // The size of the last codec configuration: a new one is the console switching profile.
    std::uint32_t configWidth=0,configHeight=0;
    std::atomic<int> quitReason=0,apiError=0;
    std::atomic<unsigned> appReason=0; // the console's RP-Application-Reason when it refused
    std::atomic<uint64_t> callbackRejected=0,transportErrors=0,assemblyErrors=0;
    std::atomic<int64_t> serverTargetBitrate=-1;
    std::atomic<uint64_t> qualityReports=0;
    // PS5 手柄控制: set before the session starts, read only by the callbacks.
    bool discardMedia=false;
    bool relayMedia=false;
    MediaRelayCallbacks mediaRelay;
    std::atomic<bool> teeEnabled{false};
    std::mutex teeMutex;
    std::shared_ptr<const MediaRelayCallbacks> teeCallbacks;
    std::vector<std::uint8_t> teeCodecConfig;
    std::uint32_t teeConfigWidth=0,teeConfigHeight=0;
    bool teeNeedsConfig=false;
    ChiakiAudioSink opusSink{};
    std::atomic<std::uint32_t> relayAudioChannels{2},relayAudioRate{48000},relayAudioFrameSize{480};
    // Only the upstream audio/Opus callback thread reads/writes these fields.
    std::uint32_t channels=0,rate=0;
    std::uint64_t sampleIndex=0;
    bool audioDiscontinuity=true;
    ChiakiOrientationTracker orientation{};ChiakiAccelNewZero accelZero{};
    uint64_t lastMotion=0;
    std::mutex feedbackMutex;ControllerFeedback feedback;
    // Watch mode ("İzle" WebRTC viewer): no Chiaki session, feeds video/Opus into the same pipeline.
    bool isWatch=false;
    std::shared_ptr<WatchRoute> watchRoute;
#ifdef OVIORA_HAS_IZLET
    std::unique_ptr<WatchReceiver> watchReceiver;
#endif
    std::mutex watchVideoMutex,watchAudioMutex,watchReasonMutex;
    std::string watchEndReason;
    std::vector<std::uint8_t> watchVps,watchSps,watchPps,watchLastConfig;

    void setWatchReason(std::string reason) noexcept {
        std::lock_guard lock(watchReasonMutex);
        if(watchEndReason.empty()) watchEndReason=reason.empty()?std::string("Yayına bağlanılamadı."):std::move(reason);
    }

    static bool videoTrace(){static const bool on=std::getenv("OVIORA_TEST_PS5_VIDEO_TRACE")!=nullptr;return on;}
    static void logCallback(ChiakiLogLevel level,const char* message,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        // Oviora: why the console refused the session request (a fixed reason code, no keys);
        // the failure message tells the user what to do with it (RemotePlaySessionSource)
        if(message&&level==CHIAKI_LOG_ERROR){
            unsigned reason=0;
            if(std::sscanf(message,"Reported Application Reason: %x",&reason)==1){
                self.appReason=reason;
                oviora::log::warn("remoteplay-session",std::format("console refused the session request, reason={:#x}",reason));
            }else{
                const std::string_view text=message;
                for(const std::string_view known:{"Session request connect failed","Failed to receive session request response",
                        "Failed to parse session request response","Nonce invalid","Server RP-Version is unknown"})
                    if(text.starts_with(known)){oviora::log::warn("remoteplay-session",std::string(known));break;}
            }
        }
        if(message&&level==CHIAKI_LOG_VERBOSE){
            int target=-1;
            if(std::sscanf(message,"StreamConnection received connection quality: target_bitrate=%d,",&target)==1&&target>=0){self.serverTargetBitrate=target;++self.qualityReports;}
        }
        // Raw upstream log strings/hexdumps may contain keys. Do not forward them.
        if(level==CHIAKI_LOG_ERROR)++self.errors;
        if(level==CHIAKI_LOG_WARNING)++self.warnings;
        // Until the picture is up, the session's own warnings/errors are logged (masked, see
        // sanitizedUpstream) so a failed connection says which step failed; 60 per process
        if(message&&!self.connected.load()&&(level==CHIAKI_LOG_ERROR||level==CHIAKI_LOG_WARNING)){
            static std::atomic<int> budget=400;
            if(budget.fetch_sub(1)>0)oviora::log::warn("remoteplay-upstream",sanitizedUpstream(message));
        }
        // PSN connect (field reports: a friend's PS5 over the server's relay fails at changing steps): the hole
        // punching's own progress lines, from a fixed list of prefixes without keys, masked like the warnings
        if(message&&level==CHIAKI_LOG_INFO&&!self.connected.load()){
            static constexpr std::string_view psnSteps[]={"check_candidates","holepunch_session_create_offer","chiaki_holepunch_session_punch_holes",
                "relay:","Responding to request","Sent response","Received an extra","Initiating random","push notification",">> "};
            const std::string_view text=message;
            for(const auto prefix:psnSteps)if(text.starts_with(prefix)){
                static std::atomic<int> budget=600;
                if(budget.fetch_sub(1)>0)oviora::log::info("remoteplay-psn",sanitizedUpstream(message));
                break;
            }
        }
        // OVIORA_TEST_PS5_VIDEO_TRACE=1 (test runs): the video receiver's own messages about
        // profiles and frames, from a fixed list of prefixes that carry no keys, rate-limited.
        if(message&&videoTrace()){
            static constexpr std::string_view safe[]={"Switched to profile","Packet has invalid adaptive","Video Receiver","Frame Processor",
                "  0: ","  1: ","  2: ","Video Profiles","Still waiting for IDR","FEC failed","Received IDR","Detected missing","Failed to complete frame","Missing reference frame","Video FEC failure","Failed to parse video header","Unit is","Received duplicate","Video callback","LaunchSpec: offering"};
            const std::string_view text=message;
            for(const auto prefix:safe)if(text.starts_with(prefix)){
                static std::atomic<int> budget=400;
                if(budget.fetch_sub(1)>0)oviora::log::info("remoteplay-video-trace",std::string(text.substr(0,std::min<size_t>(text.size(),180))));
                break;
            }
        }
        // Classify in memory; no upstream strings or hexdumps leave this callback.
        if(message&&(level==CHIAKI_LOG_ERROR||level==CHIAKI_LOG_WARNING)){
            const std::string_view text=message;
            if(text.find("Takion")!=text.npos||text.find("timeout")!=text.npos)++self.transportErrors;
            if(text.find("Video")!=text.npos||text.find("video")!=text.npos||text.find("frame")!=text.npos||text.find("FEC")!=text.npos)++self.assemblyErrors;
        }
    }
    static void eventCallback(ChiakiEvent* event,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!event||!self.active.load())return;
        switch(event->type){
        case CHIAKI_EVENT_CONNECTED:self.connected=true;setStage(ConnectStage::Connected);self.token.connected();break;
        // PSN: the data connection is punched after the control one; until it is finished the
        // connecting screen says the console was reached and the picture connection is being made
        case CHIAKI_EVENT_HOLEPUNCH:if(!self.connected)setStage(event->data_holepunch.finished?ConnectStage::Opening:ConnectStage::PsnData);break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:self.token.loginPinRequired();break;
        case CHIAKI_EVENT_QUIT:
            self.connected=false;self.quitReason=static_cast<int>(event->quit.reason);setStage(ConnectStage::Idle);g_cantDisplay=false;
            // The console's own words when it ends the stream ("Server shutting down" ...): no keys, short
            if(event->quit.reason_str)oviora::log::warn("remoteplay-session",std::format("console ended the session: reason={} text={}",
                int(event->quit.reason),std::string_view(event->quit.reason_str).substr(0,120)));
            // A remote termination is not a successful decoded EOF.
            self.token.failed(2000+static_cast<int>(event->quit.reason));break;
        case CHIAKI_EVENT_RUMBLE:{std::lock_guard lock(self.feedbackMutex);self.feedback.rumble=true;self.feedback.left=event->rumble.left;self.feedback.right=event->rumble.right;break;}
        case CHIAKI_EVENT_TRIGGER_EFFECTS:{std::lock_guard lock(self.feedbackMutex);auto& f=self.feedback;f.triggers=true;f.leftTrigger[0]=event->trigger_effects.type_left;f.rightTrigger[0]=event->trigger_effects.type_right;std::copy_n(event->trigger_effects.left,10,f.leftTrigger.begin()+1);std::copy_n(event->trigger_effects.right,10,f.rightTrigger.begin()+1);break;}
        case CHIAKI_EVENT_MOTION_RESET:{std::lock_guard lock(self.feedbackMutex);self.feedback.motionReset=true;break;}
        case CHIAKI_EVENT_KEYBOARD_OPEN:case CHIAKI_EVENT_KEYBOARD_TEXT_CHANGE:keyboardEvent(true,event->keyboard.text_str);break;
        case CHIAKI_EVENT_KEYBOARD_REMOTE_CLOSE:keyboardEvent(false,nullptr);break;
        default:break; // No mic or standby side effects.

        }
    }
    static void hapticsCallback(uint8_t* data,size_t size,void* user)noexcept{
        auto& self=*static_cast<Impl*>(user);
        if(!self.active||!data||!size||size%4||size>1200)return;
        try{std::lock_guard lock(self.feedbackMutex);auto& pcm=self.feedback.haptics;
            if(pcm.size()+size/2>600)pcm.clear();
            const auto at=pcm.size();pcm.resize(at+size/2);std::memcpy(pcm.data()+at,data,size);
        }catch(...){++self.errors;}
    }
    bool pushVideoSample(SampleKind kind,std::span<const std::uint8_t> bytes,
        std::uint32_t width,std::uint32_t height,HostTime arrival100ns,
        std::optional<std::uint16_t> wireFrameIndex,std::int32_t framesLost,
        bool referenceRecovered,bool allowCraKeyframe) noexcept {
        if(!active.load()||bytes.empty())return false;
        try{
            const auto nal=inspectAnnexB(bytes,profile.codec);
            if(!nal.valid || (!nal.hasConfig&&!nal.hasPicture)){++callbackRejected;return false;}
            if((kind==SampleKind::AccessUnit && !nal.hasPicture) ||
                (kind==SampleKind::CodecConfig && (!nal.hasConfig || nal.hasPicture))){++callbackRejected;return false;}
            VideoSample sample;
            sample.generation=token.generation();sample.arrival100ns=arrival100ns;
            sample.codec=profile.codec;
            sample.kind=kind;
            if(wireFrameIndex)sample.wireFrameIndex=wireFrameIndex;
            sample.width=width;sample.height=height;
            if(sample.kind==SampleKind::CodecConfig&&(width!=configWidth||height!=configHeight)){
                oviora::log::info("remoteplay-video",std::format("console video profile {}x{}{}",width,height,
                    configWidth?std::format(" (was {}x{})",configWidth,configHeight):std::string()));
                configWidth=width;configHeight=height;
            }
            sample.framesLost=framesLost;sample.referenceRecovered=referenceRecovered;
            sample.keyframe=nal.idr||allowCraKeyframe;
            if(relayMedia){
                if(mediaRelay.onVideo){
                    mediaRelay.onVideo(sample.kind,bytes,width,height,nal.idr,
                        framesLost>0||referenceRecovered);
                }
                ++videoCallbacks;
                return true;
            }
            if(!isWatch){
                if(sample.kind==SampleKind::CodecConfig){
                    std::shared_ptr<const MediaRelayCallbacks> teeCb;
                    {
                        std::lock_guard lock(teeMutex);
                        teeCodecConfig.assign(bytes.begin(),bytes.end());
                        teeConfigWidth=width;teeConfigHeight=height;
                        teeNeedsConfig=false;
                        if(teeEnabled.load(std::memory_order_relaxed))teeCb=teeCallbacks;
                    }
                    if(teeCb&&teeCb->onVideo){
                        teeCb->onVideo(sample.kind,bytes,width,height,false,false);
                    }
                }else if(teeEnabled.load(std::memory_order_acquire)){
                    std::shared_ptr<const MediaRelayCallbacks> teeCb;
                    std::vector<std::uint8_t> pendingConfig;
                    std::uint32_t cfgW=0,cfgH=0;
                    {
                        std::lock_guard lock(teeMutex);
                        teeCb=teeCallbacks;
                        if(teeCb&&teeCb->onVideo&&teeNeedsConfig&&!teeCodecConfig.empty()){
                            pendingConfig=teeCodecConfig;cfgW=teeConfigWidth;cfgH=teeConfigHeight;
                            teeNeedsConfig=false;
                        }
                    }
                    if(teeCb&&teeCb->onVideo){
                        if(!pendingConfig.empty()){
                            teeCb->onVideo(SampleKind::CodecConfig,pendingConfig,cfgW,cfgH,false,false);
                        }
                        teeCb->onVideo(sample.kind,bytes,width,height,nal.idr,
                            framesLost>0||referenceRecovered);
                    }
                }
            }
            sample.payload=PaddedBytes::copy(bytes);++videoCallbacks;
            const bool accepted=token.video(std::move(sample));if(!accepted)++callbackRejected;return accepted;
        }catch(...){apiError=-1001;token.failed(-1001);return false;}
    }
    void handleWatchVideo(std::span<const std::uint8_t> annexB,HostTime arrival100ns) noexcept {
        if(!active.load()||annexB.empty())return;
        std::lock_guard lock(watchVideoMutex);
        const bool isH264=profile.codec==Codec::H264;
        std::vector<std::uint8_t> pictureBuf;
        bool hasPicture=false,isIrap=false,configTouched=false;
        auto matchStart=[](std::span<const std::uint8_t> b,std::size_t pos)->std::size_t{
            if(pos+3<=b.size()&&b[pos]==0&&b[pos+1]==0){
                if(b[pos+2]==1)return 3;
                if(pos+4<=b.size()&&b[pos+2]==0&&b[pos+3]==1)return 4;
            }
            return 0;
        };
        struct NalSlice{std::size_t start,end;unsigned type;};
        std::vector<NalSlice> nals;
        std::size_t pos=0;
        while(pos+3<=annexB.size()){
            const std::size_t prefix=matchStart(annexB,pos);
            if(!prefix){++pos;continue;}
            const std::size_t hdr=pos+prefix;
            if(hdr>=annexB.size()||(annexB[hdr]&0x80u)!=0){++callbackRejected;return;}
            unsigned type=0;
            if(isH264){
                type=annexB[hdr]&31u;
                if(type==0||type>23u){++callbackRejected;return;}
            }else{
                if(hdr+1>=annexB.size()||(annexB[hdr+1]&7u)==0){++callbackRejected;return;}
                type=(annexB[hdr]>>1)&63u;
            }
            if(!nals.empty())nals.back().end=pos;
            nals.push_back({pos,annexB.size(),type});
            pos=hdr+(isH264?1u:2u);
        }
        if(nals.empty())return;
        for(const auto& n:nals){
            const auto slice=annexB.subspan(n.start,n.end-n.start);
            if(isH264){
                if(n.type==7u){watchSps.assign(slice.begin(),slice.end());configTouched=true;}
                else if(n.type==8u){watchPps.assign(slice.begin(),slice.end());configTouched=true;}
                else{
                    pictureBuf.insert(pictureBuf.end(),slice.begin(),slice.end());
                    if(n.type>=1u&&n.type<=5u)hasPicture=true;
                }
            }else{
                if(n.type==32u){watchVps.assign(slice.begin(),slice.end());configTouched=true;}
                else if(n.type==33u){watchSps.assign(slice.begin(),slice.end());configTouched=true;}
                else if(n.type==34u){watchPps.assign(slice.begin(),slice.end());configTouched=true;}
                else{
                    pictureBuf.insert(pictureBuf.end(),slice.begin(),slice.end());
                    if(n.type<=31u)hasPicture=true;
                    if(n.type==19u||n.type==20u||n.type==21u)isIrap=true;
                }
            }
        }
        if(configTouched){
            const bool ready=isH264?(!watchSps.empty()&&!watchPps.empty()):(!watchVps.empty()&&!watchSps.empty()&&!watchPps.empty());
            if(ready){
                std::vector<std::uint8_t> combined;
                combined.reserve((isH264?0:watchVps.size())+watchSps.size()+watchPps.size());
                if(!isH264)combined.insert(combined.end(),watchVps.begin(),watchVps.end());
                combined.insert(combined.end(),watchSps.begin(),watchSps.end());
                combined.insert(combined.end(),watchPps.begin(),watchPps.end());
                if(combined!=watchLastConfig){
                    watchLastConfig=combined;
                    (void)pushVideoSample(SampleKind::CodecConfig,combined,profile.width,profile.height,
                        arrival100ns,std::nullopt,0,false,false);
                }
            }
        }
        if(hasPicture&&!pictureBuf.empty()){
            if(!connected.exchange(true)){setStage(ConnectStage::Connected);token.connected();}
            (void)pushVideoSample(SampleKind::AccessUnit,pictureBuf,profile.width,profile.height,
                arrival100ns,std::nullopt,0,false,isIrap);
        }
    }
    void handleWatchAudio(std::span<const std::uint8_t> opusPacket) noexcept {
        if(!active.load()||opusPacket.empty()||!opusInitialized||!opusSink.frame_cb)return;
        std::lock_guard lock(watchAudioMutex);
        if(!active.load())return;
        opusSink.frame_cb(const_cast<std::uint8_t*>(opusPacket.data()),opusPacket.size(),opusSink.user);
    }
    static bool videoCallback(std::uint8_t* data,std::size_t size,
        const ChiakiOvioraVideoSampleInfo* info,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!self.active.load()||!data||!size||!info)return false;
        // Counted and accepted, never rejected: a false return makes Chiaki drop
        // the reference frame, leave the last complete frame behind and send a
        // corrupt-frame report to the console for every following sample.
        if(self.discardMedia){++self.videoCallbacks;return true;}
        const auto kind=info->kind==CHIAKI_OVIORA_SAMPLE_FRAME?SampleKind::AccessUnit:SampleKind::CodecConfig;
        const std::optional<std::uint16_t> wireIdx=info->frame_index_valid?std::optional<std::uint16_t>(info->frame_index):std::nullopt;
        return self.pushVideoSample(kind,{data,size},info->width,info->height,monotonic100ns(),
            wireIdx,info->frames_lost,info->reference_recovered,false);
    }
    // PS5 手柄控制 plays the capture card's audio. A sink is still attached: with
    // no frame callback Chiaki's audio receiver returns before freeing each
    // buffered frame. Nothing is decoded.
    static void discardAudio(std::uint8_t*,std::size_t,void* user) noexcept {++static_cast<Impl*>(user)->audioCallbacks;}
    static void relayAudioHeader(ChiakiAudioHeader* header,void* user) noexcept {
        if(!header)return;
        auto& self=*static_cast<Impl*>(user);
        self.relayAudioChannels.store(header->channels,std::memory_order_relaxed);
        self.relayAudioRate.store(header->rate,std::memory_order_relaxed);
        self.relayAudioFrameSize.store(header->frame_size,std::memory_order_relaxed);
    }
    static void relayAudioFrame(std::uint8_t* data,std::size_t size,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!self.active.load())return;
        try{
            if(self.mediaRelay.onAudio){
                std::span<const std::uint8_t> packet;
                if(data&&size)packet={data,size};
                self.mediaRelay.onAudio(packet,
                    self.relayAudioChannels.load(std::memory_order_relaxed),
                    self.relayAudioRate.load(std::memory_order_relaxed),
                    self.relayAudioFrameSize.load(std::memory_order_relaxed));
            }
            ++self.audioCallbacks;
        }catch(...){++self.errors;}
    }
    static void desktopAudioHeader(ChiakiAudioHeader* header,void* user) noexcept {
        if(!header)return;
        auto& self=*static_cast<Impl*>(user);
        self.relayAudioChannels.store(header->channels,std::memory_order_relaxed);
        self.relayAudioRate.store(header->rate,std::memory_order_relaxed);
        self.relayAudioFrameSize.store(header->frame_size,std::memory_order_relaxed);
        if(self.opusSink.header_cb)self.opusSink.header_cb(header,self.opusSink.user);
    }
    static void desktopAudioFrame(std::uint8_t* data,std::size_t size,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(self.active.load(std::memory_order_relaxed)&&self.teeEnabled.load(std::memory_order_acquire)){
            try{
                std::shared_ptr<const MediaRelayCallbacks> teeCb;
                {std::lock_guard lock(self.teeMutex);teeCb=self.teeCallbacks;}
                if(teeCb&&teeCb->onAudio){
                    std::span<const std::uint8_t> packet;
                    if(data&&size)packet={data,size};
                    teeCb->onAudio(packet,
                        self.relayAudioChannels.load(std::memory_order_relaxed),
                        self.relayAudioRate.load(std::memory_order_relaxed),
                        self.relayAudioFrameSize.load(std::memory_order_relaxed));
                }
            }catch(...){++self.errors;}
        }
        if(self.opusSink.frame_cb)self.opusSink.frame_cb(data,size,self.opusSink.user);
    }
    static void opusSettings(std::uint32_t channels,std::uint32_t rate,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        // Sample position stays monotonic across repeated Opus headers within
        // one session; restarting at zero makes AudioIngress reject new PCM.
        self.channels=channels;self.rate=rate;self.audioDiscontinuity=true;
        if((channels!=1&&channels!=2)||rate!=48000){self.channels=0;self.token.failed(-1003);}
    }
    static void opusFrame(std::int16_t* data,std::size_t count,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!self.active.load()||!self.channels||!data)return;
        if(count==0||count>self.rate/5u||count>(std::numeric_limits<std::uint64_t>::max)()-self.sampleIndex){self.token.failed(-1004);return;}
        try{
            PcmBlock block;block.generation=self.token.generation();block.channels=self.channels;block.rate=self.rate;
            block.firstSample=self.sampleIndex;block.arrival100ns=monotonic100ns();block.discontinuity=self.audioDiscontinuity;
            // Opus count is samples PER CHANNEL, not bytes or interleaved elements.
            block.samples.assign(data,data+count*self.channels);self.sampleIndex+=count;++self.audioCallbacks;
            if(self.token.audio(std::move(block))) self.audioDiscontinuity=false;
        }catch(...){self.apiError=-1002;self.token.failed(-1002);}
    }
    BackendResult record(ChiakiErrorCode code,const char* name){apiError=static_cast<int>(code);return result(code,name);}
    bool onOwner()const noexcept{return owner==std::this_thread::get_id();}
};
ChiakiBackend::ChiakiBackend()=default;
ChiakiBackend::~ChiakiBackend(){
    if(!p_)return;
    try{
        const auto stopped=stop();
        if(!stopped.ok && p_){
            // A failed join cannot be repaired by freeing memory still used by callbacks.
            // Quarantine/leak this one session rather than create a use-after-free.
            // The owner must treat stop failure as fatal and not reconnect this object.
            p_->active=false;(void)p_.release();
        }
    }catch(...){if(p_){p_->active=false;(void)p_.release();}}
}
BackendResult ChiakiBackend::start(const NativeConnectRequest& request,SessionInbox::Token token,bool discardMedia,MediaRelayCallbacks relay){
    if(p_)return {false,-1100,"already_initialized"};
    if((!request.psn&&!request.watch&&!validHost(request.host))||request.video.validate()||token.generation()==0)return {false,-1101,"validate_connect"};
    if(request.watch){
#ifndef OVIORA_HAS_IZLET
        (void)discardMedia;(void)relay;
        return {false,-1115,"watch_unavailable"};
#else
        p_=std::make_unique<Impl>(std::move(token));auto& s=*p_;s.profile=request.video;
        s.isWatch=true;s.watchRoute=request.watch;
        chiaki_log_init(&s.log,CHIAKI_LOG_WARNING|CHIAKI_LOG_ERROR,Impl::logCallback,&s);
        chiaki_opus_decoder_init(&s.opus,&s.log);s.opusInitialized=true;
        chiaki_opus_decoder_set_cb(&s.opus,Impl::opusSettings,Impl::opusFrame,&s);
        chiaki_opus_decoder_get_sink(&s.opus,&s.opusSink);
        ChiakiAudioHeader hdr{};chiaki_audio_header_set(&hdr,2,16,48000,2880);
        if(s.opusSink.header_cb)s.opusSink.header_cb(&hdr,s.opusSink.user);
        g_cantDisplay=false;setStage(ConnectStage::Opening);
        s.active=true;s.started=true;
        s.watchReceiver=std::make_unique<WatchReceiver>();
        WatchReceiver::Callbacks cbs;
        cbs.onVideo=[&s](std::span<const std::uint8_t> annexB,HostTime arrival100ns){s.handleWatchVideo(annexB,arrival100ns);};
        cbs.onAudio=[&s](std::span<const std::uint8_t> opusPacket){s.handleWatchAudio(opusPacket);};
        cbs.onFailed=[&s](std::string reason){
            s.connected=false;setStage(ConnectStage::Idle);s.setWatchReason(std::move(reason));
            s.token.failed(-1117);
        };
        if(!s.watchReceiver->start(s.watchRoute,std::move(cbs))){
            s.setWatchReason("Yayına bağlanılamadı.");
            s.active=false;s.started=false;s.token.failed(-1116);
            (void)stop();
            return {false,-1116,"watch_start"};
        }
        lastWatchEndReason_.clear();
        return {true,0,"watch_start"};
#endif
    }
    auto init=initializeChiaki();if(!init.ok)return init;
    p_=std::make_unique<Impl>(std::move(token));auto& s=*p_;s.host=request.psn?std::string():request.host;s.profile=request.video;
    s.relayMedia=bool(relay.onVideo)||bool(relay.onAudio);
    s.discardMedia=discardMedia&&!s.relayMedia;
    s.mediaRelay=std::move(relay);
    // Verbose formatting has per-packet overhead: enable only for explicit diagnostics.
    // Callback above whitelists numeric quality fields and never forwards raw text.
    const bool qualityTrace=std::getenv("OVIORA_TEST_PS5_QUALITY_TRACE")!=nullptr;
    chiaki_log_init(&s.log,CHIAKI_LOG_WARNING|CHIAKI_LOG_ERROR|(qualityTrace?CHIAKI_LOG_VERBOSE:0)|((Impl::videoTrace()||request.psn)?CHIAKI_LOG_INFO:0),Impl::logCallback,&s);
    ChiakiConnectInfo info{};info.ps5=true;info.host=s.host.c_str();
    if(request.psn){
        // Oviora: PSN session + hole punch for the control connection (the data one is
        // punched by the session itself), as chiaki-ng's GUI does before session init.
        const auto& psn=*request.psn;
        std::vector<std::uint8_t> duid;
        if(!hexToBytes(psn.duid,duid)){s.token.failed(-1110);p_.reset();return {false,-1110,"psn_duid"};}
        waitPsnReleases(std::chrono::seconds(15));
        s.relay=psn.relay;
        // In relay mode there is no token on this PC; the library needs a non-empty one.
        s.holepunch=chiaki_holepunch_session_init(s.relay?"oviora-relay":psn.token.c_str(),&s.log);
        if(!s.holepunch){s.token.failed(-1111);p_.reset();return {false,-1111,"psn_session_init"};}
        if(s.relay){
            ChiakiHolepunchRelay r{};r.http=RelayTrampoline::http;r.open=RelayTrampoline::open;r.poll=RelayTrampoline::poll;r.close=RelayTrampoline::close;r.user=s.relay.get();
            chiaki_holepunch_session_set_relay(s.holepunch,&r);
        }
        auto fail=[&](ChiakiErrorCode code,const char* op){
            auto out=s.record(code,op);s.token.failed(out.code);setStage(ConnectStage::Idle);
            releasePsnAsync(s.holepunch,s.relay);s.holepunch=nullptr;p_.reset();return out;
        };
        setStage(ConnectStage::PsnReach);
        ChiakiErrorCode hp=chiaki_holepunch_upnp_discover(s.holepunch);
        if(hp!=CHIAKI_ERR_SUCCESS)return fail(hp,"psn_upnp_discover");
        hp=chiaki_holepunch_session_create(s.holepunch);
        if(hp!=CHIAKI_ERR_SUCCESS)return fail(hp,"psn_session_create");
        hp=holepunch_session_create_offer(s.holepunch);
        if(hp!=CHIAKI_ERR_SUCCESS)return fail(hp,"psn_create_offer");
        hp=chiaki_holepunch_session_start(s.holepunch,duid.data(),CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5);
        if(hp!=CHIAKI_ERR_SUCCESS)return fail(hp,"psn_session_start");
        setStage(ConnectStage::PsnPunch);
        hp=chiaki_holepunch_session_punch_hole(s.holepunch,CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
        if(hp!=CHIAKI_ERR_SUCCESS)return fail(hp,"psn_punch_ctrl");
        info.holepunch_session=s.holepunch;
    }
    std::memcpy(info.regist_key,request.credentials.registrationKey.data(),request.credentials.registrationKey.size());
    std::memcpy(info.morning,request.credentials.sessionKey.data(),request.credentials.sessionKey.size());
    if(request.psn)std::memcpy(info.psn_account_id,request.psn->accountId.data(),request.psn->accountId.size());
    else std::memcpy(info.psn_account_id,request.credentials.accountId.data(),request.credentials.accountId.size());
    chiaki_connect_video_profile_preset(&info.video_profile,
        request.video.height==720?CHIAKI_VIDEO_RESOLUTION_PRESET_720p:CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
        request.video.fps==30?CHIAKI_VIDEO_FPS_PRESET_30:CHIAKI_VIDEO_FPS_PRESET_60);
    info.video_profile.codec=request.video.codec==Codec::H264?CHIAKI_CODEC_H264:request.video.codec==Codec::H265Hdr?CHIAKI_CODEC_H265_HDR:CHIAKI_CODEC_H265;
    info.video_profile.bitrate=request.video.bitrateKbps;
    info.video_profile_auto_downgrade=true;info.enable_keyboard=!request.viewOnly;info.enable_dualsense=!request.viewOnly||discardMedia;
    // Match 1913 (Yakamoz): 5% reported loss cap on both home and internet links, and
    // disable IDR-on-FEC-failure so VideoReceiver uses its 16-frame reference recovery
    // instead of stalling for 1+ RTT and requesting large IDR bursts on every lost unit.
    info.auto_regist=false;info.packet_loss_max=0.05;info.enable_idr_on_fec_failure=false;
    oviora::log::info("remoteplay",std::format("reported loss cap={:.0f}% idrOnFecFailure={} ({})",info.packet_loss_max*100,int(info.enable_idr_on_fec_failure),request.psn?"internet":"home"));
    info.adaptive_resolution=request.adaptiveResolution;
    if(const char* loss=std::getenv("OVIORA_TEST_REPORT_LOSS"))
        oviora::log::warn("remoteplay-test",std::format("OVIORA_TEST_REPORT_LOSS={}: the console is told of that packet loss (nothing is dropped)",loss));
    const auto code=chiaki_session_init(&s.session,&info,&s.log);
    wipe(info.regist_key,sizeof(info.regist_key));wipe(info.morning,sizeof(info.morning));wipe(info.psn_account_id,sizeof(info.psn_account_id));
    if(code!=CHIAKI_ERR_SUCCESS){
        auto out=s.record(code,"chiaki_session_init");s.token.failed(out.code);
        // A failed init has already left the PSN session (fini); only its memory is ours.
        if(s.holepunch){chiaki_holepunch_session_free(s.holepunch);s.holepunch=nullptr;}
        p_.reset();return out;
    }
    s.initialized=true;
    if(s.discardMedia){ChiakiAudioSink sink{};sink.user=&s;sink.frame_cb=Impl::discardAudio;chiaki_session_set_audio_sink(&s.session,&sink);}
    else if(s.relayMedia){ChiakiAudioSink sink{};sink.user=&s;sink.header_cb=Impl::relayAudioHeader;sink.frame_cb=Impl::relayAudioFrame;chiaki_session_set_audio_sink(&s.session,&sink);}
    else{
        chiaki_opus_decoder_init(&s.opus,&s.log);s.opusInitialized=true;
        chiaki_opus_decoder_set_cb(&s.opus,Impl::opusSettings,Impl::opusFrame,&s);
        chiaki_opus_decoder_get_sink(&s.opus,&s.opusSink);
        ChiakiAudioSink sink{};sink.user=&s;sink.header_cb=Impl::desktopAudioHeader;sink.frame_cb=Impl::desktopAudioFrame;
        chiaki_session_set_audio_sink(&s.session,&sink);
    }
    if(!s.discardMedia&&!s.relayMedia){
        // The sender is made against this session's stream connection; nothing is sent until
        // the microphone is switched on.
        chiaki_opus_encoder_init(&s.mic,&s.log);s.micInitialized=true;
        ChiakiAudioHeader micHeader{};chiaki_audio_header_set(&micHeader,2,16,48000,480);
        chiaki_opus_encoder_header(&micHeader,&s.mic,&s.session);
        s.micReady=s.mic.opus_encoder!=nullptr&&s.mic.audio_sender!=nullptr;
    }
    ChiakiAudioSink hapticSink{};hapticSink.user=&s;hapticSink.frame_cb=Impl::hapticsCallback;
    chiaki_session_set_haptics_sink(&s.session,&hapticSink);
    chiaki_session_set_event_cb(&s.session,Impl::eventCallback,&s);
    chiaki_session_set_oviora_video_sample_cb(&s.session,Impl::videoCallback,&s);
    // Protected content (a film app, some menus): the console stops the picture and says so
    ChiakiCtrlDisplaySink display{};display.user=&s;
    display.cantdisplay_cb=[](void* user,bool cant)noexcept{if(static_cast<Impl*>(user)->active.load())g_cantDisplay=cant;};
    chiaki_session_ctrl_set_display_sink(&s.session,&display);
    g_cantDisplay=false;
    if(!request.psn)setStage(ConnectStage::Opening);
    s.active=true;
    const auto startCode=chiaki_session_start(&s.session);
    if(startCode!=CHIAKI_ERR_SUCCESS){auto out=s.record(startCode,"chiaki_session_start");s.active=false;s.token.failed(out.code);(void)stop();return out;}
    s.started=true;
    {std::lock_guard lock(g_keyboardSessionMutex);g_keyboardSession=&s.session;}
    {std::lock_guard lock(g_keyboardMutex);g_keyboard={};}
    return s.record(startCode,"chiaki_session_start");
}
void ChiakiBackend::setMediaTee(MediaRelayCallbacks tee){
    if(!p_||p_->isWatch)return;
    auto& s=*p_;
    const bool enable=bool(tee.onVideo)||bool(tee.onAudio);
    std::shared_ptr<const MediaRelayCallbacks> next;
    if(enable)next=std::make_shared<MediaRelayCallbacks>(std::move(tee));
    std::lock_guard lock(s.teeMutex);
    s.teeCallbacks=std::move(next);
    s.teeNeedsConfig=enable;
    s.teeEnabled.store(enable,std::memory_order_release);
}
BackendResult ChiakiBackend::stop(){
    if(!p_)return {true,0,"already_stopped"};
    auto& s=*p_;
    if(!s.onOwner())return {false,-1102,"wrong_owner_thread"};
    if(s.isWatch){
        s.active=false;setStage(ConnectStage::Idle);g_cantDisplay=false;
#ifdef OVIORA_HAS_IZLET
        if(s.watchReceiver){s.watchReceiver->stop();s.watchReceiver.reset();}
#endif
        if(s.opusInitialized){chiaki_opus_decoder_fini(&s.opus);s.opusInitialized=false;}
        s.started=false;s.connected=false;
        {std::lock_guard lock(s.watchReasonMutex);lastWatchEndReason_=s.watchEndReason;}
        p_.reset();return {true,0,"watch_stop"};
    }
    s.active=false;s.teeEnabled.store(false,std::memory_order_release);setStage(ConnectStage::Idle);g_cantDisplay=false;
    {std::lock_guard lock(g_keyboardSessionMutex);if(g_keyboardSession==&s.session)g_keyboardSession=nullptr;}
    {std::lock_guard lock(g_keyboardMutex);g_keyboard.open=false;++g_keyboard.revision;}
    ChiakiErrorCode stopResult=CHIAKI_ERR_SUCCESS;
    if(s.started){
        ChiakiControllerState idle;chiaki_controller_state_set_idle(&idle);
        (void)s.record(chiaki_session_set_controller_state(&s.session,&idle),"release_controller");
        const auto stopCode=chiaki_session_stop(&s.session);
        stopResult=stopCode;
        // Join even if stop reported an error. Never fini while a callback is alive.
        const auto joinCode=chiaki_session_join(&s.session);
        if(joinCode!=CHIAKI_ERR_SUCCESS)return s.record(joinCode,"chiaki_session_join");
        s.started=false;s.connected=false;
        if(stopCode!=CHIAKI_ERR_SUCCESS)s.apiError=static_cast<int>(stopCode);
    }
    if(s.opusInitialized){chiaki_opus_decoder_fini(&s.opus);s.opusInitialized=false;}
    if(s.micInitialized){chiaki_opus_encoder_fini(&s.mic);s.micInitialized=false;s.micReady=false;}
    {std::lock_guard lock(s.teeMutex);s.teeCallbacks.reset();s.teeCodecConfig.clear();}
    // The session thread has ended: nothing uses the PSN session anymore.
    ChiakiHolepunchSession hp=s.holepunch;s.holepunch=nullptr;
    if(s.initialized){s.session.holepunch_session=nullptr;chiaki_session_fini(&s.session);s.initialized=false;}
    if(hp)releasePsnAsync(hp,s.relay);
    wipe(&s.session,sizeof(s.session));
    p_.reset();return result(stopResult,"stop_join_fini");
}
BackendResult ChiakiBackend::requestIdr(){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1103,"idr_without_session"};
    if(p_->isWatch){
#ifdef OVIORA_HAS_IZLET
        if(p_->watchReceiver)p_->watchReceiver->requestKeyframe();
#endif
        return {true,0,"watch_pli"};
    }
    return p_->record(chiaki_session_request_idr(&p_->session),"chiaki_session_request_idr");
}
BackendResult ChiakiBackend::gotoBed(){
    if(!p_||!p_->onOwner()||!p_->started||!p_->connected)return {false,-1107,"goto_bed_without_session"};
    if(p_->isWatch)return {true,0,"watch_noop"};
    return p_->record(chiaki_session_goto_bed(&p_->session),"chiaki_session_goto_bed");
}
BackendResult ChiakiBackend::setMicrophone(bool on){
    if(!p_||!p_->onOwner()||!p_->started||!p_->connected)return {false,-1108,"microphone_without_session"};
    if(p_->isWatch)return {true,0,"watch_noop"};
    auto& s=*p_;
    if(!s.micReady)return {false,-1109,"microphone_encoder"};
    if(on==s.micOn)return {true,0,"microphone_unchanged"};
    if(on&&!s.micConnected){
        const auto code=chiaki_session_connect_microphone(&s.session);
        if(code!=CHIAKI_ERR_SUCCESS)return s.record(code,"chiaki_session_connect_microphone");
        s.micConnected=true;
    }
    // The lib's flag is the state being left: true (muted until now) sends "unmute".
    const auto code=chiaki_session_toggle_microphone(&s.session,on);
    if(code==CHIAKI_ERR_SUCCESS)s.micOn=on;
    return s.record(code,"chiaki_session_toggle_microphone");
}
BackendResult ChiakiBackend::submitMicrophone(const std::int16_t* pcm,std::size_t frames){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1112,"microphone_without_session"};
    if(p_->isWatch)return {true,0,"watch_noop"};
    auto& s=*p_;
    if(!s.micOn||!s.micReady)return {true,0,"microphone_off"};
    if(!pcm||frames!=480)return {false,-1113,"microphone_block"};
    std::int16_t block[480*2];std::memcpy(block,pcm,sizeof(block));
    chiaki_opus_encoder_frame(block,&s.mic);
    return {true,0,"chiaki_opus_encoder_frame"};
}
BackendResult ChiakiBackend::submitLoginPin(std::string_view pin){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1104,"pin_without_session"};
    if(p_->isWatch)return {true,0,"watch_noop"};
    if(pin.empty()||pin.size()>8||!std::all_of(pin.begin(),pin.end(),[](char c){return c>='0'&&c<='9';}))return {false,-1105,"invalid_login_pin"};
    return p_->record(chiaki_session_set_login_pin(&p_->session,reinterpret_cast<const std::uint8_t*>(pin.data()),pin.size()),"chiaki_session_set_login_pin");
}
BackendResult ChiakiBackend::submitController(const ControllerState& state){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1106,"controller_without_session"};
    if(p_->isWatch)return {true,0,"watch_noop"};
    ChiakiControllerState c;chiaki_controller_state_set_idle(&c);
    struct Mapping{std::uint32_t semantic;ChiakiControllerButton target;};
    static constexpr Mapping buttons[]={
        {ControllerState::Cross,CHIAKI_CONTROLLER_BUTTON_CROSS},{ControllerState::Circle,CHIAKI_CONTROLLER_BUTTON_MOON},
        {ControllerState::Square,CHIAKI_CONTROLLER_BUTTON_BOX},{ControllerState::Triangle,CHIAKI_CONTROLLER_BUTTON_PYRAMID},
        {ControllerState::Left,CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT},{ControllerState::Right,CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
        {ControllerState::Up,CHIAKI_CONTROLLER_BUTTON_DPAD_UP},{ControllerState::Down,CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
        {ControllerState::L1,CHIAKI_CONTROLLER_BUTTON_L1},{ControllerState::R1,CHIAKI_CONTROLLER_BUTTON_R1},
        {ControllerState::L3,CHIAKI_CONTROLLER_BUTTON_L3},{ControllerState::R3,CHIAKI_CONTROLLER_BUTTON_R3},
        {ControllerState::Options,CHIAKI_CONTROLLER_BUTTON_OPTIONS},{ControllerState::Share,CHIAKI_CONTROLLER_BUTTON_SHARE},
        {ControllerState::Touchpad,CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},{ControllerState::PS,CHIAKI_CONTROLLER_BUTTON_PS}};
    for(auto b:buttons)if(state.buttons&b.semantic)c.buttons|=static_cast<std::uint32_t>(b.target);
    for(size_t i=0;i<state.touches.size();++i){c.touches[i].id=state.touches[i].id;c.touches[i].x=state.touches[i].x;c.touches[i].y=state.touches[i].y;}
    const auto applyMotion=[&](const std::array<float,3>& gyro,const std::array<float,3>& accel,uint64_t timestamp){
        if(!timestamp||!std::all_of(gyro.begin(),gyro.end(),[](float v){return std::isfinite(v);})||!std::all_of(accel.begin(),accel.end(),[](float v){return std::isfinite(v);}))return;
        // Repeated state snapshots contain the same batch; never integrate twice.
        if(p_->lastMotion&&timestamp<=p_->lastMotion)return;
        if(!p_->lastMotion||timestamp-p_->lastMotion>100000){chiaki_orientation_tracker_init(&p_->orientation);chiaki_accel_new_zero_set_inactive(&p_->accelZero,false);}
        chiaki_orientation_tracker_update(&p_->orientation,gyro[0],gyro[1],gyro[2],accel[0],accel[1],accel[2],&p_->accelZero,false,uint32_t(timestamp));p_->lastMotion=timestamp;
    };
    if(state.motionValid){
        for(size_t i=0;i<std::min(size_t(state.motionSampleCount),state.motionSamples.size());++i){const auto& sample=state.motionSamples[i];applyMotion(sample.gyro,sample.accel,sample.timestampUs);}
        applyMotion(state.gyro,state.accel,state.motionTimestampUs);
        chiaki_orientation_tracker_apply_to_controller_state(&p_->orientation,&c);
    }else p_->lastMotion=0;
    c.l2_state=state.l2;c.r2_state=state.r2;c.left_x=state.leftX;c.left_y=state.leftY;c.right_x=state.rightX;c.right_y=state.rightY;
    return p_->record(chiaki_session_set_controller_state(&p_->session,&c),"chiaki_session_set_controller_state");
}
ControllerFeedback ChiakiBackend::takeFeedback(){
    if(!p_||!p_->onOwner()||p_->isWatch)return {};
    std::lock_guard lock(p_->feedbackMutex);auto result=std::move(p_->feedback);p_->feedback={};
    if(result.motionReset)p_->lastMotion=0;
    return result;
}
std::string ChiakiBackend::watchEndReason()const{
    if(!p_)return lastWatchEndReason_;
    std::lock_guard lock(p_->watchReasonMutex);
    return p_->watchEndReason.empty()?lastWatchEndReason_:p_->watchEndReason;
}
NativeSnapshot ChiakiBackend::snapshot()const{
    // Like start/stop, pointer ownership requires the owner; the atomic counters
    // themselves may be updated by callbacks. Do not call concurrently with reset.
    if(!p_)return {};
    auto& s=*p_;
    NativeSnapshot out{s.started.load(),s.connected.load(),s.warnings.load(),s.errors.load(),s.videoCallbacks.load(),s.audioCallbacks.load(),s.quitReason.load(),s.apiError.load()};
    out.callbackRejected=s.callbackRejected;out.transportErrors=s.transportErrors;out.assemblyErrors=s.assemblyErrors;
    out.serverTargetBitrate=s.serverTargetBitrate;out.qualityReports=s.qualityReports;
    // Upstream packet window may reset itself; do not label this cumulative UDP.
    if(s.initialized){
        chiaki_packet_stats_get(&s.session.stream_connection.packet_stats,false,&out.packetReceived,&out.packetLost);
        // Read as the old Oviora read them: plain fields the stream thread keeps up to date.
        const auto& sc=s.session.stream_connection;
        const double loss=sc.congestion_control.packet_loss;out.packetLoss=std::isfinite(loss)?std::clamp(loss,0.0,1.0):0.0;
        const double mbps=sc.measured_bitrate;out.measuredMbps=std::isfinite(mbps)&&mbps>=0?mbps:0.0;
        // The console reports its round trip in microseconds (197 on a LAN with a 0.5 ms ping).
        const double rtt=sc.quality_rtt;out.consoleRttMs=sc.quality_reports&&std::isfinite(rtt)&&rtt>0?rtt/1000.0:-1.0;
    }
    if(s.isWatch){
        out.automaticRetryAllowed=false;out.startupRetryAllowed=false;
        return out;
    }
    // After a stopped session the PS5 may briefly still report RP_IN_USE.
    // Retrying this request cannot evict it; the console remains authoritative.
    // A manual reconnect may race the console releasing our previous session.
    // Retry RP_IN_USE within the same bounded budget; never force eviction.
    out.automaticRetryAllowed=out.lastQuitReason==CHIAKI_QUIT_REASON_NONE||out.lastQuitReason==CHIAKI_QUIT_REASON_CTRL_UNKNOWN||out.lastQuitReason==CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED||out.lastQuitReason==CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN||out.lastQuitReason==CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE;
    out.startupRetryAllowed=out.lastQuitReason==CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE;
    out.applicationReason=p_->appReason.load();
    return out;
}
} // namespace oviora::remoteplay
