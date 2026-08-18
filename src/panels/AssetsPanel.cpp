#include "panels/AssetsPanel.h"
#include "loc/Loc.h"

#include "core/Log.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    enum class AssetKind { Folder, Image, Audio, Video, Model, Shader, Text, Generic };

    AssetKind Classify(const fs::directory_entry& e)
    {
        std::error_code ec;
        if (e.is_directory(ec))
            return AssetKind::Folder;

        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);

        auto in = [&](std::initializer_list<const char*> list)
        {
            for (const char* s : list) if (ext == s) return true;
            return false;
        };

        if (in({".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga", ".gif"})) return AssetKind::Image;
        if (in({".wav", ".ogg", ".mp3", ".flac", ".mp2"}))                 return AssetKind::Audio;
        if (in({".mpg", ".mpeg", ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".wmv"})) return AssetKind::Video;
        if (in({".obj", ".fbx", ".mesh", ".gltf", ".glb", ".dae", ".3ds"})) return AssetKind::Model;
        if (in({".hlsl", ".fx", ".glsl", ".shader", ".vsh", ".fsh", ".vs", ".ps", ".cg"})) return AssetKind::Shader;
        if (in({".txt", ".lua", ".anim", ".json", ".md", ".ini", ".cfg", ".xml", ".csv", ".h", ".hpp", ".cpp", ".c", ".cs"})) return AssetKind::Text;
        return AssetKind::Generic;
    }

    void DrawGlyph(ImDrawList* dl, ImVec2 center, const char* glyph, ImU32 col, EngineState& state)
    {
        ImFont* f = state.large_icon_font;
        if (f)
        {
            const float fs = f->FontSize;
            const ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph);
            dl->AddText(f, fs, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, glyph);
        }
        else
        {
            const ImVec2 sz = ImGui::CalcTextSize(glyph);
            dl->AddText(ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, glyph);
        }
    }

    void DrawSphere(ImDrawList* dl, ImVec2 c)
    {
        const float r = 22.0f;
        dl->AddCircleFilled(c, r, IM_COL32(64, 84, 150, 255), 32);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.28f, c.y - r * 0.28f), r * 0.60f, IM_COL32(108, 138, 210, 255), 32);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.42f), r * 0.22f, IM_COL32(220, 232, 255, 255), 16);
        dl->AddCircle(c, r, IM_COL32(28, 36, 72, 255), 32, 1.5f);
    }

    void DrawVisual(ImDrawList* dl, ImVec2 center, AssetKind kind, EngineState& state)
    {
        switch (kind)
        {
        case AssetKind::Folder: DrawGlyph(dl, center, ICON_FA_FOLDER, IM_COL32(95, 155, 225, 255), state); break;
        case AssetKind::Image:  DrawGlyph(dl, center, ICON_FA_IMAGE,  IM_COL32(80, 185, 115, 255), state); break;
        case AssetKind::Audio:  DrawGlyph(dl, center, ICON_FA_MUSIC,  IM_COL32(175, 120, 205, 255), state); break;
        case AssetKind::Video:  DrawGlyph(dl, center, ICON_FA_FILM,   IM_COL32(215, 130, 95, 255),  state); break;
        case AssetKind::Model:  DrawGlyph(dl, center, ICON_FA_CUBE,   IM_COL32(205, 150, 80, 255),  state); break;
        case AssetKind::Shader: DrawSphere(dl, center); break;
        case AssetKind::Text:
        case AssetKind::Generic:
        default: DrawGlyph(dl, center, ICON_FA_FILE, ImGui::GetColorU32(ImGuiCol_Text), state); break;
        }
    }

    std::string Ellipsize(const std::string& s, float max_w)
    {
        if (ImGui::CalcTextSize(s.c_str()).x <= max_w)
            return s;
        std::string t = s;
        while (!t.empty() && ImGui::CalcTextSize((t + "..").c_str()).x > max_w)
            t.pop_back();
        return t + "..";
    }

    // --- Deferred filesystem action (applied after the grid is drawn) ---
    struct PendingOp
    {
        enum Type { None, Copy, Delete, Move, NewFolder, NewShader, NewScript } type = None;
        fs::path a; // primary path
        fs::path b; // destination dir (Move) / parent dir (New*)
    };

    // Starter template written into a freshly-created custom shader. Uses the
    // same SM3.0 entry points (VSMain/PSMain) as the built-in standard shader.
    const char* kNewShaderTemplate =
        "// Custom shader (Shader Model 3.0). Entry points: VSMain (vertex), PSMain (pixel).\n"
        "\n"
        "// Render setup - how this shader draws (edit the values):\n"
        "//@geometry quad\n"
        "//@blend    opaque\n"
        "//@depth    on\n"
        "//@cull     none\n"
        "//   geometry: quad | volume | model\n"
        "//   blend:    opaque | alpha | additive\n"
        "//   depth:    on | test | off\n"
        "//   cull:     none | back\n"
        "\n"
        "// Declarations available to the shader:\n"
        "//   VS: c0 gWVP (world*view*proj),  c4 gWorld\n"
        "//   PS: c3 gTime (.x = seconds),    c4 gCamObj (camera in local space, geometry \"volume\")\n"
        "//       s0 diffuse, s1 normal, s2 specular  (the model's maps, geometry \"model\")\n"
        "// Vertex input: POSITION, NORMAL, TANGENT, TEXCOORD0.\n"
        "\n"
        "float4x4 gWVP : register(c0);\n"
        "\n"
        "struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 tan : TANGENT; float2 uv : TEXCOORD0; };\n"
        "struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "\n"
        "VSOut VSMain(VSIn i)\n"
        "{\n"
        "    VSOut o;\n"
        "    o.pos = mul(float4(i.pos, 1.0), gWVP);\n"
        "    o.uv  = i.uv;\n"
        "    return o;\n"
        "}\n"
        "\n"
        "float4 PSMain(VSOut i) : COLOR\n"
        "{\n"
        "    return float4(i.uv, 0.0, 1.0);\n"
        "}\n";

    // Starter written into a freshly-created Lua gameplay script. Attach it via a
    // "Script" attribute; see DeveloperAPI.md for the full function reference.
    const char* kNewScriptTemplate =
        "-- Gameplay script. Attach with a \"Script\" attribute; add a Dynamic\n"
        "-- \"Rigid Body\" if it should move. See DeveloperAPI.md for all functions.\n"
        "-- Lifecycle: on_start(), on_update(dt), on_trigger(other).\n"
        "\n"
        "function on_start()\n"
        "    -- Runs once when Play (editor) or the title (console) starts.\n"
        "    log(\"script started\")\n"
        "end\n"
        "\n"
        "function on_update(dt)\n"
        "    -- Runs every frame; dt = seconds since the last frame.\n"
        "    -- Example: move with the left stick / WASD, jump on A / Space.\n"
        "    -- local vx, vy, vz = self:velocity()\n"
        "    -- self:set_velocity(input.axis(\"LX\") * 6, vy, -input.axis(\"LY\") * 6)\n"
        "    -- if input.button(\"A\") and math.abs(vy) < 0.5 then\n"
        "    --     self:apply_impulse(0, 7, 0)\n"
        "    -- end\n"
        "end\n"
        "\n"
        "function on_trigger(entrant)\n"
        "    -- Runs when a Trigger Volume on this object is entered. `entrant` is\n"
        "    -- the object that entered (name it whatever you like).\n"
        "    -- if entrant:name() == \"fox\" then log(\"caught the fox\") end\n"
        "end\n";
    PendingOp g_pending;
    bool      g_open_rename = false;
    fs::path  g_rename_target;
    char      g_rename_buf[256] = {};

    fs::path UniqueName(const fs::path& desired)
    {
        std::error_code ec;
        if (!fs::exists(desired, ec))
            return desired;
        const fs::path dir  = desired.parent_path();
        const std::string stem = desired.stem().string();
        const std::string ext  = desired.extension().string();
        for (int n = 2; n < 1000; ++n)
        {
            fs::path c = dir / (stem + " " + std::to_string(n) + ext);
            if (!fs::exists(c, ec))
                return c;
        }
        return desired;
    }

    // True if `p` is `base` or lives inside it (used to block moving a folder
    // into itself / a descendant).
    bool UnderOrEqual(const fs::path& base, const fs::path& p)
    {
        const std::string b = base.lexically_normal().string();
        const std::string q = p.lexically_normal().string();
        return q.size() >= b.size() && q.compare(0, b.size(), b) == 0;
    }

    // Drop target that moves the dragged asset into `dest_dir`.
    void MoveDropTarget(EngineState& state, const fs::path& dest_dir)
    {
        if (!ImGui::BeginDragDropTarget())
            return;
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            std::string rel((const char*)pl->Data, (std::size_t)pl->DataSize);
            g_pending = {PendingOp::Move, state.project_root / rel, dest_dir};
        }
        ImGui::EndDragDropTarget();
    }
}

