# Lighting system

## Light attributes

- **Directional Light** — global direction from the object's forward (+Z), color,
  intensity, mode, and the one supported realtime shadow-map direction.
- **Point Light** — position, color, intensity, range, and mode. Runtime selects
  up to four relevant point lights per object.
- **Spot Light** — position, forward direction, color, intensity, range, inner
  angle, outer angle, and mode. Runtime selects up to two per object.
- **Environment Light** — color, intensity, and mode. Supplies ambient/baked
  environment energy; reflection cubemaps remain a separate material input.

The first Directional Light in scene order owns the global sun direction. Point
and spot selection is per-object rather than one fixed scene-wide list.

## Light modes

Every light has one of three contribution modes. Mode controls **where the light
energy is evaluated**, not whether models cast shadows.

### Baked (`0`)

The light contributes during `lightc` baking and does not add a separate runtime
forward-light term.

- Directional, point, spot, and environment energy may enter lightmaps/probes.
- Static direct lighting, one diffuse bounce, and probe lighting are generated
  offline.
- A Baked directional light may still provide the direction for the realtime
  shadow map. This lets moving, explicitly enabled casters attenuate the
  directional part of baked lighting on static receivers.

### Realtime (`1`)

The light is excluded from `lightc` and evaluated by the runtime shader.

- Directional diffuse and specular are evaluated every frame.
- Point and spot lights are packed per rendered object.
- Realtime environment energy enters the runtime ambient term.
- A Realtime directional light also renders and samples the shadow map. Shadow
  visibility attenuates its directional diffuse, material specular, metal glint,
  and clearcoat glint; point, spot, and environment terms are not shadowed by
  that map.

### Mixed (`2`)

The light participates in both systems.

- It is included by `lightc` like Baked.
- It also contributes runtime forward lighting like Realtime.
- A Mixed directional light uses the realtime shadow map for both its realtime
  directional term and the directional portion of baked lighting.

Use Mixed deliberately: it contains both baked and realtime energy rather than
being an automatic replacement for either mode.

## Directional lightmaps

Static bake output uses two textures:

- **`lm0`** — RGBM average illumination. RGB is multiplied by alpha and the fixed
  range **16** during decode.
- **`lm1`** — dominant light direction in RGB and directionality in alpha.

The runtime reconstructs baked lighting as:

```text
directional = (1 - directionality)
            + directionality * 2 * saturate(dot(normal, dominant)) * shadow
baked       = decode_rgbm(lm0) * directional
```

Only the directional fraction is attenuated by the shadow map. Isotropic baked
and bounced energy remains visible in shadow, avoiding unnaturally black indirect
lighting.

Lightmap UVs are generated offline with xatlas and stored in `.lmuv` sidecars.
Scene bindings, probe data, atlas dimensions, and UV scale/offset values live in
`.lmap`. The source `.mesh` stays at format v9.

## Probes

Moving and model-selected dynamic objects do not sample static lightmap UVs.
They sample an SH-L1 probe grid baked around the scene:

- The nearest eight probes are blended trilinearly.
- SH coefficients are blended first, then collapsed into average color, dominant
  direction, and directionality.
- The editor reads probes from `.lmap`; the Xbox package carries the same grid as
  an `LPRB` entry (`.lprb`) in `game.spak`.
- Animator objects, skinned meshes, and non-static rigid bodies select probe
  lighting. This classification does **not** make them shadow casters.

Probe direct visibility honors `Cast Shadow`. Diffuse bounce still traces against
all static geometry so disabling direct shadow casting does not remove an
object's indirect-light transport.

## Occlusion

Occlusion keeps blocked light from passing through scene geometry. During a bake,
models with `Cast Shadow` enabled block direct Baked and Mixed light from reaching
lightmap texels and probes. This produces stable shadowed areas and lets moving
objects pick up those darker regions through the probe grid.

Diffuse bounce is evaluated against the full static scene rather than only shadow
casters. A model can therefore stop casting direct shadows without disappearing
from indirect lighting and color bleed.

The realtime directional shadow map adds moving occlusion on top of the baked
world. It attenuates directional light while preserving bounced and
non-directional fill, so shadows retain the surrounding room light instead of
becoming completely black.

## Realtime directional shadows

Directional shadows use a dedicated **1024 x 1024** render target and a 16-bit
depth surface. The map is separate from `lm0` and `lm1`:

- Editor: D3D9 render-to-texture.
- Xbox 360: EDRAM shadow pass followed by resolve to a texture.
- Shared shader: four point samples form a fixed **2 x 2 PCF** filter.
- Shadow-map X/Y projection is fitted to the world bounds of enabled casters,
  with 5% padding and a 0.25-unit minimum. This gives the caster the available
  texel density instead of fitting the whole baked scene.
- Receiver and caster bounds still contribute to near/far depth coverage.

Shadow generation is independent of Directional Light mode: Baked, Realtime,
and Mixed directional lights may all own the map. The model toggle is the sole
caster authority.

### `3D Model > Cast Shadow`

`Cast Shadow` defaults to **off** and is serialized as `castShadow` on the
`3D Model` attribute.

- Enabled models are submitted to the realtime shadow pass.
- Enabled static models enter the baker's direct/probe occlusion scene.
- Disabled models do not cast realtime or baked direct shadows.
- Rigid Body, Animator, skinning, and dynamic-lighting status never implicitly
  enable casting.
- Skinning only chooses the correct shadow vertex path after the toggle has
  admitted the model.

Realtime shadow receiving is currently restricted to static lightmapped
geometry. Dynamic/probe-lit objects cast shadows when enabled but do not receive
this directional shadow map.

## Baker

`lightc` performs these major steps:

1. Collect static geometry and lightmap UV sidecars.
2. Pack and rasterize world-space lightmap surfels.
3. Trace direct Baked/Mixed light visibility against the `Cast Shadow` scene.
4. Trace one diffuse bounce against the full static scene.
5. Bake the SH-L1 probe grid.
6. Encode `lm0`, `lm1`, `.lmap`, and probe output.

The baker treats authored static surfaces as two-sided for direct illumination by
orienting the shading normal toward each light. This supports imported planes
whose geometric winding points away from the light while preserving their baked
visibility.

A rebake is required after changing static transforms, baked/mixed lights,
`Cast Shadow` on static models, or bake-relevant geometry/material inputs.
Realtime-only light color, intensity, and transforms do not require a rebake.
