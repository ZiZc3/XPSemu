<p align="center">
  <img src="assets/xpsemu-logo.png" alt="XPSemu" width="140">
</p>

<h1 align="center">XPSemu</h1>

<p align="center">
  <strong>The original Xbox, running natively on a PlayStation 5.</strong><br>
</p>

<p align="center">
  <code>Alpha 3.0</code> &nbsp;&middot;&nbsp;
  <code>GPL-3.0</code>
</p>

---

## What is XPSemu?

XPSemu is an original Xbox emulator for the PlayStation 5, running as a native app.

The PS5 and the original Xbox both have **x86 CPUs**. So instead of translating every Xbox instruction (slow, Pre-Alpha 3.0), XPSemu **lets the game's code run directly on the PS5's CPU**. Only the few instructions that need the Xbox's hardware are caught and handled.

Around that sits the rest of an Xbox, from the [hakuX](https://github.com/rfandango/hakuX) core: its own Xbox kernel, the NV2A graphics drawn through Vulkan on an open-source driver (RADV, from [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), and the MCPX sound.

---

## Features

<details open>
<summary><strong>&nbsp; Picture</strong></summary>
<br>

- Resolution x1 to x6 (up to 3840x2880, shown at 4K)
- Anti-aliasing: FXAA, SMAA, SSAA
- Sharp (SGSR) or Smooth upscaling

</details>

<details open>
<summary><strong>&nbsp; Frame rate</strong></summary>
<br>

- **Frame Generation**: Off, Auto, x2, x3, x4. Auto takes 30 FPS games to 60 and 60 to 120
- **120 Hz** for games that run at 120 with patches.
- Built-in 60 FPS patches (Doom 3, Half-Life 2, and much more)
- FPS cap and PAL 60 Hz per game

</details>

<details open>
<summary><strong>&nbsp; Dashboard</strong></summary>
<br>

- Xbox-style home screen, 3D game shelf, covers downloaded by themselves
- Games search, play time, recently played
- Per-game settings for every option
- Home-screen shortcuts: any game as its own PS5 tile
- XISO/Split XISOs, Redump, CCI, CHD, CSO and game folders, from the PS5 or a USB drive
- Widescreen and 60 FPS patches applied by themselves
- Up to 4 players

</details>

---

## Sega Chihiro

The Sega Chihiro is an arcade board built on the original Xbox, so its games run on the same native core, at 60 FPS, each with controls made for its cabinet. **No Chihiro BIOS and no media board flash are needed** — only your game and three small base board files.

**Playable now (all 8):**

| Game | Best format |
|---|---|
| **OutRun 2** | GD-ROM |
| **Crazy Taxi: High Roller** | CHD |
| **Ollie King** | CHD |
| **Virtua Cop 3** | Netboot `.bin` |
| **Ghost Squad** | Netboot `.bin` |
| **The House of the Dead III** | Netboot `.bin` |
| **Wangan Midnight Maximum Tune** | CHD |
| **Wangan Midnight Maximum Tune 2** | CHD |

See **[Chihiro controls](Chihiro-Controls.md)** for each game's layout.

### Adding Chihiro games

Put your games in `/data/xpsemu/chihiro/games/`, or in `xpsemu/chihiro/games/` on a USB drive.

| Format | Keys needed? | |
|---|---|---|
| **Netboot image** (`.bin`) | No | Already unlocked and opens fastest. 512 MB and 1 GB images both work. |
| **CHD** (`.chd`, MAME GD-ROM) | Yes | Compressed, the smallest on disk. |
| **GD-ROM dump** (`.gdi` or `.cue` with its tracks) | Yes | |

**Keys** (CHD and GD-ROM only): the game's security key as a `.pic` or `.data` file, beside the game or in `/data/xpsemu/chihiro/`. A MAME key `.zip` works too, with no need to extract it.

> [!IMPORTANT]
> **Three base board files are always needed**, in `/data/xpsemu/chihiro/`:
> - `ic10_g24lc64.bin`
> - `pc20_g24lc64.bin`
> - `ic11_24lc024.bin`
>
> The Chihiro tab shows **"Base board files: OK"** when they're found. **No Chihiro BIOS and no media board flash are needed.**

Each game keeps its own backup RAM (high scores, settings) in `/data/xpsemu/chihiro/run/`, so your records are per game as in a real cabinet. After the first start, games skip the SEGA boot screen and start directly.

---

## Install

You need a PS5 with **kstuff** running, and **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**.

1. Copy `PPSA97358/` to `/data/homebrew/`.
2. Put your games in `/data/xpsemu/games/`, or in `xpsemu/games/` on a USB drive. (`xemu/games` folders from Alpha 2 still work too.)
3. Launch **XPSemu** from the home screen.

---

## Controls

| DualSense | Xbox |
|---|---|
| ✕ ○ □ △ | A B X Y |
| L1 / R1 | White / Black |
| L2 / R2 | Triggers |
| OPTIONS / Create | Start / Back |
| **Touchpad** | Quick menu |

On the dashboard: **△** on a game opens its settings, **OPTIONS** searches your games.

---

## Building

**Toolchain:** PS5 payload SDK and RADV from [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (built first), on Linux or WSL. curl and mbedTLS built for the PS5.

---

## Credits

- **[hakuX](https://github.com/rfandango/hakuX)** by rfandango: the Xbox core XPSemu 3 is built on (kernel, graphics, sound), its frame rate mods and its controller rumble path
- **[xemu](https://github.com/xemu-project/xemu)**: the MCPX sound emulation inside hakuX, XPSemu's roots, and the cover database
- **[XPSemu-Chihiro](https://github.com/tattoohanz/XPSemu-Chihiro)** by tattoohanz (LGPL-2.1): studied as a reference for the Sega Chihiro hardware. Its hardware-model approach informed our own implementation of the **Namco V257 drive board**, the **CRP-1231 card reader**, and the **media-board network firmware answer** used for Maximum Tune, as well as the base boards (8051 / AN2131, JVS), game controls and steering-wheel force feedback. Our Chihiro code is our own and is GPL-3.
- **[Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)**: how Chihiro games start without a BIOS
- **[MAME](https://github.com/mamedev/mame)** (Olivier Galibert, Réda Chérif-Touil): the Chihiro GD-ROM DES decryption (BSD-3-Clause)
- **[Mihawk-99](https://github.com/mihawk-99)**: [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) / [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (RADV on the PS5) and [MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen) frame generation
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)**, **[kstuff](https://github.com/EchoStretch/kstuff)**, **[etaHEN](https://github.com/etaHEN/etaHEN)**, **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** and the PS5 scene
- **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** by Swordpdf: frame pacing ideas
- **Jay's Magic Patches / [JayXbox PatchHub](https://www.jayxbox.com/Retail-Game-Modification/PatchHub.php)**: widescreen and 60 FPS patches
- **[MakeMHz](https://github.com/MakeMHz)**: the CSO image layout (ciso.py)
- **[SMAA](https://github.com/iryoku/smaa)** (MIT), **[Snapdragon GSR](https://github.com/SnapdragonGameStudios/snapdragon-gsr)**, **[AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS)** (MIT), **[Dear ImGui](https://github.com/ocornut/imgui)**, **[glslang](https://github.com/KhronosGroup/glslang)**, **[volk](https://github.com/zeux/volk)**, **[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)**, **[SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect)**, **[xxHash](https://github.com/Cyan4973/xxHash)**, **[nv2a_vsh_cpu](https://github.com/abaire/nv2a_vsh_cpu)**, **[stb_image](https://github.com/nothings/stb)**, **[fpng](https://github.com/richgel999/fpng)**, **[curl](https://github.com/curl/curl)**, **[mbedTLS](https://github.com/Mbed-TLS/mbedtls)**, **[zlib](https://github.com/madler/zlib)**
- CHD support: **[libchdr](https://github.com/rtissera/libchdr)** (BSD-3-Clause), with **[zstd](https://github.com/facebook/zstd)**, **[miniz](https://github.com/richgel999/miniz)**, **[LZMA SDK](https://www.7-zip.org/sdk.html)** and **[dr_flac](https://github.com/mackron/dr_libs)**
- Icons: **[Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons)** (MIT), **[Hugeicons](https://hugeicons.com)** (MIT), **[Kenney Input Prompts](https://kenney.nl/assets/input-prompts)** (CC0)
- Covers and art: **[xemu's xdb](https://github.com/xemu-project/xdb)**, **[libretro thumbnails](https://github.com/libretro/libretro-thumbnails)**, **[LaunchBox Games Database](https://gamesdb.launchbox-app.com)**
- Disc regions and languages: the Redump list from **[libretro-database](https://github.com/libretro/libretro-database)** (CC BY-SA 4.0)

XPSemu's icon and art are original (a green "X"), not the Xbox logo.

---

## License

GPL-3.0. Third-party parts keep their own licenses (see `licenses/`).

XPSemu includes no Microsoft BIOS, dashboard, game or other Xbox software. Use your own games, dumped from discs you own.
