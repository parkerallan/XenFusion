cmake_minimum_required(VERSION 3.20)
project(XenFusion LANGUAGES CXX RC)

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
# ImGuizmo (CedricGuillemet) — the 3D manipulation gizmo, same as the
# reference engine. Ships no CMakeLists, so we compile ImGuizmo.cpp.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        a15acd87a3f3241a29ea1363ceafc680dca3a96b
)
FetchContent_MakeAvailable(imguizmo)
add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp)
target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR})
target_link_libraries(imguizmo PUBLIC imgui)

# ---------------------------------------------------------------------------
# Assimp — editor-side model importer. Only a few importers are enabled to
# keep the build reasonable. (First configure/build compiles Assimp — slow.)
# ---------------------------------------------------------------------------
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
)
FetchContent_MakeAvailable(assimp)

# ---------------------------------------------------------------------------
# Bullet Physics (vendored 2.82) — compiled into a static "bullet" target used
# by the editor's physics preview. The same source is compiled by the Xbox 360
# runtime (runtime/game360.vcxproj). See third_party/bullet/README.md.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/CMakeLists.bullet.cmake)

# ---------------------------------------------------------------------------
# Lua (vendored 5.4.7) — scripting interpreter, same source the 360 runtime
# compiles. See third_party/lua/README.md.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/CMakeLists.lua.cmake)

# ---------------------------------------------------------------------------
# Shaders. The engine ships standard.hlsl next to the exe (below). At runtime it
# compiles standard.hlsl AND the project's custom shaders into ONE folder that
# the project owns — <project>/shaders/ — so a shipped game has a complete,
# self-contained set of compiled .cso and doesn't depend on the engine folder.
# ---------------------------------------------------------------------------
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/src/shaders)

# ---------------------------------------------------------------------------
# The engine executable.
# ---------------------------------------------------------------------------
add_executable(XenFusion
    src/app/XenFusion.rc
    src/app/main.cpp
    src/app/EngineApplication.cpp
    src/app/Settings.cpp
    src/build/BuildRun.cpp
    src/core/Log.cpp
    src/render/Renderer.cpp
    src/render/SceneRenderer.cpp
    src/render/MeshBaker.cpp
    src/render/MeshLoad.cpp
    src/render/MeshCache.cpp
    src/render/TextureLoad.cpp
    src/render/Shader.cpp
    src/render/ShaderCache.cpp
    src/render/ShaderCompiler.cpp
    src/video/VideoPlayer.cpp
    src/video/AudioOut.cpp
    src/physics/PhysicsWorld.cpp
    src/script/ScriptVM.cpp
    src/input/ControllerMapping.cpp
    src/panels/MapperPanel.cpp
    src/platform/Dialogs.cpp
    src/project/ProjectIO.cpp
    src/project/SceneJson.cpp
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
target_include_directories(XenFusion PRIVATE src third_party)
# Lets the editor's "Build and Run XEX" locate the console runtime project +
# build_run.ps1 during development (the runtime is a sibling of the editor source).
target_compile_definitions(XenFusion PRIVATE RUNTIME_SOURCE_DIR="${CMAKE_SOURCE_DIR}/runtime")
# Ole32: native folder picker (IFileDialog). Shell32: Explorer drag-drop.
target_link_libraries(XenFusion PRIVATE imgui d3d9 d3dcompiler nlohmann_json::nlohmann_json imguicolortextedit imguizmo assimp bullet lua Ole32 Shell32 Xinput9_1_0 xaudio2)

# Copy the working directory so imgui.ini (window layout) lands next to the exe.
# Subsystem per config: Debug keeps the console window (log/stdout), all other
# configs are GUI apps — no console at launch. The entry override lets the GUI
# configs link the ordinary main().
set_target_properties(XenFusion PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:XenFusion>"
    WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
)
target_link_options(XenFusion PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>")

# Ship the icon font next to the exe so it can be loaded at runtime.
add_custom_command(TARGET XenFusion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:XenFusion>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/fonts/fa-solid-900.ttf"
        "$<TARGET_FILE_DIR:XenFusion>/fonts/fa-solid-900.ttf"
    VERBATIM
)

# Ship the splash image next to the exe (shown while the editor starts).
add_custom_command(TARGET XenFusion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:XenFusion>/branding"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/branding/splash.png"
        "$<TARGET_FILE_DIR:XenFusion>/branding/splash.png"
    VERBATIM
)

# Ship the engine's built-in shader sources in <exe>/shaders (the standard
# material + the bloom post passes). At runtime the engine compiles them into
# each project's own shaders/ folder (with the customs).
add_custom_command(TARGET XenFusion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:XenFusion>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SHADER_SRC_DIR}/standard.hlsl"
        "${SHADER_SRC_DIR}/beam.hlsl"
        "${SHADER_SRC_DIR}/bloom_bright.hlsl"
        "${SHADER_SRC_DIR}/bloom_blur.hlsl"
        "${SHADER_SRC_DIR}/bloom_combine.hlsl"
        "${SHADER_SRC_DIR}/video.hlsl"
        "$<TARGET_FILE_DIR:XenFusion>/shaders"
    VERBATIM
)