void AssetsPanel::Render(EngineState& state)
{
    if (!state.show_assets_panel)
        return;

    if (!ImGui::Begin(loc::TWin("panel.assets.title", "Assets"), &state.show_assets_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasProject())
    {
        ImGui::TextDisabled(loc::T("common.no_project"));
        ImGui::End();
        return;
    }

    const fs::path dir = state.AssetsDir() / state.assets_cwd;

    // --- Breadcrumb (each crumb is also a move target) + Import on the right ---
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " assets"))
        state.assets_cwd.clear();
    MoveDropTarget(state, state.AssetsDir());
    {
        fs::path acc;
        for (const fs::path& part : state.assets_cwd)
        {
            acc /= part;
            ImGui::SameLine(0.0f, 4.0f); ImGui::TextUnformatted("/"); ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::Button(part.string().c_str()))
                state.assets_cwd = acc;
            MoveDropTarget(state, state.AssetsDir() / acc);
        }
    }
    {
        const char* lbl = loc::TI(ICON_FA_PLUS, "assets.import");
        const float w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (ImGui::Button(lbl))
            state.show_import_modal = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(loc::T("assets.import_tooltip"));
    }

    ImGui::Separator();

    // --- Gather + sort entries (folders first) ---
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        entries.push_back(e);
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b)
              {
                  const bool da = a.is_directory(), db = b.is_directory();
                  if (da != db) return da;
                  return a.path().filename().string() < b.path().filename().string();
              });

    // --- Tile grid ---
    const float tile_w = 96.0f, tile_h = 104.0f, spacing = 10.0f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const int   cols = (std::max)(1, (int)((avail_w + spacing) / (tile_w + spacing)));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    fs::path pending_nav;
    bool     do_nav = false;

    int col = 0;
    for (const fs::directory_entry& e : entries)
    {
        const fs::path    p    = e.path();
        const std::string nm   = p.filename().string();
        const AssetKind   kind = Classify(e);

        ImGui::PushID(nm.c_str());
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(tile_w, tile_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool dbl     = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const ImVec2 p1(p0.x + tile_w, p0.y + tile_h);

        // Drag source (files and folders can be moved).
        if (ImGui::BeginDragDropSource())
        {
            const std::string rel = fs::relative(p, state.project_root).generic_string();
            ImGui::SetDragDropPayload("ASSET_PATH", rel.c_str(), rel.size());
            ImGui::TextUnformatted(nm.c_str());
            ImGui::EndDragDropSource();
        }
        // Folders are move targets.
        if (kind == AssetKind::Folder)
            MoveDropTarget(state, p);

        // Right-click context menu.
        if (ImGui::BeginPopupContextItem("##ctx"))
        {
            if (kind == AssetKind::Shader || kind == AssetKind::Text || kind == AssetKind::Generic)
            {
                const bool animator = p.extension() == ".anim";
                if (ImGui::MenuItem(animator ? "Open Animator" : "Open in editor"))
                {
                    if (animator)
                    {
                        state.open_animator_path = p;
                        state.show_animator_panel = true;
                        ImGui::SetWindowFocus("###Animator");
                    }
                    else
                    {
                        state.open_file_path = p;
                        state.show_editor_panel = true;
                        ImGui::SetWindowFocus("###Editor");
                    }
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem(loc::TL("common.rename")))
            {
                g_rename_target = p;
                std::snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", nm.c_str());
                g_open_rename = true;
            }
            if (ImGui::MenuItem(loc::TL("common.copy")))   g_pending = {PendingOp::Copy,   p, {}};
            if (ImGui::MenuItem(loc::TL("common.delete"))) g_pending = {PendingOp::Delete, p, {}};
            ImGui::EndPopup();
        }

        const bool selected = (selected_ == p);
        if (selected)      dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_Header), 6.0f);
        else if (hovered)  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6.0f);

        DrawVisual(dl, ImVec2(p0.x + tile_w * 0.5f, p0.y + 36.0f), kind, state);
        const std::string label = Ellipsize(nm, tile_w - 10.0f);
        const float tw = ImGui::CalcTextSize(label.c_str()).x;
        dl->AddText(ImVec2(p0.x + (tile_w - tw) * 0.5f, p0.y + 74.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

        // Single click selects; double click opens (folder = navigate, text = editor).
        if (clicked)
            selected_ = p;
        if (dbl)
        {
            if (kind == AssetKind::Folder) { pending_nav = state.assets_cwd / nm; do_nav = true; }
            else if (kind == AssetKind::Text || kind == AssetKind::Shader || kind == AssetKind::Generic)
            {
                if (p.extension() == ".anim")
                {
                    state.open_animator_path = p;
                    state.show_animator_panel = true;
                    ImGui::SetWindowFocus("###Animator");
                }
                else
                {
                    state.open_file_path = p;
                    state.show_editor_panel = true;
                    ImGui::SetWindowFocus("###Editor");
                }
            }
        }

        ImGui::PopID();

        if (++col < cols) ImGui::SameLine(0.0f, spacing);
        else              col = 0;
    }

    if (do_nav)
        state.assets_cwd = pending_nav;

    // Right-click empty space -> New Folder.
    if (ImGui::BeginPopupContextWindow("##assets_bg",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem(loc::TI(ICON_FA_FOLDER, "assets.new_folder")))
            g_pending = {PendingOp::NewFolder, dir, {}};
        if (ImGui::MenuItem(loc::TI(ICON_FA_FILE, "assets.new_shader")))
            g_pending = {PendingOp::NewShader, dir, {}};
        if (ImGui::MenuItem(loc::TI(ICON_FA_FILE, "assets.new_script")))
            g_pending = {PendingOp::NewScript, dir, {}};
        ImGui::EndPopup();
    }

    // --- Apply deferred filesystem op ---
    if (g_pending.type != PendingOp::None)
    {
        std::error_code fec;
        switch (g_pending.type)
        {
        case PendingOp::Copy:
        {
            const fs::path src = g_pending.a;
            const fs::path dst = UniqueName(src.parent_path() /
                                            (src.stem().string() + " copy" + src.extension().string()));
            if (fs::is_directory(src, fec)) fs::copy(src, dst, fs::copy_options::recursive, fec);
            else                            fs::copy_file(src, dst, fec);
            applog::Info(fec ? "Copy failed: " + src.filename().string()
                             : "Copied " + src.filename().string());
            break;
        }
        case PendingOp::Delete:
        {
            fs::remove_all(g_pending.a, fec);
            applog::Info(fec ? "Delete failed: " + g_pending.a.filename().string()
                             : "Deleted " + g_pending.a.filename().string());
            if (selected_ == g_pending.a) selected_.clear();
            break;
        }
        case PendingOp::Move:
        {
            const fs::path src = g_pending.a, destDir = g_pending.b;
            const bool same    = (src.parent_path() == destDir);
            const bool intoSelf = fs::is_directory(src, fec) && UnderOrEqual(src, destDir);
            if (!same && !intoSelf && src != destDir)
            {
                fs::rename(src, destDir / src.filename(), fec);
                applog::Info(fec ? "Move failed: " + src.filename().string()
                                 : "Moved " + src.filename().string() + " -> " + destDir.filename().string());
            }
            break;
        }
        case PendingOp::NewFolder:
        {
            fs::create_directory(UniqueName(g_pending.a / "New Folder"), fec);
            break;
        }
        case PendingOp::NewShader:
        {
            const fs::path path = UniqueName(g_pending.a / "NewShader.hlsl");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << kNewShaderTemplate;
            applog::Info(out ? "Created " + path.filename().string()
                             : "Create shader failed: " + path.filename().string());
            break;
        }
        case PendingOp::NewScript:
        {
            const fs::path path = UniqueName(g_pending.a / "NewScript.lua");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << kNewScriptTemplate;
            applog::Info(out ? "Created " + path.filename().string()
                             : "Create script failed: " + path.filename().string());
            break;
        }
        default: break;
        }
        g_pending.type = PendingOp::None;
    }

    // --- Rename popup ---
    if (g_open_rename) { ImGui::OpenPopup("Rename Asset"); g_open_rename = false; }
    if (ImGui::BeginPopup("Rename Asset"))
    {
        ImGui::TextUnformatted(loc::T("common.new_name"));
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputText("##rn", g_rename_buf, sizeof(g_rename_buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button(loc::TL("common.rename")) || enter) && g_rename_buf[0] != '\0')
        {
            std::error_code fec;
            fs::rename(g_rename_target, g_rename_target.parent_path() / g_rename_buf, fec);
            applog::Info(fec ? "Rename failed" : "Renamed to " + std::string(g_rename_buf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(loc::TL("common.cancel")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}
