#pragma once

#include <string>
#include <vector>

// One name -> field table for object attributes, shared by BOTH targets.
//
// The editor's ObjectAttribute (state/EngineState.h) and the runtime's
// RtAttribute (runtime/src/SceneData.h) are separate structs — the editor's is
// C++11 with default member initializers, the runtime's is strict C++03 — but
// their members are named identically. That is what lets one X-macro list serve
// both: each target compiles its own two-line expansion of XF_ATTR_FIELDS
// (src/scene/AttrFieldsEditor.cpp, runtime/src/AttrFieldsRt.cpp) with offsetof
// against its own struct, and everything below is shared verbatim.
//
// The names ARE the C++ member names, so `obj:set("light_intensity", 4)` needs no
// separate per-attribute binding table. They are deliberately NOT the scene-JSON
// keys (which are unprefixed — "intensity", "fov", "fontPath" — and would collide
// across attribute types in one flat namespace).
//
// Editor-only members (video_import_profile) and variable-length ones
// (cam_track_points) are deliberately absent — the first does not exist on the
// console, and the second is not a fixed-size value.
//
// Strict C++03: this header is compiled by the XDK toolset.

#define XF_ATTR_FIELDS(X)                                  \
    /* --- asset paths --- */                              \
    X("model_path",               Str,   model_path)               \
    X("shader_path",              Str,   shader_path)              \
    X("script_path",              Str,   script_path)              \
    X("cast_shadow",              Bool,  cast_shadow)              \
    /* --- Animator --- */                                 \
    X("animator_controller_path", Str,   animator_controller_path) \
    X("animator_initial_state",   Str,   animator_initial_state)   \
    X("animator_playback_speed",  Float, animator_playback_speed)  \
    X("animator_auto_play",       Bool,  animator_auto_play)       \
    /* --- Camera --- */                                   \
    X("cam_fov",                  Float, cam_fov)                  \
    X("cam_near",                 Float, cam_near)                 \
    X("cam_far",                  Float, cam_far)                  \
    X("cam_active",               Bool,  cam_active)               \
    X("cam_type",                 Int,   cam_type)                 \
    X("cam_follow_target",        Str,   cam_follow_target)        \
    X("cam_follow_offset",        Vec3,  cam_follow_offset)        \
    X("cam_follow_orbit",         Vec3,  cam_follow_orbit)         \
    X("cam_follow_rot_offset",    Vec3,  cam_follow_rot_offset)    \
    X("cam_follow_lock",          Bool,  cam_follow_lock)          \
    X("cam_follow_smoothing",     Float, cam_follow_smoothing)     \
    X("cam_track_speed",          Float, cam_track_speed)          \
    X("cam_track_accel",          Float, cam_track_accel)          \
    X("cam_track_rot_offset",     Vec3,  cam_track_rot_offset)     \
    /* --- lights (Directional / Point / Spot / Environment) --- */ \
    X("light_color",              Vec3,  light_color)              \
    X("light_intensity",          Float, light_intensity)          \
    X("light_range",              Float, light_range)              \
    X("light_inner_deg",          Float, light_inner_deg)          \
    X("light_outer_deg",          Float, light_outer_deg)          \
    X("light_mode",               Int,   light_mode)               \
    X("light_volumetric",         Bool,  light_volumetric)         \
    X("light_volumetric_intensity", Float, light_volumetric_intensity) \
    /* --- Rigid Body --- */                               \
    X("phys_kind",                Int,   phys_kind)                \
    X("phys_shape",               Int,   phys_shape)               \
    X("phys_size",                Vec3,  phys_size)                \
    X("phys_mass",                Float, phys_mass)                \
    X("phys_lin_damping",         Float, phys_lin_damping)         \
    X("phys_ang_damping",         Float, phys_ang_damping)         \
    X("phys_restitution",         Float, phys_restitution)         \
    X("phys_friction",            Float, phys_friction)            \
    X("phys_gravity",             Bool,  phys_gravity)             \
    X("phys_gravity_scale",       Float, phys_gravity_scale)       \
    X("phys_lock_rotation",       Bool3, phys_lock_rotation)       \
    /* --- Trigger Volume --- */                           \
    X("trig_shape",               Int,   trig_shape)               \
    X("trig_size",                Vec3,  trig_size)                \
    /* --- Image overlay --- */                            \
    X("image_path",               Str,   image_path)               \
    X("image_x",                  Float, image_x)                  \
    X("image_y",                  Float, image_y)                  \
    X("image_w",                  Float, image_w)                  \
    X("image_h",                  Float, image_h)                  \
    X("image_stretch",            Bool,  image_stretch)            \
    X("image_lock_aspect",        Bool,  image_lock_aspect)        \
    X("image_tint",               Vec3,  image_tint)               \
    X("image_alpha",              Float, image_alpha)              \
    X("image_priority",           Int,   image_priority)           \
    /* --- Color block --- */                              \
    X("color_x",                  Float, color_x)                  \
    X("color_y",                  Float, color_y)                  \
    X("color_w",                  Float, color_w)                  \
    X("color_h",                  Float, color_h)                  \
    X("color_stretch",            Bool,  color_stretch)            \
    X("color_lock_aspect",        Bool,  color_lock_aspect)        \
    X("color_rgb",                Vec3,  color_rgb)                \
    X("color_alpha",              Float, color_alpha)              \
    X("color_priority",           Int,   color_priority)           \
    /* --- Skybox --- */                                   \
    X("sky_path",                 Str,   sky_path)                 \
    X("sky_rotation",             Float, sky_rotation)             \
    /* --- Text overlay --- */                             \
    X("text_font_path",           Str,   text_font_path)           \
    X("text_value",               Str,   text_value)               \
    X("text_x",                   Float, text_x)                   \
    X("text_y",                   Float, text_y)                   \
    X("text_w",                   Float, text_w)                   \
    X("text_h",                   Float, text_h)                   \
    X("text_font_size",           Float, text_font_size)           \
    X("text_color",               Vec3,  text_color)               \
    X("text_alpha",               Float, text_alpha)               \
    X("text_lock_aspect",         Bool,  text_lock_aspect)         \
    X("text_priority",            Int,   text_priority)            \
    /* --- Video overlay --- */                            \
    X("video_path",               Str,   video_path)               \
    X("video_x",                  Float, video_x)                  \
    X("video_y",                  Float, video_y)                  \
    X("video_w",                  Float, video_w)                  \
    X("video_h",                  Float, video_h)                  \
    X("video_stretch",            Bool,  video_stretch)            \
    X("video_lock_aspect",        Bool,  video_lock_aspect)        \
    X("video_tint",               Vec3,  video_tint)               \
    X("video_alpha",              Float, video_alpha)              \
    X("video_priority",           Int,   video_priority)           \
    X("video_play_mode",          Int,   video_play_mode)          \
    X("video_volume",             Float, video_volume)             \
    X("video_muted",              Bool,  video_muted)              \
    /* --- Audio source --- */                             \
    X("audio_path",               Str,   audio_path)               \
    X("audio_play",               Int,   audio_play)               \
    X("audio_volume",             Float, audio_volume)             \
    X("audio_pitch",              Float, audio_pitch)              \
    X("audio_loop",               Bool,  audio_loop)               \
    X("audio_class",              Int,   audio_class)              \
    X("audio_priority",           Int,   audio_priority)           \
    X("audio_load_mode",          Int,   audio_load_mode)          \
    X("audio_spatial",            Bool,  audio_spatial)            \
    X("audio_min_dist",           Float, audio_min_dist)           \
    X("audio_max_dist",           Float, audio_max_dist)           \
    X("audio_doppler",            Float, audio_doppler)

