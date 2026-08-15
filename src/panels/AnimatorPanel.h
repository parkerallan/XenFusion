#pragma once

#include "anim/AnimatorController.h"
#include "anim/FaceClip.h"
#include "anim/FaceRecorder.h"
#include "anim/LiveLinkFace.h"
#include "audio/AudioPlayer.h"
#include "render/AnimatorPreviewRenderer.h"

#include <d3d9.h>

#include <filesystem>
#include <string>
#include <vector>

struct EngineState;

class AnimatorPanel
{
public:
    void Render(EngineState& state);

    // Preview-renderer lifecycle hooks, forwarded by the application loop next
    // to the ViewportPanel's (mirrors that panel's ownership of SceneRenderer).
    void InitPreview(IDirect3DDevice9* device) { preview_.Initialize(device); }
    void OnPreviewDeviceLost()                 { preview_.OnDeviceLost(); }
    void BeginPreviewFrame()                   { preview_.BeginFrame(); }
    void RenderPreviewGpuPass(float dt)        { preview_.RenderGpu(dt); }
    void ShutdownPreview()                     { preview_.Shutdown(); }

    // Live tracking weights to mirror onto the selected scene object, or null
    // when tracking is off, not previewing, or not aimed at the character.
    const float* LiveFaceWeights() const
    { return (live_link_on_character_ && live_link_preview_ && have_live_weights_)
             ? live_weights_ : nullptr; }

private:
    void RefreshControllers(const EngineState& state);
    void OpenController(const std::filesystem::path& path, EngineState& state);
    void NewController();
    bool SaveController(EngineState& state);
    bool ImportAnimationsFromModel(EngineState& state, const std::string& path);
    void RenderClips(EngineState& state);
    void RenderStates();
    void RenderTransitions();
    void RenderBoneModifiers(EngineState& state);
    void RenderFace(EngineState& state);
    void RenderFacePoses();
    // Drains the socket and feeds the recorder every frame the panel is
    // open -- an unselected tab does not render, and neither must stop.
    void TickTracking(EngineState& state);
    void RenderLiveLink(EngineState& state);
    void RenderRecord(EngineState& state);
    void RenderFaceClips(EngineState& state);
    void UpdateFacePreview();
    // Live weights when tracking is driving the face, else null.
    const float* LiveWeights();
    // Starts/stops a recorded face clip in the panel preview, audio included.
    bool PlayFaceClipPreview(EngineState& state, const std::string& relative_path);
    void StopFaceClipPreview();
    void TickFaceClipPreview(float dt);
    void RenderControllerEditor(EngineState& state);
    void RenderPreviewViewport(EngineState& state);

    std::vector<std::filesystem::path> controller_paths_;
    std::filesystem::path scanned_directory_;
    std::filesystem::path loaded_path_;
    AnimatorController controller_;
    bool loaded_ = false;
    bool dirty_ = false;
    char new_name_[128] = "NewAnimator";
    bool preview_playing_ = false;
    bool preview_mesh_ = true;
    bool preview_skeleton_ = true;
    bool preview_texture_ = true;
    int preview_clip_index_ = 0;
    float preview_time_ = 0.0f;

    int face_pose_preview_ = -1;   // -1 = preview nothing

    livelink::Receiver live_link_;
    bool  live_link_preview_ = true;
    bool  live_link_on_character_ = true;
    int   live_link_port_ = livelink::kDefaultPort;
    bool  calibrating_ = false;
    double calibrate_until_ = 0.0;
    float live_weights_[face::kShapeCount] = {};
    bool  have_live_weights_ = false;

    facerec::Recorder recorder_;
    std::vector<std::string> audio_devices_;
    int   audio_device_index_ = 0;
    bool  audio_devices_scanned_ = false;
    std::string audio_device_error_;
    char  take_name_[128] = "take01";

    // Recorded clip playing in the preview. The bytes are owned here because
    // the view points into them, and the clock comes from the audio when there
    // is any, so a long take cannot drift against its own voice.
    std::vector<unsigned char> face_clip_bytes_;
    face::ClipView face_clip_view_;
    std::string    face_clip_path_;
    std::string    face_clip_audio_abs_;
    bool  face_clip_playing_ = false;
    float face_clip_time_ = 0.0f;
    float face_clip_weights_[face::kShapeCount] = {};
    aud::AudioPlayer face_clip_audio_;

    AnimatorPreviewRenderer preview_;
};
