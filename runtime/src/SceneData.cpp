#include "SceneData.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// A tiny, self-contained JSON parser. The editor writes its .scene / .proj files
// with nlohmann/json, but that (and std::filesystem) won't build under the XDK
// toolset, so the runtime parses the small, fixed schema itself. Handles the
// subset the editor emits: objects, arrays, strings (with simple escapes),
// numbers, booleans, null.
// ---------------------------------------------------------------------------
namespace
{
    struct JValue
    {
        enum Type { Null, Bool, Number, Str, Array, Object } type;
        bool                                        b;
        double                                      num;
        std::string                                 str;
        std::vector<JValue>                         arr;
        std::vector<std::pair<std::string, JValue> > obj;

        JValue() : type(Null), b(false), num(0.0) {}

        const JValue* Find(const char* key) const
        {
            for (size_t i = 0; i < obj.size(); ++i)
                if (obj[i].first == key)
                    return &obj[i].second;
            return NULL;
        }
    };

    struct Parser
    {
        const char* p;
        const char* end;

        void SkipWs()
        {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                ++p;
        }

        bool ParseString(std::string& out)
        {
            if (p >= end || *p != '"') return false;
            ++p;
            out.clear();
            while (p < end && *p != '"')
            {
                char c = *p++;
                if (c == '\\' && p < end)
                {
                    char e = *p++;
                    switch (e)
                    {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case '/': out.push_back('/');  break;
                        case '\\': out.push_back('\\'); break;
                        case '"': out.push_back('"');  break;
                        case 'u': // \uXXXX — keep ASCII, skip the 4 hex digits
                            if (p + 4 <= end) p += 4;
                            out.push_back('?');
                            break;
                        default: out.push_back(e); break;
                    }
                }
                else
                {
                    out.push_back(c);
                }
            }
            if (p < end && *p == '"') { ++p; return true; }
            return false;
        }

        bool ParseValue(JValue& v)
        {
            SkipWs();
            if (p >= end) return false;
            char c = *p;
            if (c == '"')
            {
                v.type = JValue::Str;
                return ParseString(v.str);
            }
            if (c == '{') return ParseObject(v);
            if (c == '[') return ParseArray(v);
            if (c == 't' || c == 'f')
            {
                v.type = JValue::Bool;
                if (end - p >= 4 && strncmp(p, "true", 4) == 0)  { v.b = true;  p += 4; return true; }
                if (end - p >= 5 && strncmp(p, "false", 5) == 0) { v.b = false; p += 5; return true; }
                return false;
            }
            if (c == 'n')
            {
                if (end - p >= 4 && strncmp(p, "null", 4) == 0) { v.type = JValue::Null; p += 4; return true; }
                return false;
            }
            // number
            {
                char buf[64];
                int n = 0;
                while (p < end && n < 63 &&
                       (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' ||
                        (*p >= '0' && *p <= '9')))
                {
                    buf[n++] = *p++;
                }
                if (n == 0) return false;
                buf[n] = '\0';
                v.type = JValue::Number;
                v.num  = atof(buf);
                return true;
            }
        }

        bool ParseArray(JValue& v)
        {
            v.type = JValue::Array;
            ++p; // '['
            SkipWs();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;)
            {
                JValue item;
                if (!ParseValue(item)) return false;
                v.arr.push_back(item);
                SkipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }

        bool ParseObject(JValue& v)
        {
            v.type = JValue::Object;
            ++p; // '{'
            SkipWs();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;)
            {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (p >= end || *p != ':') return false;
                ++p;
                JValue val;
                if (!ParseValue(val)) return false;
                v.obj.push_back(std::make_pair(key, val));
                SkipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
    };

    // Read an entire file into a string. Returns false if it can't be opened.
    bool ReadWholeFile(const std::string& path, std::string& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0) { fclose(f); return false; }
        out.resize((size_t)n);
        size_t got = n > 0 ? fread(&out[0], 1, (size_t)n, f) : 0;
        fclose(f);
        out.resize(got);
        return true;
    }

    float NumAt(const JValue* arr, size_t i, float dflt)
    {
        if (arr && arr->type == JValue::Array && i < arr->arr.size() &&
            arr->arr[i].type == JValue::Number)
            return (float)arr->arr[i].num;
        return dflt;
    }

    bool BoolAt(const JValue* arr, size_t i, bool dflt)
    {
        if (arr && arr->type == JValue::Array && i < arr->arr.size() &&
            arr->arr[i].type == JValue::Bool)
            return arr->arr[i].b;
        return dflt;
    }
}

