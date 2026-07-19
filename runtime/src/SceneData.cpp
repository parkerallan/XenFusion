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
                    if (t && t->type == JValue::Str) at.type = t->str;
                    if (m && m->type == JValue::Str) at.model_path = m->str;
                    if (s && s->type == JValue::Str) at.shader_path = s->str;
                    if (sc && sc->type == JValue::Str) at.script_path = sc->str;
                    const JValue* fov = ja.Find("fov");
                    const JValue* zn  = ja.Find("near");
                    const JValue* zf  = ja.Find("far");
                    const JValue* act = ja.Find("active");
                    if (fov && fov->type == JValue::Number) at.cam_fov  = (float)fov->num;
                    if (zn  && zn->type  == JValue::Number) at.cam_near = (float)zn->num;
                    if (zf  && zf->type  == JValue::Number) at.cam_far  = (float)zf->num;
                    if (act && act->type == JValue::Bool)   at.cam_active = act->b;
                    const JValue* col = ja.Find("color");
                    const JValue* lin = ja.Find("intensity");
                    const JValue* rng = ja.Find("range");
                    at.light_color[0] = NumAt(col, 0, 1.0f);
                    at.light_color[1] = NumAt(col, 1, 1.0f);
                    at.light_color[2] = NumAt(col, 2, 1.0f);
                    if (lin && lin->type == JValue::Number) at.light_intensity = (float)lin->num;
                    if (rng && rng->type == JValue::Number) at.light_range     = (float)rng->num;

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
                    }
                    else if (at.type == "Trigger Volume")
                    {
                        if (psh && psh->type == JValue::Number) at.trig_shape = (int)psh->num;
                        at.trig_size[0] = NumAt(psz, 0, at.trig_size[0]);
                        at.trig_size[1] = NumAt(psz, 1, at.trig_size[1]);
                        at.trig_size[2] = NumAt(psz, 2, at.trig_size[2]);
                    }
                    o.attributes.push_back(at);
                }
            }

            out.objects.push_back(o);
        }
        return true;
    }
}
