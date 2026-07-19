#include "app/EngineApplication.h"

#include "app/Settings.h"
#include "build/BuildRun.h"
#include "platform/Dialogs.h"
#include "project/ProjectIO.h"
#include "ui/Icons.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

#include <shellapi.h> // Drag-drop from Explorer

#include <algorithm>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// The Win32 backend exposes its message handler through this forward-decl.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static EngineApplication* g_app = nullptr;

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    if (g_app)
        return g_app->HandleMessage(hWnd, msg, wParam, lParam);
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT EngineApplication::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            renderer_.OnResize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // swallow the Alt application menu
            return 0;
        break;
    case WM_DROPFILES:
        OnDropFiles((void*)wParam);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EngineApplication::Init()
{
    g_app = this;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       ::GetModuleHandleW(nullptr), nullptr, nullptr, nullptr,
                       nullptr, L"XenFusionWindow", nullptr };
    ::RegisterClassExW(&wc);
    window_ = ::CreateWindowW(wc.lpszClassName, L"XenFusion",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1440, 900,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!window_)
        return false;

    if (!renderer_.Init(window_))
    {
        ::MessageBoxW(nullptr, L"Failed to create the Direct3D 9 device.",
                      L"XenFusion", MB_OK | MB_ICONERROR);
        return false;
    }

    ::ShowWindow(window_, SW_SHOWDEFAULT);
    ::UpdateWindow(window_);
    ::DragAcceptFiles(window_, TRUE); // accept file drops from Explorer

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    settings::Load(state_); // restore persisted settings before the theme is applied

    LoadFonts();
    ApplyStyle();

    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX9_Init(renderer_.Device());

    viewport_panel_.Init(renderer_.Device());

    // Both ImGui's font atlas and the scene's render target are D3DPOOL_DEFAULT
    // resources; they must be released before a device Reset and rebuilt after.
    renderer_.AddResetHandler(
        [] { ImGui_ImplDX9_InvalidateDeviceObjects(); },
        [] { ImGui_ImplDX9_CreateDeviceObjects(); });
    renderer_.AddResetHandler(
        [this] { viewport_panel_.OnDeviceLost(); }, // recreation is lazy
        nullptr);

    QueryPerformanceFrequency(&qpc_freq_);
    QueryPerformanceCounter(&qpc_last_);

    project::LoadRecents(state_);
    state_.AddLog("XenFusion started (Direct3D 9 backend)");

    // Startup launcher: show recent projects when nothing is open.
    if (!state_.HasProject())
        state_.show_recent_modal = true;

    running_ = true;
    return true;
}

float EngineApplication::TickDeltaSeconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - qpc_last_.QuadPart) / (double)qpc_freq_.QuadPart;
    qpc_last_ = now;
    if (dt < 0.0 || dt > 0.25) dt = 1.0 / 60.0; // clamp hitches / first frame
    return (float)dt;
}

void EngineApplication::ProcessEvents()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (msg.message == WM_QUIT)
            running_ = false;
    }
}

void EngineApplication::RunLoop()
{
    while (running_)
    {
        ProcessEvents();
        if (!running_)
            break;

        const float dt = TickDeltaSeconds();

        // Per-frame reset of panel/renderer state, before the ImGui frame.
        viewport_panel_.BeginFrame();

        // Device management (recover lost device / apply resize).
        if (!renderer_.BeginFrame())
        {
            ::Sleep(10); // device lost or minimized
            continue;
        }
        renderer_.SetClearColor(0.06f, 0.06f, 0.07f, 1.0f); // editor background

        // Build the ImGui frame. Panels (incl. the Viewport) run here; the
        // Viewport records its scene image but does not render it yet.
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::EndFrame();
        ImGui::Render();

        // GPU scene pass: fills the Viewport's offscreen target with the 3D
        // scene. Its own BeginScene/EndScene, done before the back-buffer scene
        // is opened (D3D9 scenes must not nest) so the UI can sample it.
        viewport_panel_.RenderSceneGpuPass(dt);

        // Back-buffer pass: draw the UI (which samples the scene texture).
        if (renderer_.BeginBackbuffer())
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            renderer_.EndFrame();
        }
    }
}

