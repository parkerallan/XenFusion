#include "video/VideoImport.h"

#include <sstream>

namespace video_import
{
    namespace
    {
        void ProfileSize(int profile, int& width, int& height)
        {
            switch (profile)
            {
            case Profile480p: width = 854;  height = 480; break;
            case Profile720p: width = 1280; height = 720; break;
            default:          width = 640;  height = 360; break;
            }
        }

        int ProfileQuantizer(int profile)
        {
            switch (profile)
            {
            case Profile480p: return 3;
            case Profile720p: return 2;
            default:          return 4;
            }
        }
    }

    const char* ProfileLabel(int profile)
    {
        switch (profile)
        {
        case Profile480p: return "480p";
        case Profile720p: return "720p";
        default:          return "360p";
        }
    }

    std::string BuildFfmpegCommand(const std::string& source,
                                   const std::string& destination,
                                   int profile)
    {
        int width = 0;
        int height = 0;
        ProfileSize(profile, width, height);
        const int quantizer = ProfileQuantizer(profile);

        std::ostringstream command;
        command << "ffmpeg -y -i \"" << source
            << "\" -f mpeg -c:v mpeg1video -q:v " << quantizer << " -vf \"scale="
                << width << ':' << height
                << ":force_original_aspect_ratio=decrease:force_divisible_by=2,format=yuv420p\" -r 30"
                << " -c:a mp2 -b:a 224k -ar 44100 -ac 2 \""
                << destination << '\"';
        return command.str();
    }
}