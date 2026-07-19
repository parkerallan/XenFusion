#pragma once

// Shared camera resolver: Fixed / Follow / Track view math used by BOTH the
// editor's SceneRenderer and the 360 runtime's SceneRuntime, so a camera frames
// the scene identically on PC and console. Ported from the Vulkan engine's
// CameraController, adapted to this engine's conventions: left-handed,
// row-vector, camera local forward = +Z, world up = +Y, rotations matching
// ComposeWorld's Rot(x,y,z) order. The track curve is a clamped uniform cubic
// B-spline (the reference's comments say Catmull-Rom, but this is what it
// actually implements), sampled by arc length.
//
// Header-only and strict C++03 (the runtime builds under the XDK's VS2010
// toolset): no default member initializers, no enum class, no nullptr. Same
// rules as physics/PhysicsTypes.h.

#include <math.h>
#include <vector>

namespace camr
{
    // Camera behavior (mirrors the reference's SceneObjectCameraType).
    enum CamType { CamFixed = 0, CamFollow = 1, CamTrack = 2 };

    struct View
    {
        float pos[3];
        float fwd[3]; // world-space look direction (unit-ish)
        float up[3];

        View()
        {
            pos[0] = pos[1] = pos[2] = 0.0f;
            fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = 1.0f; // +Z forward
            up[0]  = 0.0f; up[1]  = 1.0f; up[2]  = 0.0f;
        }
    };

    // --- small vector helpers (float[3]) ---
    inline float Dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    inline float Len3(const float* a) { return sqrtf(Dot3(a, a)); }
    inline void  Norm3(float* v)
    {
        const float l = Len3(v);
        if (l <= 0.0001f) { v[0] = v[1] = v[2] = 0.0f; return; }
        v[0] /= l; v[1] /= l; v[2] /= l;
    }

    // Rotate v by Euler degrees in this engine's object-rotation order
    // (Rx, then Ry, then Rz — the row-vector ComposeWorld convention), so a
    // rotation offset authored on a camera behaves like object rotation.
    inline void RotateEulerXYZ(float* v, const float* deg)
    {
        const float d2r = 3.14159265f / 180.0f;
        const float rx = deg[0] * d2r, ry = deg[1] * d2r, rz = deg[2] * d2r;
        float cx = cosf(rx), sx = sinf(rx);
        float cy = cosf(ry), sy = sinf(ry);
        float cz = cosf(rz), sz = sinf(rz);
        // Row-vector forms of this engine's RotationX/Y/Z.
        float a0 = v[0], a1 = v[1]*cx + v[2]*sx,  a2 = -v[1]*sx + v[2]*cx;  // Rx
        float b0 = a0*cy - a2*sy, b1 = a1, b2 = a0*sy + a2*cy;              // Ry
        float c0 = b0*cz + b1*sz, c1 = -b0*sz + b1*cz, c2 = b2;             // Rz
        v[0] = c0; v[1] = c1; v[2] = c2;
    }

    // Repair a degenerate or forward-parallel up vector.
    inline void GuardUp(const float* fwd, float* up)
    {
        float f[3] = { fwd[0], fwd[1], fwd[2] };
        Norm3(f);
        float u[3] = { up[0], up[1], up[2] };
        Norm3(u);
        if (Len3(up) <= 0.0001f || fabsf(Dot3(f, u)) >= 0.999f)
        {
            up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
            if (fabsf(f[1]) >= 0.999f) { up[1] = 0.0f; up[2] = 1.0f; } // near-vertical
        }
    }

    // --- Fixed: the object's transform places and aims the camera -----------
    // Basis from the object's Euler rotation: forward = local +Z, up = local +Y.
    inline void ResolveFixed(const float* pos, const float* rotDeg, View& out)
    {
        out.pos[0] = pos[0]; out.pos[1] = pos[1]; out.pos[2] = pos[2];
        float fwd[3] = { 0.0f, 0.0f, 1.0f };
        float up[3]  = { 0.0f, 1.0f, 0.0f };
        RotateEulerXYZ(fwd, rotDeg);
        RotateEulerXYZ(up, rotDeg);
        if (Len3(fwd) <= 0.0001f) { fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = 1.0f; }
        GuardUp(fwd, up);
        out.fwd[0] = fwd[0]; out.fwd[1] = fwd[1]; out.fwd[2] = fwd[2];
        out.up[0]  = up[0];  out.up[1]  = up[1];  out.up[2]  = up[2];
    }

