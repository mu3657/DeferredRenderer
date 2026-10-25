# Project-local RTXGI DDGI kernels

This software contains source code provided by NVIDIA Corporation.
The NVIDIA copyright notices are retained in each copied file; the accompanying
`License.txt` contains the SDK license.

## Origin and scope

Copied on 2026-10-19 from this repository's working copies under
`third_party/rtxgi-ddgi/rtxgi-sdk/`. The copies include the renderer adaptations
already present there, rather than an unmodified upstream SDK snapshot.
All 18 copied source/header files were checked for byte-for-byte equality at
the time of the copy. `License.txt` comes from `third_party/rtxgi-ddgi/License.txt`.

This directory is the active shader source used by the root `CMakeLists.txt`.
Make future kernel and shader-header changes here. The original third-party
shader files remain as the copy's origin and are no longer shader build inputs.
The C++ RTXGI library and its public C++ headers still come from `third_party`.

## Entry points

| Source | Entry point | Purpose |
| --- | --- | --- |
| `shaders/ddgi/ProbeBlendingCS.hlsl` | `DDGIProbeBlendingCS` | Compiled twice: irradiance and distance moments |
| `shaders/ddgi/ProbeClassificationCS.hlsl` | `DDGIProbeClassificationCS` | Active/inactive classification |
| `shaders/ddgi/ProbeRelocationCS.hlsl` | `DDGIProbeRelocationCS` | Probe position offsets |

Classification and relocation reset entry points are also compiled, matching the
previous build configuration. The runtime currently loads the four main pipelines
in `src/Renderpasses/ddgi_probe_blend_pass.cpp`; output SPIR-V names are unchanged.

## Dependencies and project adaptations

The original `shaders/` and `include/rtxgi/` relative layout is preserved so all
shader includes resolve within this directory. The three copied `.h` files are
the shader-facing definitions used with `HLSL=1`, not a standalone C++ SDK.
Keep their GPU ABI consistent with the C++ SDK headers if changing these layouts.

The copied kernels retain batched X/Y/Z-to-X/Z/Y indexing, bounded dynamic
lighting response, F32 irradiance blending adjustments, random-ray backface
classification, and inactive-history clearing. These adaptations are implemented
in the kernels and controlled by the existing CMake defines and root constants.

The renderer's Ray Query tracing, deferred GI sampling, Vulkan resource ownership,
barriers, and GPU budget scheduling remain in their existing project files.