namespace scenedata
{
    std::string ReadStartupScene(const std::string& proj_path)
    {
        std::string text;
        if (!ReadWholeFile(proj_path, text) || text.empty())
            return std::string();

        Parser ps;
        ps.p = text.c_str();
        ps.end = ps.p + text.size();
        JValue root;
        if (!ps.ParseValue(root) || root.type != JValue::Object)
            return std::string();

        const JValue* s = root.Find("startupScene");
        if (s && s->type == JValue::Str)
            return s->str;
        return std::string();
    }

    bool LoadScene(const std::string& scene_path, RtScene& out)
    {
        std::string text;
        if (!ReadWholeFile(scene_path, text) || text.empty())
            return false;

        Parser ps;
        ps.p = text.c_str();
        ps.end = ps.p + text.size();
        JValue root;
        if (!ps.ParseValue(root) || root.type != JValue::Object)
            return false;

        const JValue* name = root.Find("name");
        if (name && name->type == JValue::Str)
            out.name = name->str;

        const JValue* objs = root.Find("objects");
        if (!objs || objs->type != JValue::Array)
            return true; // valid but empty

        for (size_t i = 0; i < objs->arr.size(); ++i)
        {
            const JValue& jo = objs->arr[i];
            if (jo.type != JValue::Object)
                continue;

            RtObject o;
            const JValue* nm = jo.Find("name");
            if (nm && nm->type == JValue::Str) o.name = nm->str;

            const JValue* pos = jo.Find("position");
            const JValue* rot = jo.Find("rotation");
            const JValue* scl = jo.Find("scale");
            o.position[0] = NumAt(pos, 0, 0.0f); o.position[1] = NumAt(pos, 1, 0.0f); o.position[2] = NumAt(pos, 2, 0.0f);
            o.rotation[0] = NumAt(rot, 0, 0.0f); o.rotation[1] = NumAt(rot, 1, 0.0f); o.rotation[2] = NumAt(rot, 2, 0.0f);
            o.scale[0]    = NumAt(scl, 0, 1.0f); o.scale[1]    = NumAt(scl, 1, 1.0f); o.scale[2]    = NumAt(scl, 2, 1.0f);

            const JValue* vis = jo.Find("visible");
            o.visible = (vis && vis->type == JValue::Bool) ? vis->b : true;

            const JValue* attrs = jo.Find("attributes");
            if (attrs && attrs->type == JValue::Array)
            {
                for (size_t a = 0; a < attrs->arr.size(); ++a)
                {
                    const JValue& ja = attrs->arr[a];
                    if (ja.type != JValue::Object)
                        continue;
                    RtAttribute at;
                    const JValue* t = ja.Find("type");
                    const JValue* m = ja.Find("model_path");
                    const JValue* s = ja.Find("shader_path");
                    const JValue* sc = ja.Find("script_path");
                    const JValue* csh = ja.Find("castShadow");
                    if (t && t->type == JValue::Str) at.type = t->str;
                    if (m && m->type == JValue::Str) at.model_path = m->str;
                    if (s && s->type == JValue::Str) at.shader_path = s->str;
                    if (sc && sc->type == JValue::Str) at.script_path = sc->str;
                    if (csh && csh->type == JValue::Bool) at.cast_shadow = csh->b;
                    const JValue* fov = ja.Find("fov");
                    const JValue* zn  = ja.Find("near");
                    const JValue* zf  = ja.Find("far");
                    const JValue* act = ja.Find("active");
                    if (fov && fov->type == JValue::Number) at.cam_fov  = (float)fov->num;
                    if (zn  && zn->type  == JValue::Number) at.cam_near = (float)zn->num;
                    if (zf  && zf->type  == JValue::Number) at.cam_far  = (float)zf->num;
                    if (act && act->type == JValue::Bool)   at.cam_active = act->b;
                    const JValue* cty = ja.Find("camType");
                    const JValue* ftg = ja.Find("followTarget");
                    const JValue* flk = ja.Find("followLock");
                    const JValue* fsm = ja.Find("followSmoothing");
                    const JValue* tsp = ja.Find("trackSpeed");
                    const JValue* tac = ja.Find("trackAccel");
                    if (cty && cty->type == JValue::Number) at.cam_type = (int)cty->num;
                    if (ftg && ftg->type == JValue::Str)    at.cam_follow_target = ftg->str;
                    if (flk && flk->type == JValue::Bool)   at.cam_follow_lock = flk->b;
                    if (fsm && fsm->type == JValue::Number) at.cam_follow_smoothing = (float)fsm->num;
                    if (tsp && tsp->type == JValue::Number) at.cam_track_speed = (float)tsp->num;
                    if (tac && tac->type == JValue::Number) at.cam_track_accel = (float)tac->num;
                    const JValue* fof = ja.Find("followOffset");
                    const JValue* fob = ja.Find("followOrbit");
                    const JValue* fro = ja.Find("followRotOffset");
                    const JValue* tro = ja.Find("trackRotOffset");
                    for (int k = 0; k < 3; ++k)
                    {
                        at.cam_follow_offset[k]     = NumAt(fof, k, at.cam_follow_offset[k]);
                        at.cam_follow_orbit[k]      = NumAt(fob, k, at.cam_follow_orbit[k]);
                        at.cam_follow_rot_offset[k] = NumAt(fro, k, at.cam_follow_rot_offset[k]);
                        at.cam_track_rot_offset[k]  = NumAt(tro, k, at.cam_track_rot_offset[k]);
                    }
                    // trackPoints is an array of [x,y,z] arrays -> flat triplets.
                    const JValue* tpt = ja.Find("trackPoints");
                    if (tpt && tpt->type == JValue::Array)
                        for (size_t tp = 0; tp < tpt->arr.size(); ++tp)
                        {
                            const JValue& jp = tpt->arr[tp];
                            if (jp.type != JValue::Array || jp.arr.size() != 3)
                                continue;
                            for (int k = 0; k < 3; ++k)
                                at.cam_track_points.push_back(NumAt(&jp, k, 0.0f));
                        }
                    const JValue* col = ja.Find("color");
                    const JValue* lin = ja.Find("intensity");
                    const JValue* lmo = ja.Find("lightMode");
                    const JValue* rng = ja.Find("range");
                    const JValue* icn = ja.Find("innerCone");
                    const JValue* ocn = ja.Find("outerCone");
                    at.light_color[0] = NumAt(col, 0, 1.0f);
                    at.light_color[1] = NumAt(col, 1, 1.0f);
                    at.light_color[2] = NumAt(col, 2, 1.0f);
                    if (lin && lin->type == JValue::Number) at.light_intensity = (float)lin->num;
                    if (lmo && lmo->type == JValue::Number) at.light_mode      = (int)lmo->num;
                    if (rng && rng->type == JValue::Number) at.light_range     = (float)rng->num;
                    if (icn && icn->type == JValue::Number) at.light_inner_deg = (float)icn->num;
                    if (ocn && ocn->type == JValue::Number) at.light_outer_deg = (float)ocn->num;
                    const JValue* vol = ja.Find("volumetric");
                    const JValue* voi = ja.Find("volumetricIntensity");
                    if (vol && vol->type == JValue::Bool)   at.light_volumetric = vol->b;
                    if (voi && voi->type == JValue::Number) at.light_volumetric_intensity = (float)voi->num;

                    // "Rigid Body" / "Trigger Volume" physics fields.
                    const JValue* pk  = ja.Find("kind");
                    const JValue* psh = ja.Find("shape");
                    const JValue* psz = ja.Find("size");
                    const JValue* pm  = ja.Find("mass");
                    const JValue* pld = ja.Find("linDamping");
                    const JValue* pad = ja.Find("angDamping");
                    const JValue* pre = ja.Find("restitution");
                    const JValue* pfr = ja.Find("friction");
                    const JValue* pg  = ja.Find("gravity");
                    const JValue* pgs = ja.Find("gravityScale");
                    const JValue* plr = ja.Find("lockRotation");
                    if (at.type == "Rigid Body")
                    {
                        if (pk  && pk->type  == JValue::Number) at.phys_kind  = (int)pk->num;
                        if (psh && psh->type == JValue::Number) at.phys_shape = (int)psh->num;
                        at.phys_size[0] = NumAt(psz, 0, at.phys_size[0]);
                        at.phys_size[1] = NumAt(psz, 1, at.phys_size[1]);
                        at.phys_size[2] = NumAt(psz, 2, at.phys_size[2]);
                        if (pm  && pm->type  == JValue::Number) at.phys_mass        = (float)pm->num;
                        if (pld && pld->type == JValue::Number) at.phys_lin_damping = (float)pld->num;
                        if (pad && pad->type == JValue::Number) at.phys_ang_damping = (float)pad->num;
                        if (pre && pre->type == JValue::Number) at.phys_restitution = (float)pre->num;
                        if (pfr && pfr->type == JValue::Number) at.phys_friction    = (float)pfr->num;
                        if (pg  && pg->type  == JValue::Bool)   at.phys_gravity     = pg->b;
                        if (pgs && pgs->type == JValue::Number) at.phys_gravity_scale = (float)pgs->num;
                        for (int axis = 0; axis < 3; ++axis)
                            at.phys_lock_rotation[axis] = BoolAt(plr, axis, false);
                    }
                    else if (at.type == "Trigger Volume")
                    {
                        if (psh && psh->type == JValue::Number) at.trig_shape = (int)psh->num;
                        at.trig_size[0] = NumAt(psz, 0, at.trig_size[0]);
                        at.trig_size[1] = NumAt(psz, 1, at.trig_size[1]);
                        at.trig_size[2] = NumAt(psz, 2, at.trig_size[2]);
                    }
                    else if (at.type == "Animator")
                    {
                        const JValue* acp = ja.Find("controllerPath");
                        const JValue* ais = ja.Find("initialState");
                        const JValue* aps = ja.Find("playbackSpeed");
                        const JValue* aap = ja.Find("autoPlay");
                        if (acp && acp->type == JValue::Str)    at.animator_controller_path = acp->str;
                        if (ais && ais->type == JValue::Str)    at.animator_initial_state = ais->str;
                        if (aps && aps->type == JValue::Number) at.animator_playback_speed = (float)aps->num;
                        if (aap && aap->type == JValue::Bool)   at.animator_auto_play = aap->b;
                    }
                    else if (at.type == "Audio")
                    {
                        const JValue* app = ja.Find("audioPath");
                        const JValue* apm = ja.Find("playMode");
                        const JValue* avo = ja.Find("volume");
                        const JValue* api = ja.Find("pitch");
                        const JValue* alo = ja.Find("loop");
                        const JValue* acl = ja.Find("audioClass");
                        const JValue* apr = ja.Find("priority");
                        const JValue* alm = ja.Find("loadMode");
                        const JValue* asp = ja.Find("spatial");
                        const JValue* amn = ja.Find("minDist");
                        const JValue* amx = ja.Find("maxDist");
                        const JValue* ado = ja.Find("doppler");
                        if (app && app->type == JValue::Str)    at.audio_path = app->str;
                        if (apm && apm->type == JValue::Number) at.audio_play = (int)apm->num;
                        if (avo && avo->type == JValue::Number) at.audio_volume = (float)avo->num;
                        if (api && api->type == JValue::Number) at.audio_pitch = (float)api->num;
                        if (alo && alo->type == JValue::Bool)   at.audio_loop = alo->b;
                        if (acl && acl->type == JValue::Number) at.audio_class = (int)acl->num;
                        if (apr && apr->type == JValue::Number) at.audio_priority = (int)apr->num;
                        if (alm && alm->type == JValue::Number) at.audio_load_mode = (int)alm->num;
                        if (asp && asp->type == JValue::Bool)   at.audio_spatial = asp->b;
                        if (amn && amn->type == JValue::Number) at.audio_min_dist = (float)amn->num;
                        if (amx && amx->type == JValue::Number) at.audio_max_dist = (float)amx->num;
                        if (ado && ado->type == JValue::Number) at.audio_doppler = (float)ado->num;
                    }
                    else if (at.type == "Image")
                    {
                        const JValue* ipp = ja.Find("imagePath");
                        const JValue* ix  = ja.Find("x");
                        const JValue* iy  = ja.Find("y");
                        const JValue* iw  = ja.Find("w");
                        const JValue* ih  = ja.Find("h");
                        const JValue* ist = ja.Find("stretch");
                        const JValue* ila = ja.Find("lockAspect");
                        const JValue* iti = ja.Find("tint");
                        const JValue* ial = ja.Find("alpha");
                        const JValue* ipr = ja.Find("priority");
                        if (ipp && ipp->type == JValue::Str)    at.image_path = ipp->str;
                        if (ix  && ix->type  == JValue::Number) at.image_x = (float)ix->num;
                        if (iy  && iy->type  == JValue::Number) at.image_y = (float)iy->num;
                        if (iw  && iw->type  == JValue::Number) at.image_w = (float)iw->num;
                        if (ih  && ih->type  == JValue::Number) at.image_h = (float)ih->num;
                        if (ist && ist->type == JValue::Bool)   at.image_stretch = ist->b;
                        if (ila && ila->type == JValue::Bool)   at.image_lock_aspect = ila->b;
                        at.image_tint[0] = NumAt(iti, 0, 1.0f);
                        at.image_tint[1] = NumAt(iti, 1, 1.0f);
                        at.image_tint[2] = NumAt(iti, 2, 1.0f);
                        if (ial && ial->type == JValue::Number) at.image_alpha = (float)ial->num;
                        if (ipr && ipr->type == JValue::Number) at.image_priority = (int)ipr->num;
                    }
                    else if (at.type == "Skybox")
                    {
                        const JValue* skp = ja.Find("skyboxPath");
                        const JValue* skr = ja.Find("skyboxRotation");
                        if (skp && skp->type == JValue::Str)    at.sky_path = skp->str;
                        if (skr && skr->type == JValue::Number) at.sky_rotation = (float)skr->num;
                    }
                    else if (at.type == "Text")
                    {
                        const JValue* tfp = ja.Find("fontPath");
                        const JValue* txt = ja.Find("text");
                        const JValue* tx  = ja.Find("x");
                        const JValue* ty  = ja.Find("y");
                        const JValue* tw  = ja.Find("w");
                        const JValue* th  = ja.Find("h");
                        const JValue* tfs = ja.Find("fontSize");
                        const JValue* tco = ja.Find("color");
                        const JValue* tal = ja.Find("alpha");
                        const JValue* tla = ja.Find("lockAspect");
                        const JValue* tpr = ja.Find("priority");
                        if (tfp && tfp->type == JValue::Str)    at.text_font_path = tfp->str;
                        if (txt && txt->type == JValue::Str)    at.text_value = txt->str;
                        if (tx  && tx->type  == JValue::Number) at.text_x = (float)tx->num;
                        if (ty  && ty->type  == JValue::Number) at.text_y = (float)ty->num;
                        if (tw  && tw->type  == JValue::Number) at.text_w = (float)tw->num;
                        if (th  && th->type  == JValue::Number) at.text_h = (float)th->num;
                        if (tfs && tfs->type == JValue::Number) at.text_font_size = (float)tfs->num;
                        at.text_color[0] = NumAt(tco, 0, 1.0f);
                        at.text_color[1] = NumAt(tco, 1, 1.0f);
                        at.text_color[2] = NumAt(tco, 2, 1.0f);
                        if (tal && tal->type == JValue::Number) at.text_alpha = (float)tal->num;
                        if (tla && tla->type == JValue::Bool)   at.text_lock_aspect = tla->b;
                        if (tpr && tpr->type == JValue::Number) at.text_priority = (int)tpr->num;
                    }
                    else if (at.type == "Video")
                    {
                        const JValue* vpp = ja.Find("videoPath");
                        const JValue* vx  = ja.Find("x");
                        const JValue* vy  = ja.Find("y");
                        const JValue* vw  = ja.Find("w");
                        const JValue* vh  = ja.Find("h");
                        const JValue* vst = ja.Find("stretch");
                        const JValue* vla = ja.Find("lockAspect");
                        const JValue* vti = ja.Find("tint");
                        const JValue* val = ja.Find("alpha");
                        const JValue* vpr = ja.Find("priority");
                        const JValue* vpm = ja.Find("playMode");
                        const JValue* vvo = ja.Find("volume");
                        const JValue* vmu = ja.Find("muted");
                        if (vpp && vpp->type == JValue::Str)    at.video_path = vpp->str;
                        if (vx  && vx->type  == JValue::Number) at.video_x = (float)vx->num;
                        if (vy  && vy->type  == JValue::Number) at.video_y = (float)vy->num;
                        if (vw  && vw->type  == JValue::Number) at.video_w = (float)vw->num;
                        if (vh  && vh->type  == JValue::Number) at.video_h = (float)vh->num;
                        if (vst && vst->type == JValue::Bool)   at.video_stretch = vst->b;
                        if (vla && vla->type == JValue::Bool)   at.video_lock_aspect = vla->b;
                        at.video_tint[0] = NumAt(vti, 0, 1.0f);
                        at.video_tint[1] = NumAt(vti, 1, 1.0f);
                        at.video_tint[2] = NumAt(vti, 2, 1.0f);
                        if (val && val->type == JValue::Number) at.video_alpha = (float)val->num;
                        if (vpr && vpr->type == JValue::Number) at.video_priority = (int)vpr->num;
                        if (vpm && vpm->type == JValue::Number) at.video_play_mode = (int)vpm->num;
                        if (vvo && vvo->type == JValue::Number) at.video_volume = (float)vvo->num;
                        if (vmu && vmu->type == JValue::Bool)   at.video_muted = vmu->b;
                    }
                    o.attributes.push_back(at);
                }
            }

            out.objects.push_back(o);
        }
        return true;
    }
}