    // --- Follow: chase a target object's live world position ----------------
    // lock=false (orbit mode): position = target + offset orbited by yaw/pitch;
    //   orientation stays the camera object's own (it does NOT auto-aim).
    // lock=true: camera stays at its authored position + offset and auto-aims
    //   at the target; orbit is ignored.
    // rotOffsetDeg applies last in both modes. Mirrors the reference exactly.
    inline void ResolveFollow(const float* camPos, const float* camRotDeg,
                              const float* targetPos,
                              const float* offset, const float* orbitDeg,
                              const float* rotOffsetDeg, bool lock, View& out)
    {
        ResolveFixed(camPos, camRotDeg, out);

        if (lock)
        {
            out.pos[0] += offset[0]; out.pos[1] += offset[1]; out.pos[2] += offset[2];
            float fwd[3] = { targetPos[0] - out.pos[0], targetPos[1] - out.pos[1], targetPos[2] - out.pos[2] };
            Norm3(fwd);
            if (Len3(fwd) <= 0.0001f) { fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = 1.0f; }
            float up[3] = { 0.0f, 1.0f, 0.0f };
            GuardUp(fwd, up);
            out.fwd[0] = fwd[0]; out.fwd[1] = fwd[1]; out.fwd[2] = fwd[2];
            out.up[0]  = up[0];  out.up[1]  = up[1];  out.up[2]  = up[2];
        }
        else
        {
            const float d2r = 3.14159265f / 180.0f;
            const float yaw = orbitDeg[0] * d2r, pitch = orbitDeg[1] * d2r;
            float off[3] = { offset[0], offset[1], offset[2] };
            if (fabsf(orbitDeg[0]) > 0.0001f || fabsf(orbitDeg[1]) > 0.0001f)
            {
                // Yaw around world +Y (this engine's RotationY sense), then
                // pitch around the yawed local X axis (Rodrigues).
                const float cy = cosf(yaw), sy = sinf(yaw);
                const float cp = cosf(pitch), sp = sinf(pitch);
                float ay[3] = { off[0]*cy + off[2]*sy, off[1], -off[0]*sy + off[2]*cy };
                float axis[3] = { cy, 0.0f, -sy }; // world X through the same yaw
                const float d = Dot3(ay, axis);
                float cr[3] = { axis[1]*ay[2] - axis[2]*ay[1],
                                axis[2]*ay[0] - axis[0]*ay[2],
                                axis[0]*ay[1] - axis[1]*ay[0] };
                off[0] = ay[0]*cp + cr[0]*sp + axis[0]*d*(1.0f - cp);
                off[1] = ay[1]*cp + cr[1]*sp + axis[1]*d*(1.0f - cp);
                off[2] = ay[2]*cp + cr[2]*sp + axis[2]*d*(1.0f - cp);
            }
            out.pos[0] = targetPos[0] + off[0];
            out.pos[1] = targetPos[1] + off[1];
            out.pos[2] = targetPos[2] + off[2];
        }

        if (fabsf(rotOffsetDeg[0]) > 0.0001f || fabsf(rotOffsetDeg[1]) > 0.0001f ||
            fabsf(rotOffsetDeg[2]) > 0.0001f)
        {
            RotateEulerXYZ(out.fwd, rotOffsetDeg);
            RotateEulerXYZ(out.up, rotOffsetDeg);
            Norm3(out.fwd);
            if (Len3(out.fwd) <= 0.0001f) { out.fwd[0] = 0.0f; out.fwd[1] = 0.0f; out.fwd[2] = 1.0f; }
            GuardUp(out.fwd, out.up);
        }
    }

    // --- Track: a clamped uniform cubic B-spline through world-space points --
    // Points are a flat xyz array (count = number of points, floats = 3*count).
    // count+1 segments; endpoints clamped by index triplication. Ported verbatim
    // from the reference (handedness-agnostic world-space math).

    inline void TrackPoint(const float* pts, int count, int augIndex, float* out)
    {
        int i = augIndex - 2;
        if (i < 0) i = 0;
        if (i > count - 1) i = count - 1;
        out[0] = pts[i*3 + 0]; out[1] = pts[i*3 + 1]; out[2] = pts[i*3 + 2];
    }