namespace attrfields
{
    enum Kind
    {
        KindFloat = 0,
        KindInt,
        KindBool,
        KindStr,
        KindVec3,  // float[3]
        KindBool3  // bool[3]
    };

    struct FieldDesc
    {
        const char*  name;
        int          kind;   // Kind
        unsigned int offset; // byte offset into the target's attribute struct
    };

    // A field's value, whatever its kind. Scripts see floats/ints/bools as Lua
    // numbers and booleans, Vec3/Bool3 as three of them, and Str as a string.
    struct Value
    {
        int         kind;
        float       f[3];
        int         i;
        bool        b[3];
        std::string s;

        Value() : kind(KindFloat), i(0)
        {
            f[0] = f[1] = f[2] = 0.0f;
            b[0] = b[1] = b[2] = false;
        }
    };

    // The target's own table (each target links exactly one definition).
    const FieldDesc* Table(int& outCount);
    // Descriptor for `name`, or 0 when the field doesn't exist.
    const FieldDesc* Find(const char* name);

    // Read/write a field on an attribute struct. `attr` must point at the same
    // struct type the linked Table() was built against — the caller (each host's
    // AttrGet/AttrSet) is what guarantees that pairing.
    inline bool Get(const void* attr, const FieldDesc& field, Value& out)
    {
        if (!attr) return false;
        const char* base = (const char*)attr + field.offset;
        out.kind = field.kind;
        switch (field.kind)
        {
        case KindFloat: out.f[0] = *(const float*)base; break;
        case KindInt:   out.i    = *(const int*)base;   break;
        case KindBool:  out.b[0] = *(const bool*)base;  break;
        case KindStr:   out.s    = *(const std::string*)base; break;
        case KindVec3:
            for (int k = 0; k < 3; ++k) out.f[k] = ((const float*)base)[k];
            break;
        case KindBool3:
            for (int k = 0; k < 3; ++k) out.b[k] = ((const bool*)base)[k];
            break;
        default: return false;
        }
        return true;
    }

