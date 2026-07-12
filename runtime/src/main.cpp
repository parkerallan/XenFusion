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

    // Vsync (PresentationInterval ONE) paces us to ~60fps; use a fixed timestep
    // for the animated custom shaders (gTime) rather than a wall clock.
    const float dt = 1.0f / 60.0f;
    for (;;)
    {
        if (renderer.BeginFrame())
        {
            scene.Render(dt);
            renderer.EndFrame();
        }
    }

    // Unreachable in the title loop, but keep the teardown honest.
    // scene.Shutdown();
    // renderer.Shutdown();
}
