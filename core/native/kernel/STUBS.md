# Kernel exports that don't do the Xbox kernel's work

Some exports are accepted here without doing what the Xbox kernel does. Each
row says what happens instead, and why that is enough or what is missing.

An export this kernel doesn't implement at all stops the guest when called
("kunimpl"). A title's unimplemented imports are named when its XBE loads
(`hakuX-native-imports`), and `tools/native/imports.py` lists them over a
census (`tools/native/census.sh`).

| Export | Here | Why |
|---|---|---|
| `KeStallExecutionProcessor` | returns at once | The emulated devices answer at once; a busy wait would only slow the guest. |
| `HalRegisterShutdownNotification`, `HalIsResetOrShutdownPending` | nothing, 0 | No shutdown in the guest: the app ends the process. |
| `HalWriteSMBusValue` | ignored | The SMBus devices' settings (the SMC's LED and fan, the video encoder) have no effect here. |
| `FscSetCacheSize` | nothing | No file-system cache of its own: the host's page cache serves the disc and the drives. |
| `NtProtectVirtualMemory`, `MmSetAddressProtect` | recorded, not enforced | Guest memory has no per-page protection; `NtQueryVirtualMemory` reports what was set. |
| `MmUnmapIoSpace` | nothing | I/O space is never mapped. |
| `KeBoostPriorityThread`, `KeSetDisableBoostThread` | 0 | The scheduler has no priority boosts. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion` | nothing | APCs are delivered only in alertable waits, never into a region. |
| `KeSaveFloatingPointState`, `KeRestoreFloatingPointState` | success | A thread's FPU state stays as it is across kernel calls. |
| `XcCryptService`, `XcUpdateCrypto` | 0, logged | No replaceable crypto vector. |
| `HalReadWritePCISpace` | reads zeros, writes ignored, logged | The PCI devices are emulated by their registers, not their config space. |
| `IoSynchronousDeviceIoControlRequest` | zeros and success, logged | No device objects behind it; no title in the census sends one at boot. |
| `NtDeviceIoControlFile`, `NtFsControlFile` | known codes answered; any other fails (invalid device request), logged once | The known ones are what titles send: the disk's geometry and partition info, the DVD drive's SCSI pass-through (the Xbox security page), and a format's volume lock, unlock and dismount. |
| `KeReleaseSemaphore` past its limit | not released, logged once | The Xbox kernel raises STATUS_SEMAPHORE_LIMIT_EXCEEDED. |
