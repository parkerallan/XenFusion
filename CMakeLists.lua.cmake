# ---------------------------------------------------------------------------
# Lua (vendored 5.4.7) — editor build.
#
# Compiles the curated Lua source subset (see third_party/lua/README.md) into a
# static "lua" target. Excludes the standalone main, the amalgamation, tests, and
# the io/os/loadlib/init libraries (compile-risk on the 360 + sandboxing). The
# runtime (game360.vcxproj) lists the same files by hand.
# ---------------------------------------------------------------------------
set(LUA_DIR ${CMAKE_SOURCE_DIR}/third_party/lua/src)

file(GLOB LUA_SOURCES ${LUA_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests|liolib|loslib|loadlib|linit)\\.c$")

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${LUA_DIR})

# Lua is C; silence its benign MSVC warnings so they don't drown the engine's.
if(MSVC)
    target_compile_options(lua PRIVATE /w)
endif()