    inline bool Equal(const Value& a, const Value& b)
    {
        if (a.kind != b.kind) return false;
        switch (a.kind)
        {
        case KindFloat: return a.f[0] == b.f[0];
        case KindInt:   return a.i    == b.i;
        case KindBool:  return a.b[0] == b.b[0];
        case KindStr:   return a.s    == b.s;
        case KindVec3:
            return a.f[0] == b.f[0] && a.f[1] == b.f[1] && a.f[2] == b.f[2];
        case KindBool3:
            return a.b[0] == b.b[0] && a.b[1] == b.b[1] && a.b[2] == b.b[2];
        }
        return false;
    }

    // Which of the object's attributes a bare obj:get("light_intensity") means.
    //
    // The attribute struct is flat — every attribute type carries every field —
    // so "which attribute owns this field" cannot be answered from the field
    // alone. What can be answered is which attribute has a NON-DEFAULT value for
    // it, and that is almost always the one the author configured. Falls back to
    // the first attribute, so a freshly added light still reads its default.
    // An explicit attrIndex (>= 0) skips all of this.
    //
    // Templated because the editor and the runtime each pass their own attribute
    // struct; the logic itself is identical.
    template <typename Attr>
    int ResolveIndex(const std::vector<Attr>& attrs, int attrIndex, const FieldDesc& desc)
    {
        if (attrIndex >= 0)
            return attrIndex < (int)attrs.size() ? attrIndex : -1;
        if (attrs.empty()) return -1;

        const Attr defaults;
        Value base, mine;
        if (Get(&defaults, desc, base))
            for (size_t a = 0; a < attrs.size(); ++a)
                if (Get(&attrs[a], desc, mine) && !Equal(mine, base))
                    return (int)a;
        return 0;
    }

    inline bool Set(void* attr, const FieldDesc& field, const Value& in)
    {
        if (!attr) return false;
        char* base = (char*)attr + field.offset;
        switch (field.kind)
        {
        case KindFloat: *(float*)base = in.f[0]; break;
        case KindInt:   *(int*)base   = in.i;    break;
        case KindBool:  *(bool*)base  = in.b[0]; break;
        case KindStr:   *(std::string*)base = in.s; break;
        case KindVec3:
            for (int k = 0; k < 3; ++k) ((float*)base)[k] = in.f[k];
            break;
        case KindBool3:
            for (int k = 0; k < 3; ++k) ((bool*)base)[k] = in.b[k];
            break;
        default: return false;
        }
        return true;
    }
}
