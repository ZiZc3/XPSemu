# Handoff: v1.0.0-prepare, 2026-10-22

Repository `D:\code\xemu-android-native`, branch `v1.0.0-prepare` (master
is the release line). Read `CLAUDE.md` (device etiquette, build steps;
local-only, in `D:\code\xemu-android`) and `tools/hle/README.md` (tools and
debug knobs; local-only) first.

## State of the branch

- `v1.0.0-prepare` is v0.3.1, then the new core squashed into one commit
  (`405b7063a9` "new core"), then the core review's fixes, one commit per
  fix.
- The review: the Claude Doc "hakuX new core — technical debt and
  performance review" (https://claude.ai/artifact/S47Pw9aYhRbgdbpcnK4bYi).
  It has 65 findings (P1-P37 performance, D1-D30 debt and risk) and a
  37-item plan in Now / Next / Later tables with a Status column. Its
  Progress table lists every fix's commit and how it was checked on a
  device. Read it before taking the next item; its Status column is the
  source of truth.
- Done: items 1 to 8 (APU index checks, AAudio recovery, GPU device loss,
  decoder quick wins, kernel time and IRQL fast paths, pad input, the
  Turnip EQUAL-depth quirk, the safety fixes). P4 (narrower barriers) was
  measured with no gain on Turnip and dropped.
- In progress: item 9 (tools tracked, `.gitattributes`, docs, stale perf
  maps). Next: item 10 (appCategory game, backup rules), then the Next
  table.
- Not committed, on purpose: the DOA3 120 fps mod (`mods/4D53002D.1/`,
  `tools/mods/doa3_*`). It does not work properly; never commit it.
- Savestates restore the guest's kernel area, its guest-code fast paths
  included (the IRQL exports, item 5): a change to them shows only on a
  fresh boot or in a state saved with the new build.

## Devices

The Odin 3 (`adb -s 3458d5f`, Turnip, 120 Hz) is the main test device; the
RP5 (`adb -s 61705c55`, Turnip, 60 Hz) only when the user isn't playing on
it. Write `.device.lock` (session, time, device, purpose) while using one,
remove it after, and reset what was set (`setprop debug.hakux.state ''`,
`debug.hakux.quirks ''`). The Galaxy Tab S9 is for 120 Hz checks of the
DOA2 mod (below).

## Build and check

- Build: `cmake --build build/native/core --target hakuxcore`, then
  `bash tools/native/app-libs.sh` (the libraries into `jniLibs`; without it
  Gradle packs the old core), then `android/gradlew assembleDebug`. First
  configure: `android/README.md`.
- Savestates from adb: `setprop debug.hakux.state save:<slot>` or
  `load:<slot>` (a new value each time). Use a slot the user doesn't have.
- GPU time: `setprop debug.hakux.perf <new value>` logs `hakuX-hle-prof:
  gpu=` once a second for `debug.hakux.perfsec` seconds (10 by default).
  For A/B in one session, flip a quirk live with `debug.hakux.quirks`
  (`+name` / `-name`): separate launches of the same slot drift.
- CPU: `simpleperf record --app com.rfandango.haku_x.debug -e cpu-clock -f
  4000 --duration 10`, reported with `--symfs` holding the unstripped
  `build/native/core/libhakuxcore.so` of the installed build.