void EngineApplication::RenderUI()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("EngineDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    RenderMainMenuBar();

    // Ctrl+S saves the selected scene.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        if (SceneFile* scene = state_.SelectedScene())
            project::SaveScene(*scene);

    const ImGuiID dockspace_id = ImGui::GetID("EngineDockSpace");
    BuildDefaultDockLayout(dockspace_id);
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    // Panels.
    files_panel_.Render(state_);
    version_control_panel_.Render(state_);
    viewport_panel_.Render(state_);
    editor_panel_.Render(state_);
    settings_panel_.Render(state_);
    // Settings requested a toolchain folder pick — open the native picker here
    // (EngineApplication owns the window handle).
    if (state_.toolchain_pick)
    {
        std::filesystem::path folder;
        if (platform::PickFolder(window_, folder))
        {
            const std::string p = folder.string();
            switch (state_.toolchain_pick)
            {
            case 1: state_.toolchain_xdk      = p; break;
            case 2: state_.toolchain_emulator = p; break;
            case 3: state_.build_output_dir   = p; break;
            }
            settings::Save(state_); // persist the toolchain path immediately
        }
        state_.toolchain_pick = 0;
    }
    inspector_panel_.Render(state_);
    assets_panel_.Render(state_);
    log_panel_.Render(state_);
    performance_panel_.Render(state_);
    mapper_panel_.Render(state_);

    RenderImportModal();
    RenderRecentModal();

    // Live theme editor. ShowStyleEditor() draws into the current window, so we
    // wrap it. Its Colors tab has an "Export" button that copies the changed
    // ImGuiCol_ entries to the clipboard — paste those into ApplyStyle().
    if (state_.show_style_editor)
    {
        if (ImGui::Begin("Style Editor", &state_.show_style_editor))
        {
            // Engine themes. The stock selector further down only lists ImGui's
            // built-in palettes; this one applies (and persists) ours.
            static const char* kThemes[] = { "Light", "Gray (default)", "Classic" };
            int cur = (int)state_.theme;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Engine theme", &cur, kThemes, IM_ARRAYSIZE(kThemes)))
            {
                state_.theme = (Theme)cur;
                ApplyStyle();
            }
            ImGui::Separator();
            ImGui::ShowStyleEditor();
        }
        ImGui::End();
    }

    // "Build and Run XEX": kick off on request and stream its output to the Log.
    if (state_.build_run_requested)
    {
        buildrun::Start(state_);
        state_.build_run_requested = false;
    }
    if (state_.build_iso_requested)
    {
        buildrun::StartIso(state_);
        state_.build_iso_requested = false;
    }
    if (state_.clean_build_requested)
    {
        buildrun::Clean(state_);
        state_.clean_build_requested = false;
    }
    buildrun::Poll(state_);
}

void EngineApplication::RenderMainMenuBar()
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
        {
            std::filesystem::path folder;
            if (platform::PickFolder(window_, folder))
                project::CreateProject(folder, state_);
        }
        if (ImGui::MenuItem("Open Project..."))
        {
            std::filesystem::path folder;
            if (platform::PickFolder(window_, folder))
                project::OpenProject(folder, state_);
        }
        if (ImGui::MenuItem("Recent Projects..."))
            state_.show_recent_modal = true;
        if (ImGui::MenuItem("Close Project", nullptr, false, state_.HasProject()))
            project::CloseProject(state_);

        ImGui::Separator();

        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, state_.SelectedScene() != nullptr))
            project::SaveScene(*state_.SelectedScene());

        ImGui::Separator();

        if (ImGui::MenuItem("Exit"))
            running_ = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Build"))
    {
        if (ImGui::MenuItem("Rebuild Engine Shaders", nullptr, false, state_.HasProject()))
            state_.compile_shaders_requested = true;
        if (ImGui::MenuItem("Build and Run XEX", nullptr, false,
                            state_.HasProject() && !buildrun::Running()))
            state_.build_run_requested = true;
        if (ImGui::MenuItem("Build .iso Image", nullptr, false,
                            state_.HasProject() && !buildrun::Running()))
            state_.build_iso_requested = true;
        if (ImGui::MenuItem("Clean Build", nullptr, false, !buildrun::Running()))
            state_.clean_build_requested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Viewport",    nullptr, &state_.show_viewport_panel);
        ImGui::MenuItem("Editor",      nullptr, &state_.show_editor_panel);
        ImGui::MenuItem("Mapping",     nullptr, &state_.show_mapper_panel);
        ImGui::MenuItem("Inspector",   nullptr, &state_.show_inspector_panel);
        ImGui::MenuItem("Files",       nullptr, &state_.show_files_panel);
        ImGui::MenuItem("Version Control", nullptr, &state_.show_version_control_panel);
        ImGui::MenuItem("Assets",      nullptr, &state_.show_assets_panel);
        ImGui::MenuItem("Settings",    nullptr, &state_.show_settings_panel);
        ImGui::MenuItem("Log",         nullptr, &state_.show_log_panel);
        ImGui::MenuItem("Performance", nullptr, &state_.show_performance_panel);
        ImGui::Separator();
        ImGui::MenuItem("Style Editor", nullptr, &state_.show_style_editor);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void EngineApplication::OnDropFiles(void* hDrop)
{
    HDROP drop = (HDROP)hDrop;

    if (!state_.HasProject())
    {
        ::DragFinish(drop);
        state_.AddLog("Open a project before importing assets");
        return;
    }

    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i)
    {
        wchar_t buf[MAX_PATH];
        if (::DragQueryFileW(drop, i, buf, MAX_PATH))
        {
            fs::path p(buf);
            std::error_code ec;
            if (fs::is_regular_file(p, ec))
                state_.import_files.push_back(p);
        }
    }
    ::DragFinish(drop);

    state_.show_import_modal = true; // surface the modal with the staged files
}

