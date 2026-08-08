#pragma once

struct EngineState;

// One-click "Build and Run XEX": builds the Xbox 360 game .xex, deploys the
// current project's content, and launches it in Xenia. The work runs in a
// detached process (build_run.ps1) whose output is tailed into the editor Log
// panel, so the UI never blocks.
namespace buildrun
{
    // Kick off the pipeline for the current project. Logs an error and does
    // nothing if a build is already running or the project/toolchain isn't ready.
    void Start(EngineState& state);

    // Build the .xex, deploy the content, and pack it into an Xbox 360 disc image
    // (XDVDFS/XGD2 .iso via the XDK's XDiscBld) that Xenia can mount. Shares the
    // same detached-process + log-tailing machinery as Start. Needs the XDK path.
    void StartIso(EngineState& state);

    // Call once per frame: forward new build output to the log and detect exit.
    void Poll(EngineState& state);

    bool Running();

    // Delete the Xbox build artifacts (Release/Debug output, obj, deploy folder,
    // disc images) from the configured build output base so the next build is from
    // scratch. No-op while a build is running.
    void Clean(EngineState& state);
}