    inline void BSpline(const float* b0, const float* b1, const float* b2, const float* b3,
                        float t, float* out)
    {
        const float t2 = t * t, t3 = t2 * t;
        for (int a = 0; a < 3; ++a)
            out[a] = (1.0f / 6.0f) * (
                (-b0[a] + 3.0f*b1[a] - 3.0f*b2[a] + b3[a]) * t3 +
                ( 3.0f*b0[a] - 6.0f*b1[a] + 3.0f*b2[a]) * t2 +
                (-3.0f*b0[a] + 3.0f*b2[a]) * t +
                ( b0[a] + 4.0f*b1[a] + b2[a]));
    }

    inline void BSplineDeriv(const float* b0, const float* b1, const float* b2, const float* b3,
                             float t, float* out)
    {
        const float t2 = t * t;
        for (int a = 0; a < 3; ++a)
            out[a] = 0.5f * (-b0[a] + 3.0f*b1[a] - 3.0f*b2[a] + b3[a]) * t2
                   + (b0[a] - 2.0f*b1[a] + b2[a]) * t
                   + 0.5f * (-b0[a] + b2[a]);
    }

    inline void EvalTrack(const float* pts, int count, float u, float* outPos, float* outTan)
    {
        const int segCount = count + 1;
        int seg = (int)floorf(u);
        if (seg < 0) seg = 0;
        if (seg > segCount - 1) seg = segCount - 1;
        const float t = u - (float)seg;
        float b0[3], b1[3], b2[3], b3[3];
        TrackPoint(pts, count, seg,     b0);
        TrackPoint(pts, count, seg + 1, b1);
        TrackPoint(pts, count, seg + 2, b2);
        TrackPoint(pts, count, seg + 3, b3);
        if (outPos) BSpline(b0, b1, b2, b3, t, outPos);
        if (outTan) BSplineDeriv(b0, b1, b2, b3, t, outTan);
    }

    // Arc-length sampling: 24 chords per segment, like the reference.
    const int kTrackSubdivisions = 24;

    inline float TrackTotalLength(const float* pts, int count)
    {
        if (count < 2) return 0.0f;
        const int segCount = count + 1;
        float total = 0.0f;
        float prev[3];
        EvalTrack(pts, count, 0.0f, prev, 0);
        for (int seg = 0; seg < segCount; ++seg)
            for (int step = 1; step <= kTrackSubdivisions; ++step)
            {
                const float u = (float)seg + (float)step / (float)kTrackSubdivisions;
                float cur[3];
                EvalTrack(pts, count, u, cur, 0);
                const float d[3] = { cur[0]-prev[0], cur[1]-prev[1], cur[2]-prev[2] };
                total += Len3(d);
                prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
            }
        return total;
    }

    // distance (clamped to [0, total length]) -> position + unit tangent.
    inline void SampleTrackAtDistance(const float* pts, int count, float distance,
                                      float* outPos, float* outTan)
    {
        outTan[0] = 0.0f; outTan[1] = 0.0f; outTan[2] = 1.0f;
        if (count < 2)
        {
            outPos[0] = count > 0 ? pts[0] : 0.0f;
            outPos[1] = count > 0 ? pts[1] : 0.0f;
            outPos[2] = count > 0 ? pts[2] : 0.0f;
            return;
        }
        // Walk the dense chord table, accumulating length until `distance`.
        const int segCount = count + 1;
        const float total = TrackTotalLength(pts, count);
        if (total <= 0.0001f)
        {
            EvalTrack(pts, count, 0.0f, outPos, outTan);
            Norm3(outTan);
            if (Len3(outTan) <= 0.0001f) { outTan[0] = 0.0f; outTan[1] = 0.0f; outTan[2] = 1.0f; }
            return;
        }
        float target = distance;
        if (target < 0.0f) target = 0.0f;
        if (target > total) target = total;

        float walked = 0.0f;
        float prevU = 0.0f;
        float prev[3];
        EvalTrack(pts, count, 0.0f, prev, 0);
        for (int seg = 0; seg < segCount; ++seg)
            for (int step = 1; step <= kTrackSubdivisions; ++step)
            {
                const float u = (float)seg + (float)step / (float)kTrackSubdivisions;
                float cur[3];
                EvalTrack(pts, count, u, cur, 0);
                const float d[3] = { cur[0]-prev[0], cur[1]-prev[1], cur[2]-prev[2] };
                const float chord = Len3(d);
                if (walked + chord >= target)
                {
                    const float lt = chord > 0.0001f ? (target - walked) / chord : 0.0f;
                    const float su = prevU + (u - prevU) * lt;
                    EvalTrack(pts, count, su, outPos, outTan);
                    Norm3(outTan);
                    if (Len3(outTan) <= 0.0001f) { outTan[0] = 0.0f; outTan[1] = 0.0f; outTan[2] = 1.0f; }
                    return;
                }
                walked += chord;
                prevU = u;
                prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
            }
        EvalTrack(pts, count, (float)segCount, outPos, outTan); // numeric tail
        Norm3(outTan);
    }

