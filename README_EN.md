# TriangleBin-MultiPlatform

English | [中文](README.md)

A multi-platform collection of ports of [TriangleBin](https://github.com/Swung0x48/TriangleBin), the GPU fill-rate and shader-order test:

- **Windows DirectX** — Direct3D 11 and 12, x86 / x64 / ARM64, Windows 7 and later
- **Windows Vulkan** — Vulkan 1.0, x86 / x64 / ARM64, Windows 7 and later with a Vulkan-capable driver
- **macOS Metal** — universal app for Intel (macOS 10.15+) and Apple Silicon (macOS 11.0+)

This repository contains the Windows DirectX source tree and publishes all three builds in [Releases](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases). The rest of this document describes the Windows DirectX port and its build.

It draws a stack of overlapping triangles and counts how many fragments the GPU actually
shades. That count is what tells you whether your GPU is an **IMR** (immediate-mode
renderer, shades every covered pixel of every triangle) or a **TBR** (tile-based renderer,
bins geometry per tile and shades each pixel roughly once).

![TriangleBin DirectX showing the Controls window and the Select GPU window](docs/screenshot.png)

## Download

The latest release is [v1.1.0](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases/tag/v1.1.0):

| Platform | Release asset |
|---|---|
| Windows DirectX | [TriangleBin-Windows-DirectX-v1.1.0.exe](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases/download/v1.1.0/TriangleBin-Windows-DirectX-v1.1.0.exe) |
| Windows Vulkan | [TriangleBin-Windows-Vulkan-v1.1.0.exe](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases/download/v1.1.0/TriangleBin-Windows-Vulkan-v1.1.0.exe) |
| macOS Metal | [TriangleBin-macOS-Metal-v1.1.0.zip](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases/download/v1.1.0/TriangleBin-macOS-Metal-v1.1.0.zip) |

The Windows DirectX launcher is a single file for every architecture: it detects the host CPU,
runs the matching x64 / x86 / ARM64 build, and cleans up after itself when it exits. No
installer, nothing written to the registry. It runs on Windows 7 and later. Direct3D 12 is
used automatically on Windows 10/11 with a capable GPU; on older systems the app falls back
to Direct3D 11. All shaders are embedded at build time, so no `d3dcompiler_47.dll`
installation is required.

The Windows Vulkan launcher is also a single file for x86 / x64 / ARM64. It targets
Windows 7 and later, uses Vulkan 1.0 core with `VK_KHR_swapchain`, and requires the
graphics driver's Vulkan loader.

The macOS Metal download is a zip containing `TriangleBin.app`, a universal binary for Intel and
Apple Silicon. If Gatekeeper blocks the first launch, right-click the app and choose **Open**.

To build the Windows DirectX source yourself instead, see [Building](#building).

## Relationship to the original

This is a multi-platform port collection, not a fork in the "small patch" sense. The
idea, the controls and the measurement are taken from [TriangleBin](https://github.com/Swung0x48/TriangleBin) by
Swung 0x48 (MIT). The Windows DirectX renderer, build system and platform support were
rewritten here; the Windows Vulkan and macOS Metal builds are published in [Releases](https://github.com/ddYIbb/TriangleBin-MultiPlatform/releases).

| | [TriangleBin](https://github.com/Swung0x48/TriangleBin) | TriangleBin-MultiPlatform (this repository) |
|---|---|---|
| Graphics API | OpenGL / OpenGL ES | Windows DirectX 11/12, Windows Vulkan 1.0, macOS Metal |
| Platforms | Android (APK) + Windows x64 | Windows: x86 / x64 / ARM64; macOS: Intel / Apple Silicon |
| Build system | Gradle + Android NDK | CMake + MSVC (Windows DirectX source); Vulkan / macOS builds in Releases |
| GPU selection | — | DirectX version can switch DXGI adapters at runtime (F2) |
| Failure diagnostics | — | writes a report on startup failure or crash |

## What this port adds

**Automatic API selection.** Direct3D 12 is tried first; if device creation fails the
app falls back to Direct3D 11 and says so in the log. There is no manual override —
the original only offered automatic selection too.

**Select GPU window (F2).** Every hardware DXGI adapter is listed with its driver
version, and marked `(D3D11 only)` when it cannot do D3D12. This matters on laptops
that have both an integrated and a discrete GPU: the adapter that Windows hands out by
default is not always the one you want to measure. `Auto (best GPU for this app)` keeps
the default behaviour. Switching adapters recreates the device, so the UI debounces
rapid clicks into a single switch. If the same physical GPU is enumerated more than
once by a driver or virtual display adapter, the list merges entries by PCI identity
(VendorId / DeviceId / SubSysId / Revision) and driver version.

**Diagnostic report.** On a fatal startup error (for example no DirectX 11 capable
device) or an unhandled exception, the app writes `trianglebin-error-report.txt` next
to the executable. It contains the OS version and build number, CPU architecture, GPU
name, driver version, the highest supported DirectX feature level, and the exact error.
Sending that one file back is enough to diagnose most startup crashes.

**Runtime value readout.** The same information the original displayed via
`glGetString(...)` is now labelled with the DirectX call it came from: `DXGI GetDesc`
for the adapter, `DXGI CheckInterfaceSupport` for the driver version, and
`D3D12CreateDevice` / `D3D11CreateDevice` for the device.

**ARM64 support.** The x86, x64 and ARM64 configurations share one source tree. ARM64
links SDL2 statically (`deps/SDL2/lib/arm64/SDL2-static.lib`) because SDL2's shared
library cannot be linked for MSVC ARM64; `src/sdl2_atomic_shim.c` supplies the missing
`_Interlocked*` symbols.

**Single-file launcher.** `tools/launcher` builds one executable with the x64, x86 and
ARM64 payloads embedded as resources. It extracts the build matching the host CPU and
runs it, so a release is a single `.exe` rather than one archive per architecture.

**Performance and memory.** Both backends use `D16_UNORM` for the depth buffer, which
still resolves the 0–200 triangle layers while halving depth-buffer memory. D3D11
caches one RTV per back buffer instead of creating it every frame, and D3D12 skips
re-uploading the random vertex buffer when the triangle count has not changed.

## Controls

| Control | Meaning |
|---|---|
| `Rendered/Screen %` | Fragments rendered / screen pixel count, as a percentage. Range 0–500. |
| `0%` / `100%` | Jump straight to 0% or 100%. |
| `Tris` | Number of random triangles to draw, 0–200. The two fullscreen triangles are not counted. |
| `Auto Increment` | Increase `Rendered/Screen %` automatically every frame. |
| `ppf` | Percent per frame — how much `Rendered/Screen %` grows while Auto Increment is on. Range 0–10. |
| `Frag Count` | Fragments the GPU actually shaded for the current frame. It should stay at or below the requested target. |
| `clear color` | Background colour. |
| `F2` | Open or close the Select GPU window. |

How to read it: raise `Tris` and watch `Frag Count`. If the count keeps climbing with
the triangle count, the GPU is shading every covered pixel of every triangle — that is
IMR behaviour. If it plateaus well below the target, the GPU is binning geometry and
shading each pixel once per tile — that is TBR behaviour.

## Building

Requirements:

- Windows 10 or 11
- Visual Studio 2022 or newer with the **Desktop development with C++** workload
- CMake 3.20 or newer (the copy bundled with Visual Studio works)

SDL2, Dear ImGui and GLM are vendored under `deps/`, so no package manager is needed.

```bash
git clone https://github.com/ddYIbb/TriangleBin-MultiPlatform.git
cd TriangleBin-MultiPlatform

cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

If several Visual Studio versions are installed and CMake picks the wrong one,
name the generator explicitly with `-G "Visual Studio 17 2022"` (or
`-G "Visual Studio 18 2026"`).

The executable lands in `build-x64/demo-dx.exe`. Copy `deps/SDL2/lib/x64/SDL2.dll` next
to it before running.

The other architectures use the same commands with a different generator platform and
output directory:

```bash
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release

cmake -S . -B build-arm64 -A ARM64
cmake --build build-arm64 --config Release
```

The ARM64 build links SDL2 statically and therefore needs no `SDL2.dll`.

### Launcher

`tools/launcher` embeds the three executables and the two needed `SDL2.dll` files
as resources. Configure it only after all three builds above have produced their
`demo-dx.exe`, since the resource compiler reads those files at build time:

```bash
cmake -S tools/launcher -B tools/launcher/build -A Win32
cmake --build tools/launcher/build --config Release
```

The result is `tools/launcher/build/demo-dx.exe`, a single portable executable.
It is deliberately built as Win32 so that the one file runs on every Windows
machine: x86 and x64 natively, ARM64 through the built-in emulation.

`launcher.rc.in` is a template — `CMakeLists.txt` fills in the absolute path of
each payload at configure time and writes `launcher.rc` into the build
directory. Nothing machine-specific is stored in the repository.

## Third-party components

| Component | Version | Licence |
|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | 2.30.11 | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | — | MIT |
| [GLM](https://github.com/g-truc/glm) | — | MIT |

## Licence

MIT — see [LICENSE](LICENSE).

Based on [TriangleBin](https://github.com/Swung0x48/TriangleBin) by Swung 0x48,
licensed under the MIT Licence. The original copyright notice is preserved in
`LICENSE`.
