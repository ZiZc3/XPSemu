# XPSemu third-party licenses

XPSemu itself is **GPL-3.0**. It includes no Microsoft or Sega BIOS, dashboard,
media board, game or other first-party software. The files in this folder are
the licenses of the third-party parts XPSemu is built on or adapts. Each one
keeps its own license; all are compatible with GPL-3.0.

## Core and emulation

| File | Covers |
|---|---|
| `GPL-3.0.txt` | XPSemu's own license, and hakuX |
| `GPL-2.0.txt` | GPL-2-or-later parts inside the MCPX sound core (DSP56300, SVF) |
| `LGPL-2.1.txt` | The MCPX APU devices (from xemu), and the **XPSemu-Chihiro** fork code whose approach the Chihiro work adapts (see below) |
| `hakuX.txt` | hakuX and the per-file licenses of the parts it bundles |
| `MihawkGen.txt` | MihawkGen frame generation |

## Sega Chihiro

| File | Covers |
|---|---|
| `XPSemu-Chihiro-tattoohanz.txt` | Study / reference credit for the tattoohanz **XPSemu-Chihiro** fork (LGPL-2.1+), whose hardware-model approach informed our Namco V257 drive board, CRP-1231 card reader and the media-board network firmware answer for Maximum Tune. Our Chihiro code is our own (GPL-3); the fork's code it adapts is under `LGPL-2.1.txt`. |
| `MAME-DES.txt` | The Chihiro GD-ROM DES decryption, ported from MAME (BSD-3-Clause) |

## Picture, video and upscaling

| File | Covers |
|---|---|
| `FidelityFX-CAS.txt` | AMD FidelityFX CAS sharpening (MIT) — new in 3.2 |
| `SMAA.txt` | SMAA anti-aliasing (MIT) |
| `snapdragon-gsr.txt` | Snapdragon Game Super Resolution (Sharp upscaling) |

## Disc formats (CHD / CSO)

| File | Covers |
|---|---|
| `libchdr.txt` | CHD reading (BSD-3-Clause) |
| `zstd.txt`, `miniz.txt`, `LZMA-SDK.txt`, `dr_flac.txt` | libchdr's dependencies |

## Graphics / Vulkan toolchain

| File | Covers |
|---|---|
| `DearImGui.txt` | Dear ImGui (the UI) |
| `glslang.txt`, `volk.txt`, `VulkanMemoryAllocator.txt`, `SPIRV-Reflect.txt` | Vulkan shader and memory tooling |
| `nv2a_vsh_cpu.txt` | NV2A vertex-shader helper |
| `rpmalloc.txt`, `unordered_dense.txt`, `range-v3.txt`, `fmt.txt`, `xxHash.txt`, `SoftFloat-3e.txt`, `cephes.txt` | FEX and its dependencies |
| `FEX.txt`, `SDL3.txt`, `adrenotools.txt` | Bundled runtime libraries |

## Network, crypto and misc

| File | Covers |
|---|---|
| `curl.txt`, `mbedTLS.txt` | HTTPS (covers and the in-app updater) |
| `zlib.txt` | zlib |
| `adpcm-xq.txt` | ADPCM decoder inside the APU |

## Icons and input art

| File | Covers |
|---|---|
| `Hugeicons.txt` (MIT), `FluentUI-System-Icons.txt` (MIT), `Kenney-Input-Prompts.txt` (CC0) | UI icons and controller glyphs |

> Covers, regions and languages come from community databases (xemu's xdb,
> libretro thumbnails, LaunchBox, and the Redump list from libretro-database,
> CC BY-SA 4.0). See `Credits.md` for links.
