/* XOG: what our PS5 layer adds around hakuX's core. Our own code. */
#ifndef REXEMU_CPU_H
#define REXEMU_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NcCore;

/* cpu_ps5.c */
void rx_cpu_set_image(struct NcCore *core, uint32_t lo, uint32_t hi);
bool rx_mmio_fault(void *ucontext, uint32_t addr);
void rx_kernel_stage(const char *stage);

/* vmhost_ps5.c: the Xbox's addresses are the host's on the PS5 */
#define RX_HOLE_END 0x80120000u /* libc.prx's data ends before it (vmhost_ps5.c) */
void rx_stubs_write(uint8_t *phys);
void rx_stubs_leave(void);
bool rx_hole_on(void);
bool rx_vm_reserve(void);
bool rx_ram_create(uint32_t bytes);
void *rx_ram_view(uint32_t bytes);
bool rx_map(uint32_t va, uint32_t pa, uint32_t len);
bool rx_unmap(uint32_t va, uint32_t len);
int rx_protect(void *addr, size_t len, int prot);

/* main_ps5.c */
void rx_log_raw(const char *text, size_t len);

#endif
