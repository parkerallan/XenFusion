#include "anim/FaceClipAsset.h"

#include "anim/FaceClip.h"
#include "anim/FaceShapes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

using nlohmann::json;

namespace faceclip
{
    namespace
    {
        void PushU32BE(std::vector<unsigned char>& out, unsigned int value)
        {
            out.push_back((unsigned char)((value >> 24) & 0xFF));
            out.push_back((unsigned char)((value >> 16) & 0xFF));
            out.push_back((unsigned char)((value >> 8) & 0xFF));
            out.push_back((unsigned char)(value & 0xFF));
        }
    }

    void Compact(FaceClipAsset& asset)
    {
        if (asset.shapes.empty() || asset.frames.empty())
            return;
        const std::size_t columns = asset.shapes.size();
        std::vector<std::size_t> keep;
        for (std::size_t c = 0; c < columns; ++c)
        {
            for (const std::vector<uint8_t>& frame : asset.frames)
                if (c < frame.size() && frame[c] != 0) { keep.push_back(c); break; }
        }
        if (keep.size() == columns)
            return;

        std::vector<uint8_t> shapes;
        for (std::size_t c : keep) shapes.push_back(asset.shapes[c]);
        for (std::vector<uint8_t>& frame : asset.frames)
        {
            std::vector<uint8_t> trimmed;
            trimmed.reserve(keep.size());
            for (std::size_t c : keep) trimmed.push_back(c < frame.size() ? frame[c] : 0);
            frame.swap(trimmed);
        }
        asset.shapes.swap(shapes);
    }

    bool Load(const std::filesystem::path& path, FaceClipAsset& out, std::string& error)
    {
        error.clear();
        std::ifstream input(path);
        if (!input)
        {
            error = "cannot open " + path.string();
            return false;
        }

        json document;
        try { input >> document; }
        catch (const std::exception& parse_error)
        {
            error = std::string("invalid JSON: ") + parse_error.what();
            return false;
        }
        if (!document.is_object())
        {
            error = "face clip must be a JSON object";
            return false;
        }

        out = FaceClipAsset();
        out.version = document.value("version", 1);
        out.name = document.value("name", path.stem().string());
        out.source_audio_path = document.value("source_audio", std::string());
        out.audio_offset_ms = document.value("audio_offset_ms", 0);
        out.fps = document.value("fps", 30.0f);
        if (out.fps <= 0.0f) out.fps = 30.0f;

        for (const json& entry : document.value("shapes", json::array()))
        {
            int shape = -1;
            if (entry.is_number_integer()) shape = entry.get<int>();
            else if (entry.is_string())    shape = face::ShapeIndex(entry.get<std::string>().c_str());
            if (shape < 0 || shape >= face::kShapeCount)
            {
                error = "face clip names a shape that is not an ARKit blendshape";
                return false;
            }
            out.shapes.push_back((uint8_t)shape);
        }
        if (out.shapes.empty())
        {
            error = "face clip has no shapes";
            return false;
        }

        for (const json& entry : document.value("frames", json::array()))
        {
            if (!entry.is_array() || entry.size() != out.shapes.size())
            {
                error = "a frame does not match the shape count";
                return false;
            }
            std::vector<uint8_t> frame;
            frame.reserve(out.shapes.size());
            for (const json& value : entry)
                frame.push_back((uint8_t)std::clamp(value.get<int>(), 0, 255));
            out.frames.push_back(std::move(frame));
        }
        if (out.frames.size() < 2)
        {
            error = "face clip needs at least two frames";
            return false;
        }
        return true;
    }

    bool Save(const std::filesystem::path& path, const FaceClipAsset& asset, std::string& error)
    {
        error.clear();
        if (!asset.Valid())
        {
            error = "face clip has nothing to save";
            return false;
        }

        json document;
        document["version"] = asset.version;
        document["name"] = asset.name;
        document["source_audio"] = asset.source_audio_path;
        document["audio_offset_ms"] = asset.audio_offset_ms;
        document["fps"] = asset.fps;
        // Shapes by name: a recording outlives any index table, and a diff of
        // two takes should be readable.
        json shapes = json::array();
        for (uint8_t shape : asset.shapes) shapes.push_back(face::ShapeName(shape));
        document["shapes"] = std::move(shapes);
        json frames = json::array();
        for (const std::vector<uint8_t>& frame : asset.frames)
            frames.push_back(json(frame));
        document["frames"] = std::move(frames);

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream output(path, std::ios::trunc);
        if (!output)
        {
            error = "cannot write " + path.string();
            return false;
        }
        // One frame per line: readable without being 300 KB of pretty-printed
        // single integers.
        output << document.dump();
        if (!output)
        {
            error = "write failed for " + path.string();
            return false;
        }
        return true;
    }

    bool CookBE(const FaceClipAsset& asset, std::vector<unsigned char>& payload,
                std::string& error)
    {
        error.clear();
        payload.clear();
        if (!asset.Valid())
        {
            error = "face clip has no frames";
            return false;
        }
        if (asset.frames.size() > face::kClipMaxFrames)
        {
            error = "face clip is too long to cook";
            return false;
        }
        if (asset.source_audio_path.size() > face::kClipMaxAudioPath)
        {
            error = "source audio path is too long to cook";
            return false;
        }

        const unsigned int frame_count = (unsigned int)asset.frames.size();
        const unsigned int target_count = (unsigned int)asset.shapes.size();
        PushU32BE(payload, face::kClipMagic);
        PushU32BE(payload, face::kClipVersion);
        PushU32BE(payload, frame_count);
        PushU32BE(payload, target_count);
        PushU32BE(payload, (unsigned int)std::lround((double)asset.fps * 1000.0));
        PushU32BE(payload, (unsigned int)asset.audio_offset_ms);
        PushU32BE(payload, (unsigned int)asset.source_audio_path.size());

        payload.insert(payload.end(), asset.shapes.begin(), asset.shapes.end());
        while ((payload.size() - face::kClipHeaderBytes) % 4 != 0)
            payload.push_back(0);

        payload.reserve(payload.size() + (std::size_t)frame_count * target_count);
        for (const std::vector<uint8_t>& frame : asset.frames)
            payload.insert(payload.end(), frame.begin(), frame.end());

        if (!asset.source_audio_path.empty())
        {
            while (payload.size() % 4 != 0)
                payload.push_back(0);
            payload.insert(payload.end(), asset.source_audio_path.begin(),
                           asset.source_audio_path.end());
        }
        return true;
    }
}
