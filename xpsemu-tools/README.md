# XPSemu Tools

Standalone app-jailbreak daemon for XPSemu (`PPSA97358`). It jailbreaks XPSemu
on request so the emulator gets root + sandbox escape (needed for `/data` and
the JIT), gated by a whitelist.

- Watches `/mnt/sandbox/*` + `/download0/etahen_jailbreak` for XPSemu's
  `{"PID":<pid>}` request (staged `.tmp` + rename, the same etaHEN/OnionHEN
  protocol XPSemu speaks).
- Verifies the PID's real title ID (`sceKernelGetAppInfo`) matches the sandbox
  directory it came from, and that the title is listed in
  `/data/whitelist.txt`.
- Escalates with the ps5-payload-sdk kernel helpers only
  (`kernel_set_ucred_*`, `kernel_set_proc_rootdir/jaildir`,
  `kernel_get_root_vnode`): uid/gid -> 0, rootdir/jaildir -> root vnode,
  authid `0x4801000000000013`, caps `0xff..`, attr `0x80`. Same pattern as
  etaHEN's `Hijacker::jailbreak()` / OnionHEN's app-jailbreak service.
- Multi-firmware: offsets resolve at runtime via the SDK, so every firmware
  the SDK + kstuff covers works (up to 13.60 with a current SDK).
- Quiet: one `XPSemu Tools active` toast at boot, one `XPSemu Started...`
  toast per successful unlock. No debug `Jailbreak!` spam.

## Requirements

- Jailbroken PS5 with `kstuff` active.
- ELF loader (`elfldr` on 9021) / HBL / `autoload.txt`.

## Install

1. Build (on Linux, e.g. WSL2 Ubuntu):
```sh
   export PS5_PAYLOAD_SDK=~/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk
   make
```
   Output: `xpsemu_tools.elf`.
2. Copy `xpsemu_tools.elf` to the PS5 and load it together with `kstuff`
   (e.g. add both paths to `/data/autoload.txt`, kstuff first).
3. Allow XPSemu:
```
   PPSA97358
```
   in `/data/whitelist.txt` (create it if needed; see `whitelist.txt` here).
4. Start XPSemu from the home screen (via ShadowMountPlus as usual).

## Protocol (for reference)

XPSemu (`ui/xemu-os-utils-ps5.c`) writes `{"PID":<pid>}` to
`/download0/etahen_jailbreak.tmp`, chmods 0666, fsyncs, renames to
`/download0/etahen_jailbreak`, then waits up to ~10 s for the file to be
consumed (600 x 16.7 ms polls) plus ~7.5 s grace for creds to settle
(450 polls). This daemon polls every 100 ms and unlinks the file once
handled, which is the consume signal the app waits for.
