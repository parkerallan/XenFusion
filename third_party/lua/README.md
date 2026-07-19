# Lua (vendored)

Vendored [Lua](https://www.lua.org/) interpreter, used by both the PC editor
(CMake) and the Xbox 360 runtime (`game360.vcxproj`) so scripts run identically on
both — the same parity approach as the vendored Bullet.

- **Version:** 5.4.7 (`LUA_VERSION_MAJOR.MINOR.RELEASE` = 5.4.7)
- **Source:** github.com/lua/lua, commit `1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3`
- **License:** MIT — see `src/lua.h` header comment.

## Why the reference interpreter (not LuaJIT)

The Xbox 360 hypervisor blocks writable+executable memory, so no runtime
code-generation — LuaJIT can't run (and has no PowerPC backend). Reference Lua is a
pure **bytecode interpreter** in portable C, which is exactly what the console
needs. Scripts ship as **source** and are compiled on load — never `.luac`
bytecode, which is endian/word-size specific (the only endian-sensitive part of
Lua, in `ldump.c`/`lundump.c`, is thus never exercised on the big-endian console).

## Curated build set (do not add the excluded files)

`src/` holds the complete upstream source, but both builds compile a curated
subset. **Excluded** on purpose:

- `lua.c`, `onelua.c` — standalone `main()` / amalgamation (would collide).
- `ltests.c` — internal test harness.
- `liolib.c`, `loslib.c`, `loadlib.c`, `linit.c` — file I/O, OS access, dynamic
  library loading, and the open-everything init. Dropped for two reasons: they use
  CRT/OS facilities that aren't guaranteed on the 360, and gameplay scripts should
  be **sandboxed** (no file/OS/`require`). `ScriptVM` opens only the safe libraries
  (base, coroutine, table, string, math, utf8, debug) by calling their
  `luaopen_*` directly instead of `luaL_openlibs`.

`CMakeLists.lua.cmake` (project root, included by the top-level `CMakeLists.txt`)
builds the subset into a static `lua` target for the editor; the runtime lists the
same `.c` files in `game360.vcxproj`.
