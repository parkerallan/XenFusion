#include "light/LightSelect.h"

#include <cassert>
#include <cmath>

namespace
{
    bool Near(float left, float right)
    {
        return std::fabs(left - right) < 1.0e-5f;
    }
}

int main()
{
    lsel::Aabb bounds = {{-10.0f, -1.0f, -1.0f}, {10.0f, 1.0f, 1.0f}};
    lsel::PointLight points[6] = {};
    for (int index = 0; index < 6; ++index)
    {
        points[index].position[0] = -9.0f + index * 4.0f;
        points[index].position[1] = 2.0f;
        points[index].color[0] = (float)(index + 1);
        points[index].invRangeSq = 1.0f / 16.0f;
    }
    points[0].position[0] = -100.0f;

    float pointPos[4][4];
    float pointCol[4][4];
    lsel::PackPoints(points, 6, bounds, pointPos, pointCol);
    assert(Near(pointCol[0][0], 6.0f));
    assert(Near(pointCol[1][0], 5.0f));
    assert(Near(pointCol[2][0], 4.0f));
    assert(Near(pointCol[3][0], 3.0f));

    lsel::SpotLight spots[2] = {};
    spots[0].position[2] = -2.0f;
    spots[0].direction[2] = 1.0f;
    spots[0].color[0] = 1.0f;
    spots[0].invRange = 0.1f;
    spots[0].innerCos = 0.95f;
    spots[0].outerCos = 0.8f;
    spots[1] = spots[0];
    spots[1].direction[2] = -1.0f;

    float spotPos[2][4];
    float spotDir[2][4];
    float spotCol[2][4];
    lsel::PackSpots(spots, 2, bounds, spotPos, spotDir, spotCol);
    assert(Near(spotDir[0][2], 1.0f));
    assert(Near(spotCol[1][0], 0.0f));
    assert(Near(spotDir[1][2], 1.0f));
    assert(Near(spotDir[1][3], 1.0f));

    const float world[16] = {
        2, 0, 0, 0,
        0, 3, 0, 0,
        0, 0, 4, 0,
        5, 6, 7, 1
    };
    const float localMin[3] = {-1, -1, -1};
    const float localMax[3] = { 1,  1,  1};
    lsel::Aabb transformed;
    lsel::TransformAabb(localMin, localMax, world, transformed);
    assert(Near(transformed.min[0], 3.0f) && Near(transformed.max[0], 7.0f));
    assert(Near(transformed.min[1], 3.0f) && Near(transformed.max[1], 9.0f));
    assert(Near(transformed.min[2], 3.0f) && Near(transformed.max[2], 11.0f));
    return 0;
}