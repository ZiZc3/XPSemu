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
- Built-in 60 FPS patches with unlocks (Doom 3, Half-Life 2, and much more)
- FPS cap and PAL 60 Hz per game

<details open>
<summary><strong>&nbsp; Dashboard</strong></summary>
<br>

- Xbox-style home screen, 3D game shelf, covers downloaded by themselves
- Games search, play time, recently played
- Per-game settings for every option
- Home-screen shortcuts: any game as its own PS5 tile
- XISO, Redump, CCI and game folders, from the PS5 or a USB drive
- Widescreen and 60 FPS patches applied by themselves
- Up to 4 players

</details>

---

## Install

You need a PS5 with **kstuff** running, and **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**.

1. Copy `PPSA97358/` to `/data/homebrew/`.
2. Put your games in `/data/xpsemu/games/`, or in `xpsemu/games/` on a USB drive. (`xemu/games` folders from Alpha 2 still work too.)
3. Launch **XPSemu** from the home screen.

**Updating from Alpha 2.1?** Replace the `PPSA97358` folder. Your games stay where they are. Alpha 3 starts with a fresh Xbox hard disk in `/data/xpsemu/`.

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

```sh
bash tools/deps.sh                                  # downloads glslang, volk, VMA, SPIRV-Reflect, libslirp, nv2a_vsh_cpu
XOG_PREFIX=<curl+mbedTLS> RELEASE=1 bash tools/build.sh   # the app, in dist/PPSA97358
```

Without `RELEASE=1` it builds a test app (PPSA97360) that sits beside the real one.

---

## Credits

- **[hakuX](https://github.com/rfandango/hakuX)** by rfandango: the Xbox core XPSemu 3 is built on (kernel, graphics, sound) and its frame rate mods
- **[xemu](https://github.com/xemu-project/xemu)**: the MCPX sound emulation inside hakuX, XPSemu's roots, and the cover database
- **[Mihawk-99](https://github.com/mihawk-99)**: [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) / [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (RADV on the PS5) and [MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen) frame generation
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)**, **[kstuff](https://github.com/EchoStretch/kstuff)**, **[etaHEN](https://github.com/etaHEN/etaHEN)**, **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** and the PS5 scene
- **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** by Swordpdf: frame pacing ideas
- **Jay's Magic Patches / [JayXbox PatchHub](https://www.jayxbox.com/Retail-Game-Modification/PatchHub.php)**: widescreen and 60 FPS patches
- **[SMAA](https://github.com/iryoku/smaa)** (MIT), **[Snapdragon GSR](https://github.com/SnapdragonGameStudios/snapdragon-gsr)**, **[Dear ImGui](https://github.com/ocornut/imgui)**, **[glslang](https://github.com/KhronosGroup/glslang)**, **[volk](https://github.com/zeux/volk)**, **[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)**, **[SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect)**, **[xxHash](https://github.com/Cyan4973/xxHash)**, **[nv2a_vsh_cpu](https://github.com/abaire/nv2a_vsh_cpu)**, **[stb_image](https://github.com/nothings/stb)**, **[fpng](https://github.com/richgel999/fpng)**, **[curl](https://github.com/curl/curl)**, **[mbedTLS](https://github.com/Mbed-TLS/mbedtls)**, **[zlib](https://github.com/madler/zlib)**
- Icons: **[Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons)** (MIT), **[Hugeicons](https://hugeicons.com)** (MIT), **[Kenney Input Prompts](https://kenney.nl/assets/input-prompts)** (CC0)
- Covers and art: **[xemu's xdb](https://github.com/xemu-project/xdb)**, **[libretro thumbnails](https://github.com/libretro/libretro-thumbnails)**, **[LaunchBox Games Database](https://gamesdb.launchbox-app.com)**

XPSemu's icon and art are original (a green "X"), not the Xbox logo.

---

## License

GPL-3.0. Third-party parts keep their own licenses (see `licenses/`).

XPSemu includes no Microsoft BIOS, dashboard, game or other Xbox software. Use your own games, dumped from discs you own.
