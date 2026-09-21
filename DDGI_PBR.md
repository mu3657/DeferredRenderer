# DDGI and PBR implementation notes

This renderer keeps one glTF metallic-roughness material contract across the
raster GBuffer, transparent forward lighting, shadow maps, and DDGI Ray Query
hits. The goal is to make differences between raster and indirect-lighting
results diagnosable instead of hiding them behind pass-specific assumptions.

## Frame path

1. The asset loader resolves linear/sRGB texture intent and uploads five
   textures into the global Bindless array: base color, metallic-roughness,
   normal, occlusion, and emissive.
2. CPU-side tangent generation produces a 64-byte vertex ABI with explicit
   handedness for diagnostics. Normal mapping is enabled only for assets marked
   `tangentSpaceReady`; legacy PNCV assets use their reliable geometry normals.
3. The GBuffer writes base color, world normal plus metallic, roughness plus AO,
   and a dedicated RGBA16F emissive target. Normal derivatives widen
   perceptual roughness where necessary to suppress sub-pixel GGX sparkle.
4. Deferred and transparent lighting share `shaders/pbr.glsl`: GGX NDF,
   height-correlated Smith visibility, Schlick Fresnel, and diffuse energy
   conservation.
5. DDGI Ray Query uses the same alpha-mask, legacy two-sided geometry,
   Bindless material, geometry-normal/tangent-readiness rule, and emissive
   semantics before writing probe ray radiance.
6. RTXGI probe blending updates irradiance/distance atlases in amortized probe
   batches. Deferred lighting applies the receiver material's Fresnel-aware
   diffuse energy and AO when composing DDGI irradiance.

## Material ABI

`MaterialConstants` and the matching GLSL `MaterialData` are 272-byte SSBO
records. Important fields are:

| Field | Meaning |
| --- | --- |
| `colorFactors` | linear base-color factor and alpha |
| `metal_rough_factors.xy` | metallic and perceptual roughness factors |
| `metal_rough_factors.zw` | normal scale and occlusion strength |
| `emissive_factors.rgb` | linear emissive factor |
| `emissive_factors.w` | alpha cutoff for MASK materials |
| `materialFlags` | alpha-mask, double-sided, and validated tangent-space bits |

MASK materials use fragment discard in both GBuffer and shadow pipelines, and
the same cutoff is evaluated while confirming Ray Query candidates. Bistro and
other legacy baked scenes remain two-sided in raster and Ray Query because their
winding is not yet a trustworthy authored single-sided contract.

## Runtime validation

The `PBR Material Debug` window isolates final lighting, base color, world
normal, metallic, perceptual roughness, ambient occlusion, and HDR emissive.
It also exposes signed normal/view hemisphere (green = facing camera, red =
rejected by the BRDF) and direct lighting alone. Legacy raster shading preserves
authored normals instead of unconditionally flipping them with gl_FrontFacing;
the latter was introduced during this change and conflicts with the unvalidated
winding of the legacy two-sided asset path. The user confirmed that removing
this inversion fixed the near-black image and isolated bright-speck regression.
DDGI windows separately expose trace modes, atlas/history progress, confidence,
irradiance heatmaps, and final indirect contribution.

Tiled lighting, DDGI composition, and emissive contribution default to enabled
again after the normal regression was isolated. Their UI switches remain
available for independent comparisons. Restoring defaults is not visual sign-off
of the combined result. Shared GLSL includes now invalidate shader build outputs,
so edits to pbr.glsl and material layouts cannot silently leave stale binaries.

Current acceptance checks:

- `cmake --build cmake-build-debug --config Debug` compiles all GLSL/HLSL and
  links `bin/engine.exe` in an MSVC developer environment.
- After restoring the defaults, the Debug build and `git diff --check` passed;
  `spirv-val --target-env vulkan1.3 --scalar-block-layout` also passed for seven
  affected modules: GBuffer vertex/fragment, deferred, transparent, DDGI trace,
  shadow vertex, and masked shadow fragment. These are structural checks.
- Bistro startup registers and builds 1,681 BLAS/geometries/instances and the
  TLAS without an ABI or pipeline creation failure.
- Observed DDGI batches report 1,024/1,024 non-zero radiance rays,
  512/512 non-zero irradiance texels, and zero non-finite values.

These GPU diagnostics prove the trace-to-atlas data path, not final image
quality. Final visual sign-off still requires comparing the PBR debug outputs,
DDGI composite, and an external frame capture on the target GPU.

## Honest scope boundary

The current validation status is intentionally narrower than the implementation:

- User-confirmed: the authored-normal raster baseline fixes the reported black
  image and speck regression with the new direct-lighting BRDF in place.
- Code-connected, final visual comparison pending: HDR emissive composition,
  alpha-mask raster/shadow/ray consistency, and DDGI receiver diffuse response.
- Not validated: tangent-space normal mapping (legacy materials keep it disabled),
  transparent material parity, and specular-antialiasing quality.
- DDGI hit lighting remains a simplified Lambertian diffuse transport model;
  only deferred and forward direct lighting share the complete GGX BRDF.
  DDGI is diffuse GI, not a specular-reflection solution.

For runtime acceptance, hold camera, lights, and exposure fixed; compare emissive
off/on at an emissive material, then compare DDGI composition off/on after probe
history has converged. Use indirect-only and confidence outputs to distinguish
missing probe coverage from material response. Test MASK using a known cutout
surface and its shadow. Do not infer any of these results from build success.

Probe relocation and classification kernels are compiled but remain disabled
until their 32 fixed-ray schedule can be integrated with the renderer's partial
probe-batch scheduler. The active implementation uses visibility moments,
normal/view bias, back-face diagnostics, hysteresis, and multi-bounce history;
it should not be described as a complete RTXGI feature port.

## Resume-ready summary

Implemented a Vulkan 1.3 hybrid raster/Ray Query material pipeline with a
Bindless glTF metallic-roughness ABI shared by deferred, transparent, shadow,
and DDGI passes; added a guarded tangent-space path with geometry-normal
fallback, alpha-tested shadow/ray traversal, HDR emissive GBuffer storage,
energy-conserving GGX lighting with specular antialiasing, and GPU diagnostics that validate probe
radiance/atlas writes without overstating final visual quality.
