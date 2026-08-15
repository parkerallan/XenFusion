# Facial animation

Blendshape-driven faces on both targets: expression poses, procedural blinking
and eye gaze, from the same data and the same code.

The organising principle is that **the console never solves anything**. Poses are
authored offline and cooked to ARKit indices; the Xbox 360 adds vertex deltas and
nothing else.

## The pipeline

```
character.gltf ──► MeshBaker ──► .mesh v10 ──► spakc ──► 'MRPH' entry
   (ARKit shapes)   sparsify         morph block         (per-mesh companion)
                    quantise
                    reorder

.anim controller ──► animc ──► 'ANC1' v3 (expression poses)
```

At run time: the face layer resolves 52 ARKit weights → the deformer adds sparse
vertex deltas → the existing skinning palette runs unchanged.

## Why blendshapes, and why on the CPU

Facial rigs speak ARKit. Character exports (CC4, MetaHuman, Daz), MediaPipe's
landmarker and every viseme table all name the same 52 shapes, so keeping them as
the wire format end to end means a curve lands on a mesh by name with no lossy
conversion. Bone-driven faces are cheaper per frame but need a solver, lose the
fine shapes (lip corners, brow furrow, squint), and would crowd a joint budget
that is already at 72 with `gSkinPalette` occupying c8–c223.

Deforming on the CPU keeps **one code path for both targets and zero shader
changes** — no new vertex declaration, no new constant registers, no second
skinned variant to keep in step. It is affordable because the work is small:

- Targets are **sparse**. A jaw shape moves a fraction of a head, so only the
  vertices it actually touches carry a record.
- The affected vertices are **permuted into one contiguous range** at bake time,
  so a frame rewrites a single slice in one linear sweep.
- A resting face is **free**. When every weight is ~0 the deformed mesh *is* the
  base mesh, so the object draws the mesh's shared vertex buffer and nothing is
  written at all.

Deltas are quantised: position as `int16` scaled per target, normal as `int8`.
Tangents are deliberately not morphed — the normal map dominates surface detail
at face scale — and normals are not renormalised, because the pixel shader
normalises the interpolated normal anyway.

Morphing composes with skinning because the deltas are authored in the same bind
pose the palette expects. Morph first, then skin.

## Files

| Path | What |
|---|---|
| `src/anim/FaceShapes.h` | the ARKit-52 vocabulary; name → index folding |
| `src/anim/FaceDeform.h` | sparse delta application (shared, header-only) |
| `src/anim/FaceRuntime.{h,cpp}` | the layer: pose, blink, gaze (shared) |

## Formats

**Mesh (v10).** Meshes flagged `MESH_FLAG_MORPH` close with a morph block:
target count, the region's first vertex and length, then per target a name, its
ARKit index, a position scale and its sparse deltas. Targets that resolve to no
ARKit shape are **dropped at bake** — nothing downstream could drive them.

**`'MRPH'` companion**, keyed at `"<mesh path>#morph"` — the same shape a cooked
GIF uses for its frames. Kept out of the mesh payload so a character with no face
is byte-identical to before, and so morph data only loads for meshes that have
it. Requested after the mesh is resident, so the deltas always have vertices to
validate against.

**`'ANC1'` v3.** The controller gained a face block: the default pose and the
expression poses, resolved to ARKit indices at cook so the console never compares
a name.

## Budget

A morphing object needs a private clone of its vertex buffer (two, since the GPU
is a frame behind), because two characters sharing a mesh wear different
expressions. That is the whole mesh: about 2.9 MB for a 33k-vertex character. The console caps
concurrent morphing objects (default 4); past the cap an object falls back to the
shared base-pose buffer rather than pushing the streaming budget over.

The system-memory base pose and the deltas are added to the streaming residency
budget when the companion attaches, so a face is not invisible to it.

## Gotchas

- **Only the skinned bake path captures blendshapes.** The static path re-imports
  with `aiProcess_PreTransformVertices`, which discards anim meshes.
- **Sparsification is relative to model size**, because exporters disagree about
  units (metres for glTF, centimetres for much FBX).
- **The vertex permutation reorders the mesh.** Indices and the influence stream
  are remapped with it; a bake-time check rejects a permutation that is not a
  bijection, because that would corrupt static geometry too.
- **Delta records are cooked big-endian** (`u16` + 3×`i16` swapped, the four
  `i8` bytes not) so the console reads them natively. This is invisible on PC —
  `FaceMorphBakeTest` asserts it directly.
- **Gaze assumes ARKit's nose-relative naming**: looking to the character's left
  takes the left eye `Out` and the right eye `In`. A rig with those mirrored will
  cross its eyes.

## Tests

| Test | Covers |
|---|---|
| `face_morph_bake` | name folding, dropped non-ARKit targets, region contiguity, permutation validity, deform linearity, cook byte order |
| `face_layer` | pose snap/ease/weight, blink firing, gaze mapping and ease-back |
| `animator_cooker` | the v3 face block: poses and their ARKit indices |
