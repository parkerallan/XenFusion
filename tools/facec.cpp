// Cooks a .faceclip (recorded ARKit weights, JSON) into the big-endian 'FCL1'
// payload the console samples. Mirrors animc: deploy.ps1 runs it per clip and
// hands the output to spakc under the clip's logical path, so nlohmann stays
// host-only.

#include "anim/FaceClip.h"
#include "anim/FaceClipAsset.h"

#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: facec <clip.faceclip> <output.fcl>\n";
        return 2;
    }

    FaceClipAsset asset;
    std::string error;
    if (!faceclip::Load(argv[1], asset, error))
    { std::cerr << "facec: " << error << '\n'; return 1; }

    std::vector<unsigned char> payload;
    if (!faceclip::CookBE(asset, payload, error))
    { std::cerr << "facec: " << error << '\n'; return 1; }

    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(payload.data()), (std::streamsize)payload.size());
    if (!output) { std::cerr << "facec: cannot write output\n"; return 1; }

    std::cout << "facec: wrote " << payload.size() << " bytes ("
              << asset.frames.size() << " frames, " << asset.shapes.size()
              << " shapes, " << asset.Duration() << "s)\n";
    return 0;
}
