#include "anim/AnimatorCooker.h"

#include "anim/AnimationClip.h"
#include "anim/AnimationCodec.h"
#include "anim/FaceShapes.h"
#include "AnimatorCookFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace
{
    void PushU32(std::vector<unsigned char>& output, unsigned int value)
    {
        output.push_back((unsigned char)(value >> 24));
        output.push_back((unsigned char)(value >> 16));
        output.push_back((unsigned char)(value >> 8));
        output.push_back((unsigned char)value);
    }
    void PushFloat(std::vector<unsigned char>& output, float value)
    {
        unsigned int bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        PushU32(output, bits);
    }
    std::string Trim(const std::string& value)
    {
        size_t first = 0;
        while (first < value.size() && std::isspace((unsigned char)value[first])) ++first;
        size_t last = value.size();
        while (last > first && std::isspace((unsigned char)value[last - 1])) --last;
        return value.substr(first, last - first);
    }
    bool ParseBool(const std::string& value, float& encoded)
    {
        if (value == "true" || value == "True" || value == "TRUE") { encoded = 1.0f; return true; }
        if (value == "false" || value == "False" || value == "FALSE") { encoded = 0.0f; return true; }
        return false;
    }
    bool ParseCondition(const std::string& condition, unsigned int& parameter_hash,
                        unsigned int& operation, float& value, unsigned int& flags)
    {
        parameter_hash = 0; operation = animcook::CondAlways; value = 0.0f; flags = 0;
        const std::string expression = Trim(condition);
        if (expression.empty()) return true;
        const char* operators[] = {"==", "!=", ">=", "<=", ">", "<"};
        const unsigned int opcodes[] = {animcook::CondEqual, animcook::CondNotEqual,
            animcook::CondGreaterEqual, animcook::CondLessEqual,
            animcook::CondGreater, animcook::CondLess};
        for (int index = 0; index < 6; ++index)
        {
            const size_t position = expression.find(operators[index]);
            if (position == std::string::npos) continue;
            const std::string name = Trim(expression.substr(0, position));
            const std::string right = Trim(expression.substr(position + std::strlen(operators[index])));
            if (name.empty() || right.empty()) return false;
            parameter_hash = animation::NameHash(name.c_str());
            operation = opcodes[index];
            if (ParseBool(right, value)) flags |= animcook::kConditionBoolLiteral;
            else
            {
                char* end = nullptr;
                value = std::strtof(right.c_str(), &end);
                if (end == right.c_str() || *end != '\0' || !std::isfinite(value)) return false;
            }
            return true;
        }
        std::string name = expression;
        operation = animcook::CondTruthy;
        if (name[0] == '!') { operation = animcook::CondFalsy; name = Trim(name.substr(1)); }
        if (name.empty()) return false;
        parameter_hash = animation::NameHash(name.c_str());
        return true;
    }
}

