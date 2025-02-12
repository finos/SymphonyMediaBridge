#pragma once
#include <string>

namespace bridge
{

struct VideoCodecSpec
{
    bool vp8 : 1;
    bool h264 : 1;

    VideoCodecSpec() : vp8(0), h264(0) {}

    static VideoCodecSpec makeVp8()
    {
        VideoCodecSpec v;
        v.vp8 = 1;
        return v;
    }

    std::string toString() const
    {
        std::string s;
        if (vp8)
        {
            s += "vp8 ";
        }
        if (h264)
        {
            s += "h264 ";
        }

        return s;
    }
};
} // namespace bridge