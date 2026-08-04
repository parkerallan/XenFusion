# Opt-in host-side tests. Included by the root CMakeLists only when configured
# with -DXENFUSION_BUILD_TESTS=ON, so normal builds do not generate these
# executable targets or add them to ALL_BUILD.

add_executable(light_select_test tests/LightSelectTest.cpp)
target_include_directories(light_select_test PRIVATE src)
add_test(NAME light_select COMMAND light_select_test)

add_executable(light_basis_test tests/LightBasisTest.cpp)
target_include_directories(light_basis_test PRIVATE src)
add_test(NAME light_basis COMMAND light_basis_test)

add_executable(lightmap_unwrap_test
    tests/LightmapUnwrapTest.cpp
    src/light/LightmapUnwrap.cpp
)
target_include_directories(lightmap_unwrap_test PRIVATE src)
target_link_libraries(lightmap_unwrap_test PRIVATE assimp xatlas)
add_test(NAME lightmap_unwrap COMMAND lightmap_unwrap_test
    "${CMAKE_SOURCE_DIR}/../360proj/assets/models/brick_wall.obj")

add_executable(mesh_skin_bake_test
    tests/MeshSkinBakeTest.cpp
    src/render/MeshBaker.cpp
)
target_include_directories(mesh_skin_bake_test PRIVATE src)
target_link_libraries(mesh_skin_bake_test PRIVATE assimp d3d9)
add_test(NAME mesh_skin_bake_fox
    COMMAND mesh_skin_bake_test
        "${CMAKE_SOURCE_DIR}/../360proj/assets/models/Fox.gltf")

add_executable(skin_shader_compile_test
    tests/SkinShaderCompileTest.cpp
    src/render/Shader.cpp
    src/core/Log.cpp
)
target_include_directories(skin_shader_compile_test PRIVATE src)
target_link_libraries(skin_shader_compile_test PRIVATE d3d9 d3dcompiler)
add_test(NAME skin_shader_compile
    COMMAND skin_shader_compile_test
        "${CMAKE_SOURCE_DIR}/src/shaders/standard.hlsl")

add_executable(animator_controller_test
    tests/AnimatorControllerTest.cpp
    src/anim/AnimatorController.cpp
)
target_include_directories(animator_controller_test PRIVATE src)
target_link_libraries(animator_controller_test PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME animator_controller_round_trip
    COMMAND animator_controller_test
        "${CMAKE_SOURCE_DIR}/../360proj/assets/animators/NewAnimator.anim")

add_executable(animation_clip_test
    tests/AnimationClipTest.cpp
    src/anim/AnimationClip.cpp
)
target_include_directories(animation_clip_test PRIVATE src)
target_link_libraries(animation_clip_test PRIVATE assimp)
add_test(NAME animation_clip_fox
    COMMAND animation_clip_test
        "${CMAKE_SOURCE_DIR}/../360proj/assets/models/Fox.gltf")

add_executable(animator_runtime_test
    tests/AnimatorRuntimeTest.cpp
    src/anim/AnimatorRuntime.cpp
)
target_include_directories(animator_runtime_test PRIVATE src)
target_link_libraries(animator_runtime_test PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME animator_runtime_transitions COMMAND animator_runtime_test)

add_executable(animation_codec_test
    tests/AnimationCodecTest.cpp
    src/anim/AnimationClip.cpp
    src/anim/AnimationCodec.cpp
)
target_include_directories(animation_codec_test PRIVATE src runtime/src)
target_link_libraries(animation_codec_test PRIVATE assimp)
add_test(NAME animation_codec_fox
    COMMAND animation_codec_test
        "${CMAKE_SOURCE_DIR}/../360proj/assets/models/Fox.gltf")

add_executable(animator_cooker_test
    tests/AnimatorCookerTest.cpp
    src/anim/AnimatorController.cpp
    src/anim/AnimatorCooker.cpp
    src/anim/AnimationClip.cpp
    src/anim/AnimationCodec.cpp
)
target_include_directories(animator_cooker_test PRIVATE src runtime/src)
target_link_libraries(animator_cooker_test PRIVATE assimp nlohmann_json::nlohmann_json)
add_test(NAME animator_cooker_fox
    COMMAND animator_cooker_test
        "${CMAKE_SOURCE_DIR}/../360proj"
        "${CMAKE_SOURCE_DIR}/../360proj/assets/animators/FoxAnim.anim")

add_executable(text_layout_test
    tests/TextLayoutTest.cpp
    src/text/TextLayout.cpp
    src/text/CookedFont.cpp
)
target_include_directories(text_layout_test PRIVATE src runtime/src)
add_test(NAME text_layout COMMAND text_layout_test)