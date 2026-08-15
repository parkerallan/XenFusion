#pragma once

// The authored side of a facial performance: `.faceclip` on disk as JSON, and
// the cook that turns it into the frames both runtimes sample (anim/FaceClip.h).
//
// Frames are dense and quantised to bytes, matching what a capture produces and
// what the cooked format wants. Only shapes that actually move are stored, so a
// take that never opens the jaw carries no jaw column.
//
// Editor-only (nlohmann + std::filesystem); the shipped game plays cooked
// frames.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct FaceClipAsset
{
    int version = 1;
    std::string name;
    // The audio recorded alongside this take, project-relative. Playback starts
    // it and follows its position, so speech and lips cannot drift.
    std::string source_audio_path;
    // Nudges the clip against its audio when capture start is not sample-exact.
    int audio_offset_ms = 0;
    float fps = 30.0f;
    // ARKit shape index per column (see anim/FaceShapes.h).
    std::vector<uint8_t> shapes;
    // frames[frame][column], 255 = 1.0.
    std::vector<std::vector<uint8_t>> frames;

    float Duration() const
    { return frames.size() > 1 ? (float)(frames.size() - 1) / fps : 0.0f; }
    bool Valid() const
    { return !shapes.empty() && frames.size() > 1; }
};

namespace faceclip
{
    bool Load(const std::filesystem::path& path, FaceClipAsset& out, std::string& error);
    bool Save(const std::filesystem::path& path, const FaceClipAsset& asset, std::string& error);

    // Emits the 'FCL1' payload. The SAME bytes feed the pak and the engine's own
    // playback, so a performance cannot look one way in the viewport and another
    // on console.
    bool CookBE(const FaceClipAsset& asset, std::vector<unsigned char>& payload,
                std::string& error);

    // Drops columns that never leave zero and builds the shape table. Used by
    // the recorder before saving a take.
    void Compact(FaceClipAsset& asset);
}
