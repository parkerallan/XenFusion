#ifndef VIDEO_VIDEOIMPORT_H
#define VIDEO_VIDEOIMPORT_H

#include <string>

namespace video_import
{
    enum Profile
    {
        Profile360p = 0,
        Profile480p = 1,
        Profile720p = 2
    };

    const char* ProfileLabel(int profile);
    std::string BuildFfmpegCommand(const std::string& source,
                                   const std::string& destination,
                                   int profile);
}

#endif