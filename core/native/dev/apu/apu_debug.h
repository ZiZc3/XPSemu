/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2020-2021 Matt Borgerson
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

#ifndef MCPX_APU_DEBUG_H
#define MCPX_APU_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_VOICE_WORKERS 16

typedef enum McpxApuDebugMonitorPoint {
    MCPX_APU_DEBUG_MON_AC97,
    MCPX_APU_DEBUG_MON_VP,
    MCPX_APU_DEBUG_MON_GP,
    MCPX_APU_DEBUG_MON_EP,
    MCPX_APU_DEBUG_MON_GP_OR_EP
} McpxApuDebugMonitorPoint;

#endif
