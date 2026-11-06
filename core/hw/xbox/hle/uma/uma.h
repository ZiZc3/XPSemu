/*
 * hakuX HLE GPU — guest RAM in GPU-importable memory (G0.3 strategy (a)).
 */
#ifndef HW_XBOX_HLE_UMA_UMA_H
#define HW_XBOX_HLE_UMA_UMA_H

#include <stdbool.h>
#include <stddef.h>

struct AHardwareBuffer;

/* The buffer behind guest RAM (hle_uma_ram_fd), for the Vulkan import. */
struct AHardwareBuffer *hle_uma_ahb(void);
/* The native core: guest RAM as an AHB (the one hle_uma_ahb gives the
 * Vulkan import), as a file descriptor of its memory, which the VM maps at
 * every address it needs instead of a memfd's. -1 if the buffer, its fd or
 * the checks fail (the caller falls back to a memfd: no zero-copy then). A
 * second call with the same size (a relaunch) gives the same buffer,
 * cleared. */
int hle_uma_ram_fd(size_t size);

#endif