void EngineApplication::ImportStagedFiles(const char* dest)
{
    if (!state_.HasProject())
        return;

    std::error_code ec;
    const fs::path dest_dir = state_.project_root / dest;
    fs::create_directories(dest_dir, ec);

    int imported = 0;
    for (const fs::path& src : state_.import_files)
    {
        fs::copy_file(src, dest_dir / src.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) { state_.AddLog("Import failed: " + src.filename().string()); ec.clear(); }
        else    { ++imported; }
    }
    state_.AddLog("Imported " + std::to_string(imported) + " file(s) to " + dest);
}

void EngineApplication::RenderImportModal()
{
    if (state_.show_import_modal && !ImGui::IsPopupOpen("Import Assets"))
        ImGui::OpenPopup("Import Assets");

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Import Assets", &state_.show_import_modal))
        return;

    ImGui::TextWrapped("Drag files from Windows Explorer onto this window to stage them.");
    ImGui::Spacing();

    static const char* kDests[] = { "assets", "assets/models", "assets/textures", "assets/audio" };
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Destination", &state_.import_dest, kDests, IM_ARRAYSIZE(kDests));

    ImGui::SeparatorText("Staged files");
    if (state_.import_files.empty())
    {
        ImGui::TextDisabled("(drag files here - none staged yet)");
    }
    else
    {
        ImGui::BeginChild("##staged", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders);
        for (int i = 0; i < (int)state_.import_files.size(); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::SmallButton("x"))
            {
                state_.import_files.erase(state_.import_files.begin() + i);
                ImGui::PopID();
                --i;
                continue;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(state_.import_files[i].filename().string().c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    const bool has_files = !state_.import_files.empty();
    ImGui::BeginDisabled(!has_files);
    if (ImGui::Button("Import", ImVec2(120.0f, 0.0f)))
    {
        ImportStagedFiles(kDests[state_.import_dest]);
        state_.import_files.clear();
        state_.show_import_modal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(120.0f, 0.0f)))
        state_.import_files.clear();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
    {
        state_.import_files.clear();
        state_.show_import_modal = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EngineApplication::RenderRecentModal()
{
    if (state_.show_recent_modal && !ImGui::IsPopupOpen("Recent Projects"))
        ImGui::OpenPopup("Recent Projects");

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Recent Projects", &state_.show_recent_modal))
        return;

    // New / Open buttons.
    if (ImGui::Button(ICON_FA_PLUS " New Project", ImVec2(160.0f, 0.0f)))
    {
        std::filesystem::path folder;
        if (platform::PickFolder(window_, folder) && project::CreateProject(folder, state_))
        {
            state_.show_recent_modal = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open Project", ImVec2(160.0f, 0.0f)))
    {
        std::filesystem::path folder;
        if (platform::PickFolder(window_, folder) && project::OpenProject(folder, state_))
        {
            state_.show_recent_modal = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SeparatorText(ICON_FA_CLOCK " Recent");

    if (state_.recent_projects.empty())
    {
        ImGui::TextDisabled("No recent projects.");
    }
    else
    {
        const float line_h = ImGui::GetTextLineHeight();
        const float item_h = line_h * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        int remove_index = -1;
        std::filesystem::path to_open;

        ImGui::BeginChild("##recent", ImVec2(-FLT_MIN, 240.0f), ImGuiChildFlags_FrameStyle);
        for (int i = 0; i < (int)state_.recent_projects.size(); ++i)
        {
            const std::filesystem::path& proj = state_.recent_projects[i];
            std::error_code ec;
            const bool exists = std::filesystem::exists(proj, ec);

            ImGui::PushID(i);
            const ImVec2 screen0 = ImGui::GetCursorScreenPos();
            const float  btn_w   = ImGui::GetFrameHeight();
            const float  sel_w   = ImGui::GetContentRegionAvail().x - btn_w - ImGui::GetStyle().ItemSpacing.x;

            if (ImGui::Selectable("##sel", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(sel_w, item_h)) && exists)
                to_open = proj;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(screen0.x + 6.0f, screen0.y + 2.0f),
                        ImGui::GetColorU32(exists ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                        proj.filename().string().c_str());
            dl->AddText(ImVec2(screen0.x + 6.0f, screen0.y + line_h + 4.0f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        proj.generic_string().c_str());

            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + item_h * 0.5f - btn_w * 0.5f);
            if (ImGui::Button("x", ImVec2(btn_w, btn_w)))
                remove_index = i;

            ImGui::PopID();
        }
        ImGui::EndChild();

        if (remove_index >= 0)
            project::RemoveRecent(state_, remove_index);
        if (!to_open.empty() && project::OpenProject(to_open, state_))
        {
            state_.show_recent_modal = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
}

void EngineApplication::LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();

    // Resolve the icon font next to the executable.
    wchar_t exe[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path font_path =
        std::filesystem::path(std::wstring(exe, n)).parent_path() / "fonts" / "fa-solid-900.ttf";

    if (std::filesystem::exists(font_path))
    {
        // Range must outlive atlas build, hence static.
        static const ImWchar range[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        ImFontConfig cfg;
        cfg.MergeMode        = true;  // merge icons into the default font
        cfg.PixelSnapH       = true;
        cfg.GlyphMinAdvanceX = 13.0f; // keep icons a consistent width
        io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 13.0f, &cfg, range);

        // A separate large icons-only font for the Assets browser tiles.
        state_.large_icon_font =
            io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 44.0f, nullptr, range);
    }
    else
    {
        state_.AddLog("Icon font missing: " + font_path.string());
    }
}

void EngineApplication::ApplyStyle()
{
    // Base colors from the selected theme.
    switch (state_.theme)
    {
    case Theme::Gray:    ApplyGrayTheme();           break;
    case Theme::Light:   ImGui::StyleColorsLight();  break;
    case Theme::Classic:
    default:             ImGui::StyleColorsClassic(); break;
    }

    // Shared sizing / borders for every theme.
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameBorderSize   = 1.0f; // frame borders enabled
    style.WindowRounding    = 7.0f;
    style.ChildRounding     = 7.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    style.WindowPadding     = ImVec2(12.0f, 10.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
}

void EngineApplication::ApplyGrayTheme()
{
    // Start from the light theme, then shift the surfaces toward gray and swap
    // the default blue accent for orange.
    ImGui::StyleColorsLight();

    const ImVec4 orange       = ImVec4(0.90f, 0.50f, 0.12f, 1.00f);
    const ImVec4 orange_hover = ImVec4(0.98f, 0.58f, 0.20f, 1.00f);
    const ImVec4 orange_active= ImVec4(0.78f, 0.40f, 0.08f, 1.00f);
    auto tint = [](const ImVec4& col, float a) { return ImVec4(col.x, col.y, col.z, a); };

    ImVec4* c = ImGui::GetStyle().Colors;

    // Gray surfaces.
    c[ImGuiCol_WindowBg]       = ImVec4(0.80f, 0.80f, 0.82f, 1.00f);
    c[ImGuiCol_ChildBg]        = ImVec4(0.83f, 0.83f, 0.85f, 1.00f);
    c[ImGuiCol_PopupBg]        = ImVec4(0.85f, 0.85f, 0.87f, 1.00f);
    c[ImGuiCol_MenuBarBg]      = ImVec4(0.76f, 0.76f, 0.78f, 1.00f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_TitleBg]        = ImVec4(0.74f, 0.74f, 0.76f, 1.00f);
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.66f, 0.66f, 0.69f, 1.00f);
    c[ImGuiCol_Button]         = ImVec4(0.74f, 0.74f, 0.77f, 1.00f);
    c[ImGuiCol_Tab]            = ImVec4(0.72f, 0.72f, 0.75f, 1.00f);
    c[ImGuiCol_Border]         = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);
    c[ImGuiCol_Separator]      = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);

    // Orange accents (everywhere the light theme used blue).
    c[ImGuiCol_CheckMark]        = orange;
    c[ImGuiCol_SliderGrab]       = orange;
    c[ImGuiCol_SliderGrabActive] = orange_active;
    c[ImGuiCol_FrameBgHovered]   = tint(orange, 0.30f);
    c[ImGuiCol_FrameBgActive]    = tint(orange, 0.55f);
    c[ImGuiCol_Header]           = tint(orange, 0.45f);
    c[ImGuiCol_HeaderHovered]    = tint(orange, 0.70f);
    c[ImGuiCol_HeaderActive]     = orange;
    c[ImGuiCol_ButtonHovered]    = orange_hover;
    c[ImGuiCol_ButtonActive]     = orange_active;
    c[ImGuiCol_Tab]              = ImVec4(0.72f, 0.72f, 0.75f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.98f, 0.70f, 0.40f, 1.00f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.95f, 0.62f, 0.28f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]        = orange;
    // Docked panels that aren't the focused window use the *dimmed* tab colors —
    // these were left as the light theme's blue.
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.76f, 0.76f, 0.78f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.90f, 0.68f, 0.45f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = tint(orange, 0.60f);
    c[ImGuiCol_SeparatorHovered] = orange_hover;
    c[ImGuiCol_SeparatorActive]  = orange_active;
    c[ImGuiCol_ResizeGrip]       = tint(orange, 0.35f);
    c[ImGuiCol_ResizeGripHovered]= tint(orange, 0.70f);
    c[ImGuiCol_ResizeGripActive] = orange;
    c[ImGuiCol_TextSelectedBg]   = tint(orange, 0.35f);
    c[ImGuiCol_DockingPreview]   = tint(orange, 0.45f);
    c[ImGuiCol_NavCursor]        = orange;
    c[ImGuiCol_DragDropTarget]   = orange;
    c[ImGuiCol_PlotLinesHovered] = orange_hover;
    c[ImGuiCol_PlotHistogram]    = orange;
}

void EngineApplication::BuildDefaultDockLayout(ImGuiID dockspace_id)
{
    if (state_.dock_layout_built)
        return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

    ImGuiID center_id = dockspace_id;
    ImGuiID left_id   = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left,  0.20f, nullptr, &center_id);
    ImGuiID right_id  = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, nullptr, &center_id);
    ImGuiID bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down,  0.28f, nullptr, &center_id);

    ImGui::DockBuilderDockWindow("Files",          left_id);
    ImGui::DockBuilderDockWindow("Version Control", left_id);
    ImGui::DockBuilderDockWindow("Viewport",    center_id);
    ImGui::DockBuilderDockWindow("Editor",      center_id);
    ImGui::DockBuilderDockWindow("Settings",    center_id);
    ImGui::DockBuilderDockWindow("Inspector",   right_id);
    ImGui::DockBuilderDockWindow("Log",         bottom_id);
    ImGui::DockBuilderDockWindow("Assets",      bottom_id);
    ImGui::DockBuilderDockWindow("Performance", bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);

    state_.dock_layout_built = true;
    state_.AddLog("Created default dock layout");
}

void EngineApplication::Shutdown()
{
    settings::Save(state_); // persist settings on exit

    viewport_panel_.Shutdown();

    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    renderer_.Shutdown();

    if (window_)
    {
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
    ::UnregisterClassW(L"XenFusionWindow", ::GetModuleHandleW(nullptr));
    g_app = nullptr;
}
