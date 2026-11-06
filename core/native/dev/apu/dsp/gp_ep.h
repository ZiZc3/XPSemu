/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef HW_XBOX_MCPX_APU_GP_EP_H
#define HW_XBOX_MCPX_APU_GP_EP_H

#include "native/dev/apu/apu_base.h"
#include "native/dev/apu/apu_regs.h"

#include "dsp.h"
#include "dsp_dma.h"
#include "dsp_cpu.h"
#include "dsp_state.h"

typedef struct MCPXAPUState MCPXAPUState;

typedef struct MCPXAPUGPState {
    bool realtime;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUGPState;

typedef struct MCPXAPUEPState {
    bool realtime;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUEPState;

/* the GP's and EP's MMIO windows (0x30000 and 0x50000 into the APU's,
 * 0x10000 bytes each) */
uint64_t mcpx_apu_gp_read(void *opaque, uint32_t addr, unsigned int size);
void mcpx_apu_gp_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size);
uint64_t mcpx_apu_ep_read(void *opaque, uint32_t addr, unsigned int size);
void mcpx_apu_ep_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size);

void mcpx_apu_dsp_init(MCPXAPUState *d);
void mcpx_apu_update_dsp_preference(MCPXAPUState *d);
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);

#endif
