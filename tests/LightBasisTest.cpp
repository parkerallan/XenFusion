#include "light/LightBasis.h"
#include "light/LightProbeGrid.h"

#include <cmath>
#include <iostream>

namespace
{
    bool Near(float a, float b, float tolerance = 1.0e-4f)
    {
        return std::fabs(a - b) <= tolerance;
    }
}

int main()
{
    lbasis::Accumulator environmentBasis;
    lbasis::Sample environment = {{0.35f, 0.35f, 0.35f}, {0.0f, 0.0f, 0.0f}, 1.0f};
    environmentBasis.Add(environment);
    float environmentColor[3], environmentDirection[4];
    lbasis::Encode(environmentBasis, environmentColor, environmentDirection);
    if (!Near(environmentColor[0], 0.35f) || !Near(environmentColor[1], 0.35f) ||
        !Near(environmentColor[2], 0.35f) || !Near(environmentDirection[3], 0.0f))
    {
        std::cerr << "uniform environment basis failed\n";
        return 1;
    }

    lbasis::Accumulator basis;
    lbasis::Sample key = {{2.0f, 1.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, 0.5f};
    basis.Add(key);

    float color[3], direction[4];
    lbasis::Encode(basis, color, direction);
    if (!Near(color[0], 1.0f) || !Near(color[1], 0.5f) || !Near(color[2], 0.25f) ||
        !Near(direction[0], 0.0f) || !Near(direction[1], 1.0f) ||
        !Near(direction[2], 0.0f) || !Near(direction[3], 1.0f))
    {
        std::cerr << "single-lobe basis encode failed\n";
        return 1;
    }

    lbasis::Sample opposite = {{2.0f, 1.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 0.5f};
    basis.Add(opposite);
    lbasis::Encode(basis, color, direction);
    if (!Near(color[0], 2.0f) || !Near(color[1], 1.0f) || !Near(color[2], 0.5f) ||
        !Near(direction[3], 0.0f))
    {
        std::cerr << "opposed-lobe directionality failed\n";
        return 1;
    }

    const float hdr[3] = {8.7f, 2.45f, 0.9f};
    float rgbm[4], decoded[3];
    lbasis::EncodeRgbm(hdr, rgbm);
    for (int channel = 0; channel < 4; ++channel)
        rgbm[channel] = std::floor(rgbm[channel] * 255.0f + 0.5f) / 255.0f;
    lbasis::DecodeRgbm(rgbm, decoded);
    if (!Near(decoded[0], hdr[0], 0.04f) || !Near(decoded[1], hdr[1], 0.04f) ||
        !Near(decoded[2], hdr[2], 0.04f))
    {
        std::cerr << "RGBM HDR round-trip failed\n";
        return 1;
    }

    lprobe::Grid grid;
    grid.spacing = 2.0f;
    grid.dimensions[0] = grid.dimensions[1] = grid.dimensions[2] = 2;
    grid.probes.resize(8);
    for (size_t probe = 0; probe < grid.probes.size(); ++probe)
    {
        grid.probes[probe].sh[0] = static_cast<float>(probe);
        grid.probes[probe].sh[1] = static_cast<float>(probe) * 2.0f;
        grid.probes[probe].sh[2] = static_cast<float>(probe) * 3.0f;
    }
    const float center[3] = {1.0f, 1.0f, 1.0f};
    float probeColor[4], probeDirection[4];
    lprobe::Sample(grid, center, probeColor, probeDirection);
    if (!Near(probeColor[0], 3.5f) || !Near(probeColor[1], 7.0f) ||
        !Near(probeColor[2], 10.5f) || !Near(probeColor[3], 1.0f) ||
        !Near(probeDirection[3], 0.0f))
    {
        std::cerr << "probe trilinear blend failed\n";
        return 1;
    }
    return 0;
}