namespace animator
{
    bool CookControllerBE(const std::filesystem::path& project_root,
                          const AnimatorController& controller,
                          std::vector<unsigned char>& output, std::string& error)
    {
        if (!ValidateController(controller, error)) return false;

        struct CookedClip
        {
            unsigned int id_hash;
            unsigned int name_hash;
            std::vector<unsigned char> payload;
        };
        std::vector<CookedClip> clips;
        for (const AnimatorClipReference& reference : controller.clips)
        {
            AnimationClip clip;
            if (!animation::BakeClip(project_root / reference.source_model_path,
                                     reference.clip_name, 30.0f, clip, error))
                return false;
            CookedClip cooked;
            cooked.id_hash = animation::NameHash(reference.id.c_str());
            cooked.name_hash = animation::NameHash(reference.clip_name.c_str());
            if (!animation::EncodeClipBE(clip, cooked.payload, error)) return false;
            clips.push_back(std::move(cooked));
        }

        const unsigned int state_offset = animcook::kHeaderBytes;
        const unsigned int transition_offset = state_offset +
            (unsigned int)controller.states.size() * animcook::kStateBytes;
        const unsigned int clip_offset = transition_offset +
            (unsigned int)controller.transitions.size() * animcook::kTransitionBytes;
        const unsigned int modifier_offset = clip_offset +
            (unsigned int)clips.size() * animcook::kClipBytes;
        // Resolved to ARKit indices here so the console never compares a name.
        struct CookedPose
        {
            unsigned int name_hash;
            std::vector<std::pair<unsigned char, unsigned char>> targets;
        };
        std::vector<CookedPose> poses;
        for (const FaceExpressionPose& pose : controller.face.poses)
        {
            if (pose.name.empty()) continue;
            CookedPose cooked;
            cooked.name_hash = animation::NameHash(pose.name.c_str());
            for (const auto& [target, weight] : pose.weights)
            {
                const int shape = face::ShapeIndex(target.c_str());
                if (shape == face::kShapeNone) continue;
                const float clamped = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
                cooked.targets.push_back({(unsigned char)shape,
                                          (unsigned char)std::lround(clamped * 255.0f)});
            }
            poses.push_back(std::move(cooked));
        }
        if (poses.size() > animcook::kMaxPoses)
        {
            error = "controller has " + std::to_string(poses.size()) + " face poses; limit is " +
                    std::to_string(animcook::kMaxPoses);
            return false;
        }
        if (controller.face.clips.size() > animcook::kMaxFaceClips)
        {
            error = "controller references too many face clips";
            return false;
        }
        unsigned int pose_target_total = 0;
        for (const CookedPose& pose : poses)
            pose_target_total += (unsigned int)pose.targets.size();

        const unsigned int pose_offset = modifier_offset +
            (unsigned int)controller.bone_modifiers.size() * animcook::kModifierBytes;
        const unsigned int pose_target_offset = pose_offset +
            (unsigned int)poses.size() * animcook::kPoseBytes;
        const unsigned int face_clip_offset = pose_target_offset +
            pose_target_total * animcook::kPoseTargetBytes;
        const unsigned int string_offset = face_clip_offset +
            (unsigned int)controller.face.clips.size() * animcook::kFaceClipBytes;
        unsigned int string_bytes = 0;
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
            string_bytes += (unsigned int)modifier.bone_name.size();
        for (const std::string& clip : controller.face.clips)
            string_bytes += (unsigned int)clip.size();
        const unsigned int data_offset = string_offset + string_bytes;

        output.clear();
        PushU32(output, animcook::kMagic);
        PushU32(output, animcook::kVersion);
        PushU32(output, animation::NameHash(controller.default_state.c_str()));
        PushU32(output, (unsigned int)controller.states.size());
        PushU32(output, (unsigned int)controller.transitions.size());
        PushU32(output, (unsigned int)clips.size());
        PushU32(output, state_offset);
        PushU32(output, transition_offset);
        PushU32(output, clip_offset);
        PushU32(output, data_offset);
        PushU32(output, (unsigned int)controller.bone_modifiers.size());
        PushU32(output, modifier_offset);
        PushU32(output, string_offset);
        PushU32(output, string_bytes);
        PushU32(output, animation::NameHash(controller.face.default_pose.c_str()));
        PushU32(output, (unsigned int)poses.size());
        PushU32(output, pose_offset);
        PushU32(output, pose_target_offset);
        PushU32(output, (unsigned int)controller.face.clips.size());
        PushU32(output, face_clip_offset);

        for (const AnimatorStateDefinition& state : controller.states)
        {
            PushU32(output, animation::NameHash(state.name.c_str()));
            PushU32(output, animation::NameHash(state.clip_id.c_str()));
            PushFloat(output, state.playback_speed);
            PushU32(output, state.loop ? animcook::kStateLoop : 0);
            PushU32(output, 0);
        }
        for (const AnimatorTransitionDefinition& transition : controller.transitions)
        {
            unsigned int parameter_hash = 0, operation = 0, flags = 0;
            float condition_value = 0.0f;
            if (!ParseCondition(transition.condition, parameter_hash, operation,
                                condition_value, flags))
            {
                error = "unsupported transition condition: " + transition.condition;
                return false;
            }
            if (transition.has_exit_time) flags |= animcook::kTransitionHasExitTime;
            PushU32(output, animation::NameHash(transition.from_state.c_str()));
            PushU32(output, animation::NameHash(transition.to_state.c_str()));
            PushU32(output, parameter_hash);
            PushU32(output, operation);
            PushFloat(output, condition_value);
            PushFloat(output, transition.blend_duration);
            PushFloat(output, transition.exit_time);
            PushU32(output, flags);
            PushU32(output, 0);
        }
        unsigned int payload_offset = data_offset;
        for (const CookedClip& clip : clips)
        {
            PushU32(output, clip.id_hash);
            PushU32(output, clip.name_hash);
            PushU32(output, payload_offset);
            PushU32(output, (unsigned int)clip.payload.size());
            payload_offset += (unsigned int)clip.payload.size();
        }
        unsigned int bone_name_offset = 0;
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
        {
            PushU32(output, animation::NameHash(modifier.bone_name.c_str()));
            PushU32(output, modifier.type == AnimatorBoneModifierType::Collision
                ? animcook::ModifierCollision : animcook::ModifierPhysics);
            PushU32(output, modifier.affects_children ? animcook::kModifierAffectsChildren : 0);
            PushU32(output, 0u);
            PushFloat(output, modifier.strength);
            PushFloat(output, modifier.damping);
            PushFloat(output, modifier.stiffness);
            PushFloat(output, modifier.mass);
            PushFloat(output, modifier.drag);
            PushFloat(output, modifier.gravity_scale);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, modifier.gravity_dir[axis]);
            PushFloat(output, modifier.angle_limit_deg);
            PushFloat(output, modifier.radius);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, modifier.box_half_extents[axis]);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, modifier.box_center[axis]);
            PushU32(output, bone_name_offset);
            PushU32(output, (unsigned int)modifier.bone_name.size());
            bone_name_offset += (unsigned int)modifier.bone_name.size();
        }
        unsigned int pose_target_cursor = 0;
        for (const CookedPose& pose : poses)
        {
            PushU32(output, pose.name_hash);
            PushU32(output, pose_target_cursor);
            PushU32(output, (unsigned int)pose.targets.size());
            pose_target_cursor += (unsigned int)pose.targets.size();
        }
        for (const CookedPose& pose : poses)
            for (const auto& [shape, weight] : pose.targets)
            {
                output.push_back(shape);
                output.push_back(weight);
                output.push_back(0);
                output.push_back(0);
            }
        // Clip paths continue the same string table the bone names started.
        unsigned int clip_path_offset = 0;
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
            clip_path_offset += (unsigned int)modifier.bone_name.size();
        for (const std::string& clip : controller.face.clips)
        {
            // A script names a clip by its file stem; the path is the pak key.
            const std::string stem = std::filesystem::path(clip).stem().string();
            PushU32(output, animation::NameHash(stem.c_str()));
            PushU32(output, clip_path_offset);
            PushU32(output, (unsigned int)clip.size());
            clip_path_offset += (unsigned int)clip.size();
        }
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
            output.insert(output.end(), modifier.bone_name.begin(), modifier.bone_name.end());
        for (const std::string& clip : controller.face.clips)
            output.insert(output.end(), clip.begin(), clip.end());
        for (const CookedClip& clip : clips)
            output.insert(output.end(), clip.payload.begin(), clip.payload.end());
        return true;
    }
}