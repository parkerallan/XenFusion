# Bullet Physics (vendored)

Vendored subset of [Bullet Physics](https://github.com/bulletphysics/bullet3),
used by both the PC editor (CMake) and the Xbox 360 runtime (`game360.vcxproj`)
so the two targets run the **same** physics code.

- **Version:** 2.82 (`BT_BULLET_VERSION 282`)
- **Tag:** `2.82`
- **Upstream commit:** `19f999ac087e68ffc2551ffb73e35e60271a0d27`
- **License:** zlib — see `LICENSE.txt`.

## What's here

Only the three libraries the engine needs, under `src/`:

- `LinearMath` — vectors, transforms, aligned containers
- `BulletCollision` — collision shapes, broadphase, ghost objects (triggers)
- `BulletDynamics` — the dynamics world, rigid bodies, constraint solver

Skipped upstream trees: `BulletSoftBody`, `BulletMultiThreaded`, `MiniCL`,
`vectormath`, examples, tests, and the `.bullet` serialization users.

## Platform SIMD (do not "fix")

`LinearMath/btScalar.h` selects the SIMD backend from the compiler's platform
macros — no build flags needed:

- **Xbox 360:** the XDK defines `_XBOX`, which selects Bullet's native
  `BT_USE_VMX128` path (`ppcintrinsics.h`, `__fsel`). This is Bullet's genuine,
  shipped-on-360 configuration.
- **PC editor:** MSVC selects `BT_USE_SSE`.

Do **not** use Bullet's `.bullet` serialization (`btBulletWorldImporter` /
`btSerializer`) — that format is the only endian-sensitive part of Bullet, and
it is not vendored. The physics world is always built at load time from the
scene's own attribute data, which is endian-clean.

`CMakeLists.bullet.cmake` (in the project root, included by the top-level
`CMakeLists.txt`) compiles this tree into a static `bullet` target for the
editor; the runtime lists the same `.cpp` files in `game360.vcxproj`.