    // Track sample -> view: forward = tangent, then the Euler rotation offset;
    // up = world +Y (with vertical guard), rotated by the same offset.
    inline void ResolveTrack(const float* pts, int count, float distance,
                             const float* rotOffsetDeg, View& out)
    {
        float tan[3];
        SampleTrackAtDistance(pts, count, distance, out.pos, tan);
        const bool hasOffset = fabsf(rotOffsetDeg[0]) > 0.0001f ||
                               fabsf(rotOffsetDeg[1]) > 0.0001f ||
                               fabsf(rotOffsetDeg[2]) > 0.0001f;
        if (hasOffset)
        {
            RotateEulerXYZ(tan, rotOffsetDeg);
            Norm3(tan);
            if (Len3(tan) <= 0.0001f) { tan[0] = 0.0f; tan[1] = 0.0f; tan[2] = 1.0f; }
        }
        float up[3] = { 0.0f, 1.0f, 0.0f };
        if (fabsf(Dot3(tan, up)) >= 0.999f) { up[1] = 0.0f; up[2] = 1.0f; }
        if (hasOffset)
        {
            RotateEulerXYZ(up, rotOffsetDeg);
            GuardUp(tan, up);
        }
        out.fwd[0] = tan[0]; out.fwd[1] = tan[1]; out.fwd[2] = tan[2];
        out.up[0]  = up[0];  out.up[1]  = up[1];  out.up[2]  = up[2];
    }

    // --- per-frame state (owned by each renderer) ----------------------------

    // Advance a track camera: linear speed ramp toward cruise, distance clamps
    // at the path end (no loop / ping-pong), dt capped so a hitch can't teleport.
    inline void AdvanceTrack(float& distance, float& speed,
                             float cruise, float accel, float dt, float totalLen)
    {
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;
        if (cruise < 0.0f) cruise = 0.0f;
        if (accel > 0.0f)
        {
            speed += accel * dt;
            if (speed > cruise) speed = cruise;
        }
        else
            speed = cruise;
        distance += speed * dt;
        if (distance > totalLen) distance = totalLen;
    }

    // Exponential smoothing toward the freshly resolved view. `smoothing` is
    // the time constant tau in seconds (0 = snap). Position and orientation
    // both use the plain ease alpha = 1 - exp(-dt/tau).
    struct FollowSmooth
    {
        bool  active;
        float pos[3], fwd[3], up[3];

        FollowSmooth() { active = false; pos[0]=pos[1]=pos[2]=0.0f; fwd[0]=fwd[1]=0.0f; fwd[2]=1.0f; up[0]=up[2]=0.0f; up[1]=1.0f; }
        void Reset() { active = false; }
    };

    inline void SmoothFollow(View& v, FollowSmooth& s, float smoothing, float dt)
    {
        if (smoothing <= 0.0f) { s.active = false; return; }
        if (!s.active)
        {
            for (int i = 0; i < 3; ++i) { s.pos[i] = v.pos[i]; s.fwd[i] = v.fwd[i]; s.up[i] = v.up[i]; }
            s.active = true;
            return;
        }
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;
        const float tau = smoothing > 0.0001f ? smoothing : 0.0001f;
        const float alpha = 1.0f - expf(-dt / tau);
        for (int i = 0; i < 3; ++i)
        {
            s.pos[i] += (v.pos[i] - s.pos[i]) * alpha;
            s.fwd[i] += (v.fwd[i] - s.fwd[i]) * alpha;
            s.up[i]  += (v.up[i]  - s.up[i])  * alpha;
        }
        for (int i = 0; i < 3; ++i) { v.pos[i] = s.pos[i]; v.fwd[i] = s.fwd[i]; v.up[i] = s.up[i]; }
        Norm3(v.fwd);
        if (Len3(v.fwd) <= 0.0001f) { v.fwd[0] = 0.0f; v.fwd[1] = 0.0f; v.fwd[2] = 1.0f; }
        GuardUp(v.fwd, v.up);
    }
}