- Savestate layout: a change to a saved struct needs its `.def` entry
  (`NC_F`, or `NC_SKIP` for what isn't saved), then
  `python tools/native/statelock.py --sizes`; the build checks it.

## Paused: DOA2 120 fps mod (`mods/54430006.6/120fps`)

Paused on 2026-10-20 for another task. The user's request: a 120 fps mod
for Dead or Alive 2 Ultimate (`54430006.6`, `doa2.xbe`), made as a mod
(XBE patches; not an emulator feature), the game logic at its normal 60,
the frames in between blended ("interpolate at 120").

**Devices.** 120 Hz checks on the Galaxy Tab S9 (`adb -s
192.168.4.25:40381`, a 120 Hz panel). The game is on its SD card; launch it
with `am start -a android.intent.action.VIEW -d
'content://com.android.externalstorage.documents/tree/5A5D-CCFB%3AROMs%2Fxbox/document/5A5D-CCFB%3AROMs%2Fxbox%2FDead%20or%20Alive%202%20Ultimate%20(USA%2C%20Japan)%20(En%2CJa).iso'
-n com.rfandango.haku_x.debug/com.rfandango.haku_x.LauncherActivity`, then
`setprop debug.hakux.state load:1` after ~30 s. Keep it awake
(`input keyevent KEYCODE_WAKEUP`); when it locks, the user has to unlock
it. The Tab has a 3x render scale override for this game (`per_game_settings`
`54430006.6/render_scale`): at 4x the GPU heats up, the kernel caps it at
401 of 719 MHz (`/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel` 5) and frames
swing between 120 and 52; at 3x it held 120. The RP5 (60 Hz panel) serves
for reverse engineering and logic checks. Savestate slot 1 is the sparring
fight on both.

**What the mod does now** (`tools/mods/doa2_120fps.py <doa2.xbe> <mod.txt>`
writes `mod.txt`; the XBE is
`D:\code\xemu-android\build\hle-xbe\Dead_or_Alive_2_Ultimate__doa2.xbe`):
- `refresh: 120`: the vblank at 120 Hz.
- The game runs as fiber tasks (24 slots at 0xA94570, scheduler 0xFA440,
  current slot 0xA94814). A logic frame is the first main-loop pass of each
  60 Hz tick (D3D's vblank count at `[0x3554F8]+0x1DE8`, halved), decided
  at the controller update (call at 0xFF3AF): only logic frames read the
  controllers. On the other frames the scheduler (0xFA467) runs only the
  tasks that drew (the D3D draws set the slot's bit in DRAWN, 0x10CF0).
- The code sits in the header page (sizeof_headers 0xC68 -> 0x1000):
  variables at 0x10CF0 (DRAWN), 0x10CF4 (PARITY, 0 on logic frames),
  0x10CF8 (LASTTICK), 0x10E90 (PENDING); code at 0x10D00-0x10F0C. Free:
  0x10F10-0x10FFF.
- Tasks that run every frame without drawing (C_ALWAYS, by entry): the
  select screen's model loader (0x12F490, fed by 0x12F460; skipped, its
  fighters were drawn every other tick) and the fight's render setup
  (slot 12, 0x116B20: resets D3D's render state and the draw lists, sets
  the lights, queues the shadows; skipped, the ground under the fighters
  went darker every other tick). The frame counter 0x87D56C (bumps at
  0xD9364 and 0xDAF32) and the render setup's 0x850730 (0x5C46C) move on
  logic frames only, so a fighter drawn twice in one tick takes the
  game's repeat path (0x5B729).
- 2D lists (text, icons): tasks append items (0xB0590), the 2D task (slot
  15, entry 0x6D0B0) draws them (0xB0600) and resets them (0xAE300) at its
  frame's end. The select screen's task (slot 1, 0x13B800) doesn't draw,
  so it was skipped on the frames in between and its "A Select / B Back"
  legend flickered (fixed 2026-10-22): the reset now waits for the next
  logic frame's start (C_LOGIC, PENDING 0x10E90) and the frames in between
  drop their appends, drawing the logic frame's 2D again. Found with the
  force mask (below): slot 1 alone made it steady.
- Finding a task by experiment: a cave that also runs the slots set in a
  guest word (`bt [0x10CFC], slot`), then `setprop debug.hakux.memscan
  poke=10CFC:<mask>,<salt>` on the running game. One build, any number of
  masks.
- Verified on the Tab at 3x: 120 fps, the fighter's frame counter
  (0x8A3918) and the global logic counter (0x87D56C) at 60 a second, inputs
  register. Each logic frame is shown twice: motion still looks like 60.

**Next, in order.**
1. Draw-side updates still run every frame (twice per tick):
   - The game's own "this draw repeats the last frame" flag is the byte
     0x8783C9 (100+ readers). Draw code skips its CPU steps while it is
     set: seam copies (0x4E390), hair and cloth chains (0x591A0), ribbons
     (0x50510). Slot 13 (stage, entry 0x14C610, at 0x14C69F) and slot 14
     (fighters, entry 0x5C3E0, per fighter at 0x5B737) set it while they
     draw when their logic counter hasn't moved. But the stage effects
     (falling petals: a spawn every 5th scene frame at 0x1E4950, the counter
     0xE34780 bumped at 0x1E740A) test 0xE0D7B8, which slot 13 copies from
     0x8783C9 at the top of its loop (0x14C636), before that check. Plan:
     set 0x8783C9 for the whole in-between frame (save it in the controller
     cave on the in-between frame, restore it on the next logic frame).
   - HUD timers the flag doesn't cover: 0x8A29A8 (a fade-in, +1 at
     0x6D60E/0x6D616), 0x88371C (blink, read at 0x6D562), 0xEB8640 (a pulse
     into 0x7BE270 at 0x291900). Bump them on logic frames only.
   - Other counters that rise every frame (`debug.hakux.memscan`
     `snap`/`diff`): 0xBACD60/0xBACDD8 (written at 0xFE437), and ones at
     0xF2AD6C, 0xF3E804, 0xF5CDE8, 0x1056C90 that track D3D's frame count.
     Check what reads them before changing them.
2. Blending (shown half a tick late). Findings so far:
   - Every draw is fixed function with skinning off (`debug.hakux.pbdraws`
     `skin 0`). The fighters are rigid parts, each drawn with WORLD = one
     palette matrix: 0x283EA0 loads it into the current world 0xEA6080 and
     0x288AA0 culls and calls `SetTransform(WORLD, 0xEA6080)` (~460 a
     frame).
   - Palette: 0x875C20 + fighter * 0x800 (32 slots of 0x40: rigid matrices
     or zeros). Inverse palette (world to bone, for seams and ribbons):
     0x87AFA0 + fighter * 0x800. `[0x850504]` and `[0x80B7A0]` point at
     the current fighter's. Both are written once per tick by logic task
     slot 9 (entry 0xE68B0), through the matrix store at 0x283EF0
     (`NC_WATCHPA`).
   - Camera: VIEW 0x879C10, PROJECTION 0x87D360 (set by `SetTransform` at
     0x2F090, 0x26C6C0, 0x26C890, 0x27A690, 0x280720; menus use 0x8A0AE0,
     0x8A0A04). Its writer is not found yet: the sparring camera holds
     still. Watch it (`NC_WATCHPA`) while the fighters move.
   - A run of 890 matrices at 0x855C10-0x863A90 (186 code references)
     changes over time; not identified.
   - Plan: on a logic frame, when the scheduler reaches the first slot
     with a DRAWN bit, save the palettes and the camera, then write the
     halfway values between the last tick's and this tick's. Skip a
     matrix whose translation jumps more than ~1 unit (camera cuts, round
     starts). At the next main-loop pass (the controller cave) put the
     saved ones back, so the in-between frame draws the tick exactly.
     The buffers (~17 KB) come from `MmAllocateContiguousMemory` (kernel
     thunk 0x3C155C, ordinal 165) on first use, the pointer kept in the
     header page. About 240 bytes of code space remain there.
   - To look for afterwards: the seams and the hair roots. The CPU builds
     them from the halfway palette on the logic frame, and the in-between
     frame reuses them while drawing the exact palette. If that shows, run
     the seam steps on in-between frames too (they keep no state), but
     not the hair simulation.
3. Run it on the Tab (the user judges the smoothness), then drop
   "(in progress)" from the mod's name and description.

**Tools for this** (`tools/hle/README.md`): `debug.hakux.memscan`
(`snap`/`diff` for counters, `mats`/`mats1[:va]` for matrix runs and
physical addresses), `NC_WATCHPA=pa:len:delay` (who stores there; the clock
is checked every 4096 rearms, so use a delay of 30 s or less and read
the log ~40 s later), `HAKUX_HLE_PROBE` with `debug.hakux.probe <new>` to
re-arm, `debug.hakux.vpeek va:256` (at most 256 words), `tools/hle/re/`
`cgraph.py` (call graph and trees) and `callargs.py` (a function's call
sites with their pushed arguments).
