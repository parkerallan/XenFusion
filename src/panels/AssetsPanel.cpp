#include "panels/AssetsPanel.h"

#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    enum class AssetKind { Folder, Image, Audio, Model, Shader, Text, Generic };

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
        if (in({".wav", ".ogg", ".mp3", ".flac"}))                         return AssetKind::Audio;
        if (in({".obj", ".fbx", ".mesh", ".gltf", ".glb", ".dae", ".3ds"})) return AssetKind::Model;
        if (in({".hlsl", ".fx", ".glsl", ".shader", ".vsh", ".fsh", ".vs", ".ps", ".cg"})) return AssetKind::Shader;
        if (in({".txt", ".lua", ".json", ".md", ".ini", ".cfg", ".xml", ".csv", ".h", ".hpp", ".cpp", ".c", ".cs"})) return AssetKind::Text;
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

    // Fake a shaded 3D sphere for shader assets.
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
}

void AssetsPanel::Render(EngineState& state)
{
    if (!state.show_assets_panel)
        return;

    if (!ImGui::Begin("Assets", &state.show_assets_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasProject())
    {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }

    // --- Breadcrumb + import button ---
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " assets"))
        state.assets_cwd.clear();
    {
        fs::path acc;
        for (const fs::path& part : state.assets_cwd)
        {
            acc /= part;
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::Button(part.string().c_str()))
                state.assets_cwd = acc;
        }
    }
    ImGui::SameLine();
    {
        const char* lbl = ICON_FA_PLUS " Import";
        const float w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (ImGui::Button(lbl))
            state.show_import_modal = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Import files (or drag them from Explorer)");
    }

    ImGui::Separator();

    // --- Gather current directory entries (folders first) ---
    const fs::path dir = state.AssetsDir() / state.assets_cwd;
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

    if (entries.empty())
    {
        ImGui::TextDisabled("(empty)");
        ImGui::End();
        return;
    }

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
        const fs::path  p    = e.path();
        const std::string nm = p.filename().string();
        const AssetKind kind = Classify(e);

        ImGui::PushID(nm.c_str());
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(tile_w, tile_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool dbl     = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const ImVec2 p1(p0.x + tile_w, p0.y + tile_h);

        const bool selected = (selected_ == p);
        if (selected)
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_Header), 6.0f);
        else if (hovered)
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6.0f);

        DrawVisual(dl, ImVec2(p0.x + tile_w * 0.5f, p0.y + 36.0f), kind, state);

        const std::string label = Ellipsize(nm, tile_w - 10.0f);
        const float tw = ImGui::CalcTextSize(label.c_str()).x;
        dl->AddText(ImVec2(p0.x + (tile_w - tw) * 0.5f, p0.y + 74.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

        if (kind == AssetKind::Folder)
        {
            if (clicked || dbl) { pending_nav = state.assets_cwd / nm; do_nav = true; }
        }
        else
        {
            if (clicked)
                selected_ = p;
            // Double-click a text/code/shader file to edit it.
            if (dbl && (kind == AssetKind::Text || kind == AssetKind::Shader || kind == AssetKind::Generic))
            {
                state.open_file_path = p;
                state.show_editor_panel = true;
                ImGui::SetWindowFocus("Editor");
            }
        }

        ImGui::PopID();

        if (++col < cols)
            ImGui::SameLine(0.0f, spacing);
        else
            col = 0;
    }

    if (do_nav)
        state.assets_cwd = pending_nav;

    ImGui::End();
}
