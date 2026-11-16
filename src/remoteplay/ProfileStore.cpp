#include "oviora/remoteplay/ProfileStore.h"
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
#include <iterator>
namespace oviora::remoteplay {
namespace {
std::filesystem::path userProfileDirectory(){
    PWSTR raw=nullptr;
    const auto hr=SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw);
    if(FAILED(hr))throw std::runtime_error("Cannot resolve Remote Play user data directory");
    std::filesystem::path result(raw);CoTaskMemFree(raw);
    return result/L"Oviora"/L"remoteplay";
}
std::filesystem::path executableDirectory(){
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    if(length==0||length>=std::size(path))return {};
    return std::filesystem::path(path).parent_path();
}
// Same walk as runtime::applicationRoot() (kept local: this library does not link oviora_base).
std::filesystem::path applicationDataDirectory(const std::filesystem::path& exe){
    std::error_code ec;auto candidate=exe;
    for(unsigned depth=0;depth!=6;++depth){
        if(std::filesystem::exists(candidate/L"runtime_local",ec))return candidate/L"runtime_local";
        const auto parent=candidate.parent_path();
        if(parent.empty()||parent==candidate)break;
        candidate=parent;
    }
    return exe/L"runtime_local";
}
// A test install is marked by test-kurulumu.txt next to the executable. Its PS5 pairings and
// stream settings live in its own runtime_local\remoteplay, so a test can never change the
// installed app's settings.ini. The installed app has no marker: its folder stays as it was.
std::filesystem::path testProfileDirectory(){
    const auto exe=executableDirectory();
    std::error_code ec;
    if(exe.empty()||!std::filesystem::is_regular_file(exe/L"test-kurulumu.txt",ec))return {};
    const auto own=applicationDataDirectory(exe)/L"remoteplay";
    const auto seeded=own/L".eslendi";
    if(!std::filesystem::exists(seeded,ec)){
        // Once: copy the user's pairings (DPAPI files of this same Windows user) and the last
        // chosen console, so tests still connect. Settings ([Stream] etc.) start fresh.
        std::filesystem::create_directories(own,ec);
        std::filesystem::path user;
        try{user=userProfileDirectory();}catch(...){}
        if(!user.empty()){
            for(std::filesystem::directory_iterator it(user,ec),end;!ec&&it!=end;it.increment(ec)){
                if(it->path().extension()!=L".dat")continue;
                std::error_code copyError;
                std::filesystem::copy_file(it->path(),own/it->path().filename(),std::filesystem::copy_options::skip_existing,copyError);
            }
            wchar_t last[80]{};
            GetPrivateProfileStringW(L"RemotePlay",L"LastProfile",L"",last,80,(user/L"settings.ini").c_str());
            if(last[0])WritePrivateProfileStringW(L"RemotePlay",L"LastProfile",last,(own/L"settings.ini").c_str());
        }
        std::ofstream mark(seeded,std::ios::binary);mark<<"1";
    }
    return own;
}
}
std::filesystem::path profileDirectory(){
    static const std::filesystem::path test=testProfileDirectory();
    if(!test.empty())return test;
    return userProfileDirectory();
}
bool profileDirectoryIsTestInstall(){
    return profileDirectory()!=userProfileDirectory();
}
ProfileMigration migrateProfiles(const std::filesystem::path& legacyRoot){
    return migrateProfilesTo(legacyRoot,profileDirectory());
}
ProfileMigration migrateProfilesTo(const std::filesystem::path& legacyRoot,const std::filesystem::path& destination){
    ProfileMigration report;
    auto migrate=[&](const std::filesystem::path& source){
        std::error_code ec;
        if(!std::filesystem::is_regular_file(source,ec))return;
        auto target=destination/source.filename();
        auto marker=target;marker+=L".migrated";
        if(std::filesystem::exists(marker,ec))return;
        if(std::filesystem::exists(target,ec)){
            // A previous import may have completed its atomic write before a crash.
            if(loadProfile(target)){std::ofstream migrated(marker,std::ios::binary);migrated<<"1";if(!migrated)++report.failed;}
            else ++report.failed;
            return; // Never overwrite a newer profile.
        }
        auto value=loadProfile(source);
        if(!value||!saveProfile(target,*value)||!loadProfile(target)){++report.failed;return;}
        std::ofstream migrated(marker,std::ios::binary);migrated<<"1";
        if(!migrated){++report.failed;return;}
        ++report.imported; // Keep the original: recovery must remain possible.
    };
    migrate(legacyRoot/L"remoteplay-profile.dat");
    std::error_code ec;unsigned scanned=0;
    for(std::filesystem::directory_iterator it(legacyRoot/L"remoteplay",ec),end;!ec&&it!=end&&scanned<256;it.increment(ec),++scanned){
        auto id=it->path().stem().wstring();
        if(it->path().extension()==L".dat"&&id.size()==32&&std::all_of(id.begin(),id.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))migrate(it->path());
    }
    return report;
}
namespace {
struct SecretBytes {std::vector<uint8_t> data;~SecretBytes(){SecureZeroMemory(data.data(),data.size());}};
struct ProtectedBlob {DATA_BLOB value{};~ProtectedBlob(){if(value.pbData){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}}};
void append32(std::vector<uint8_t>& b,uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(n>>(8*i)));}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
}
bool saveProfile(const std::filesystem::path& path,const NativeConnectRequest& request){
    if(!validHost(request.host)||request.video.validate())return false;
    if(!request.consoleId.empty()&&(request.consoleId.size()!=12||!std::all_of(request.consoleId.begin(),request.consoleId.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})))return false;
    SecretBytes plain;
    auto& b=plain.data;b.insert(b.end(),{'V','R','P',2});
    append32(b,uint32_t(request.host.size()));append32(b,request.video.width);append32(b,request.video.height);
    append32(b,request.video.fps);append32(b,request.video.bitrateKbps);append32(b,uint32_t(request.video.codec));
    b.insert(b.end(),request.credentials.accountId.begin(),request.credentials.accountId.end());
    b.insert(b.end(),request.credentials.registrationKey.begin(),request.credentials.registrationKey.end());
    b.insert(b.end(),request.credentials.sessionKey.begin(),request.credentials.sessionKey.end());
    b.insert(b.end(),request.host.begin(),request.host.end());
    b.push_back(request.viewOnly?1:0);
    b.push_back(uint8_t(request.consoleId.size()));
    b.insert(b.end(),request.consoleId.begin(),request.consoleId.end());
    DATA_BLOB input{DWORD(b.size()),b.data()};ProtectedBlob encrypted;
    if(!CryptProtectData(&input,L"Oviora PS5 pairing",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted.value))return false;
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);if(ec)return false;
    auto temp=path;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;bool ok=WriteFile(file,encrypted.value.pbData,encrypted.value.cbData,&written,nullptr)&&written==encrypted.value.cbData;
    if(ok)ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file);
    if(ok)ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok)DeleteFileW(temp.c_str());return ok;
}
std::optional<NativeConnectRequest> loadProfile(const std::filesystem::path& path,ProfileLoadError* error){
    if(error)*error=ProfileLoadError::Read;
    std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f){std::error_code ec;if(error&&!std::filesystem::exists(path,ec)&&!ec)*error=ProfileLoadError::Missing;return {};}
    const auto size=f.tellg();if(size<=0||size>8192)return {};
    std::vector<uint8_t> encrypted(static_cast<size_t>(size));f.seekg(0);if(!f.read(reinterpret_cast<char*>(encrypted.data()),size))return {};
    DATA_BLOB input{DWORD(encrypted.size()),encrypted.data()};ProtectedBlob plain;
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain.value)){if(error)*error=ProfileLoadError::Decrypt;return {};}
    if(error)*error=ProfileLoadError::Format;
    const auto* p=plain.value.pbData;const auto n=plain.value.cbData;
    constexpr size_t header=28,credentials=40;
    if(n<header+credentials||std::memcmp(p,"VRP",3)!=0||(p[3]!=1&&p[3]!=2))return {};
    const auto hostSize=read32(p+4);
    const size_t base=header+credentials+hostSize;
    if(hostSize>253||n<base||read32(p+24)>2)return {};
    if(p[3]==1&&n!=base)return {};
    if(p[3]==2&&(n<base+2||p[base]>1||(p[base+1]!=0&&p[base+1]!=12)||n!=base+2+p[base+1]))return {};
    NativeConnectRequest r;r.video.width=read32(p+8);r.video.height=read32(p+12);r.video.fps=read32(p+16);
    r.video.bitrateKbps=read32(p+20);r.video.codec=Codec(read32(p+24));
    std::memcpy(r.credentials.accountId.data(),p+header,8);std::memcpy(r.credentials.registrationKey.data(),p+header+8,16);
    std::memcpy(r.credentials.sessionKey.data(),p+header+24,16);
    r.host.assign(reinterpret_cast<const char*>(p+header+credentials),hostSize);
    if(p[3]==2){
        r.viewOnly=p[base]!=0;r.consoleId.assign(reinterpret_cast<const char*>(p+base+2),p[base+1]);
        if(!std::all_of(r.consoleId.begin(),r.consoleId.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    }
    if(!validHost(r.host)||r.video.validate())return {};
    if(error)*error=ProfileLoadError::None;return r;
}
}
