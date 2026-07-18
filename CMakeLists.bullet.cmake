# ---------------------------------------------------------------------------
# Bullet Physics (vendored 2.82) — editor build.
#
# Compiles the three needed libraries (LinearMath / BulletCollision /
# BulletDynamics) from third_party/bullet/src into a single static "bullet"
# target. We do NOT use Bullet's own CMakeLists (it carries install/version/
# packaging plumbing we don't need); a plain glob is enough and keeps the
# runtime's file list — which lists the same .cpp by hand — easy to mirror.
#
# SIMD backend is chosen by btScalar.h from the compiler's platform macros
# (MSVC -> BT_USE_SSE here; the 360 XDK -> BT_USE_VMX128 in game360.vcxproj),
# so no SIMD flags are set here. See third_party/bullet/README.md.
# ---------------------------------------------------------------------------
set(BULLET_DIR ${CMAKE_SOURCE_DIR}/third_party/bullet/src)

file(GLOB_RECURSE BULLET_SOURCES
    ${BULLET_DIR}/LinearMath/*.cpp
    ${BULLET_DIR}/BulletCollision/*.cpp
    ${BULLET_DIR}/BulletDynamics/*.cpp
)

add_library(bullet STATIC ${BULLET_SOURCES})
target_include_directories(bullet PUBLIC ${BULLET_DIR})

# Bullet is C++03; silence its (many) benign MSVC warnings so they don't drown
# the engine's own output.
if(MSVC)
    target_compile_options(bullet PRIVATE /w)
endif()
