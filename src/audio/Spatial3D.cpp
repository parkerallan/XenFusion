#include "audio/Spatial3D.h"

#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h> // SPEAKER_ masks
#endif
#include <x3daudio.h>

#include <math.h>
#include <string.h>

namespace aud
{
    namespace
    {
        X3DAUDIO_HANDLE g_handle;
        bool g_init  = false;
        bool g_tried = false;

        void Normalize3(float* v, float fx, float fy, float fz)
        {
            const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-6f)
            {
                v[0] /= len; v[1] /= len; v[2] /= len;
            }
            else
            {
                v[0] = fx; v[1] = fy; v[2] = fz;
            }
        }
    }

    bool Spatial3D::Init()
    {
        if (g_init)
            return true;
        if (g_tried)
            return false;
        g_tried = true;
        // The XDK signature returns void; the Windows SDK one returns HRESULT.
#ifdef _XBOX
        X3DAudioInitialize(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
                           X3DAUDIO_SPEED_OF_SOUND, g_handle);
        g_init = true;
#else
        g_init = SUCCEEDED(X3DAudioInitialize(
            SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
            X3DAUDIO_SPEED_OF_SOUND, g_handle));
#endif
        return g_init;
    }

    void Spatial3D::Calculate(const ListenerState& listener,
                              const SpatialParams& emitter,
                              float gains[2], float* dopplerRatio)
    {
        gains[0] = gains[1] = 1.0f;
        if (dopplerRatio)
            *dopplerRatio = 1.0f;
        if (!Init())
            return; // 2D fallback: full gains, no doppler

        X3DAUDIO_LISTENER l;
        memset(&l, 0, sizeof(l));
        l.Position.x = listener.pos[0];
        l.Position.y = listener.pos[1];
        l.Position.z = listener.pos[2];
        l.OrientFront.x = listener.fwd[0];
        l.OrientFront.y = listener.fwd[1];
        l.OrientFront.z = listener.fwd[2];
        l.OrientTop.x = listener.up[0];
        l.OrientTop.y = listener.up[1];
        l.OrientTop.z = listener.up[2];
        l.Velocity.x = listener.vel[0];
        l.Velocity.y = listener.vel[1];
        l.Velocity.z = listener.vel[2];
        Normalize3(&l.OrientFront.x, 0.0f, 0.0f, 1.0f);
        Normalize3(&l.OrientTop.x,   0.0f, 1.0f, 0.0f);

        X3DAUDIO_EMITTER e;
        memset(&e, 0, sizeof(e));
        e.ChannelCount        = 1;
        e.CurveDistanceScaler = (emitter.minDist > 0.01f) ? emitter.minDist : 0.01f;
        e.DopplerScaler       = (emitter.doppler >= 0.0f) ? emitter.doppler : 0.0f;
        e.OrientFront.z = 1.0f;
        e.OrientTop.y   = 1.0f;
        e.Position.x = emitter.pos[0];
        e.Position.y = emitter.pos[1];
        e.Position.z = emitter.pos[2];
        e.Velocity.x = emitter.vel[0];
        e.Velocity.y = emitter.vel[1];
        e.Velocity.z = emitter.vel[2];

        float matrix[2] = { 0.0f, 0.0f };
        X3DAUDIO_DSP_SETTINGS dsp;
        memset(&dsp, 0, sizeof(dsp));
        dsp.SrcChannelCount     = 1;
        dsp.DstChannelCount     = 2;
        dsp.pMatrixCoefficients = matrix;

        X3DAudioCalculate(g_handle, &l, &e,
                          X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER,
                          &dsp);

        gains[0] = matrix[0];
        gains[1] = matrix[1];
        if (dopplerRatio)
            *dopplerRatio = (dsp.DopplerFactor > 0.0f) ? dsp.DopplerFactor : 1.0f;

        // The default inverse curve never reaches zero — the attribute's
        // maxDist is a hard cutoff (the Vulkan-style audible model).
        const float dx = emitter.pos[0] - listener.pos[0];
        const float dy = emitter.pos[1] - listener.pos[1];
        const float dz = emitter.pos[2] - listener.pos[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (emitter.maxDist > 0.0f && dist > emitter.maxDist)
            gains[0] = gains[1] = 0.0f;
    }
}
