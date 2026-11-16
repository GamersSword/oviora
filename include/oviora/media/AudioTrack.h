#pragma once
#include <string>

namespace oviora::media {
struct AudioTrack {
    int streamIndex = -1;
    std::string language, title, codec;
    unsigned channels = 0;
};
}
