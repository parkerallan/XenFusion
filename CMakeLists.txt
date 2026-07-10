cmake_minimum_required(VERSION 3.20)
project(engine360 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Dear ImGui (docking branch) pulled in at configure time.
# ImGui ships no CMakeLists of its own, so we compile the core + the two
# backends we need (Win32 for windowing/input, DX9 for rendering) as a
# static library.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6-docking
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
# d3d9.lib ships with the Windows SDK. Note: the ImGui DX9 backend does NOT
# require the legacy d3dx9 helper library, so no old DirectX SDK is needed.
target_link_libraries(imgui PUBLIC d3d9)

# ---------------------------------------------------------------------------
# nlohmann/json (single-header) for project manifests and .scene files.
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# ImGuiColorTextEdit (BalazsJako) — the code editor widget, same as the
# reference engine. Pinned to a master commit that uses the modern ImGui key
# API. It ships no CMakeLists, so we compile TextEditor.cpp ourselves.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imguicolortextedit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG        ca2f9f1462e3b60e56351bc466acda448c5ea50d
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(imguicolortextedit)

add_library(imguicolortextedit STATIC ${imguicolortextedit_SOURCE_DIR}/TextEditor.cpp)
target_include_directories(imguicolortextedit PUBLIC ${imguicolortextedit_SOURCE_DIR})
target_link_libraries(imguicolortextedit PUBLIC imgui)

# ---------------------------------------------------------------------------
# The engine executable.
# ---------------------------------------------------------------------------
add_executable(engine360
    src/app/main.cpp
    src/app/EngineApplication.cpp
    src/render/Renderer.cpp
    src/render/SceneRenderer.cpp
    src/platform/Dialogs.cpp
    src/project/ProjectIO.cpp
    src/panels/ViewportPanel.cpp
    src/panels/InspectorPanel.cpp
    src/panels/FilesPanel.cpp
    src/panels/VersionControlPanel.cpp
    src/panels/AssetsPanel.cpp
    src/panels/SettingsPanel.cpp
    src/panels/LogPanel.cpp
    src/panels/PerformancePanel.cpp
    src/panels/EditorPanel.cpp
)
target_include_directories(engine360 PRIVATE src)
# Ole32: native folder picker (IFileDialog). Shell32: Explorer drag-drop.
target_link_libraries(engine360 PRIVATE imgui d3d9 nlohmann_json::nlohmann_json imguicolortextedit Ole32 Shell32)

# Copy the working directory so imgui.ini (window layout) lands next to the exe.
set_target_properties(engine360 PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine360>"
)

# Ship the icon font next to the exe so it can be loaded at runtime.
add_custom_command(TARGET engine360 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:engine360>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/fonts/fa-solid-900.ttf"
        "$<TARGET_FILE_DIR:engine360>/fonts/fa-solid-900.ttf"
    VERBATIM
)
