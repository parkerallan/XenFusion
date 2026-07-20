#include "XboxRenderer.h"
#include "SceneRuntime.h"

#include <xtl.h>

// Xbox 360 game entry point. This is the runtime that ships inside the title and
// runs on the console / Xenia: it brings up Direct3D, loads the scene the editor
// authored, and renders it from a fixed camera every frame. No editor, no ImGui,
// no window — the title owns the whole display.
//
// Content is deployed alongside default.xex and reached through "game:\":
//   game:\game.proj            (optional; startupScene, else scenes\Main.scene)
//   game:\scenes\*.scene
//   game:\assets\...           (meshes + textures)
//   game:\shaders\*.cso        (precompiled Xenos shaders) + *.dir (custom //@ state)

void __cdecl main()
{
    XboxRenderer renderer;
    if (!renderer.Init())
        return; // no device — nothing we can do headless

    // Match the editor's default viewport background.
    renderer.SetClearColor(0.094f, 0.094f, 0.106f);

    SceneRuntime scene;
    if (!scene.Init(renderer.Device(), "game:\\"))
    {
        renderer.Shutdown();
        return;
    }
    scene.InitBloom(renderer); // emissive glow post chain (optional shaders)

    // Real per-frame delta from the CPU timebase — the same wall-clock dt the
    // editor uses (QueryPerformanceCounter). We can't assume vsync paces us to
    // 60fps: Xenia (and an uncapped console) present faster, and a fixed 1/60
    // would then advance animated shaders (gTime) and the physics step faster
    // than real time. First frame / hitches clamp to 1/60.
    LARGE_INTEGER qpcFreq, qpcLast, qpcNow;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLast);
    for (;;)
    {
        // Metal reflections: capture the scene into the env cube map from the
        // metallic object's position. Outside the tiling bracket by design.
        scene.RenderEnvCapture();

        if (renderer.BeginFrame())
        {
            QueryPerformanceCounter(&qpcNow);
            float dt = (float)((double)(qpcNow.QuadPart - qpcLast.QuadPart) /
                               (double)qpcFreq.QuadPart);
            qpcLast = qpcNow;
            if (dt < 0.0f || dt > 0.25f)
                dt = 1.0f / 60.0f; // first frame / hitch

            scene.Render(dt);
            renderer.EndFrame();
        }
    }

    // Unreachable in the title loop, but keep the teardown honest.
    // scene.Shutdown();
    // renderer.Shutdown();
}
