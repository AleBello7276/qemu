/*
 * Xbox 360 Xenon machine XGPU helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_XGPU_H
#define HW_PPC_XENON_XGPU_H

#include "hw/ppc/xenon/xenon-internal.h"

typedef struct XenonXgpuFbInfo {
    uint32_t base;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    bool enabled;
    bool tiled;
} XenonXgpuFbInfo;

void xenon_xgpu_reset(XenonMachineState *xms);
void xenon_xgpu_init_pci_config(XenonMachineState *xms);
uint32_t xenon_xgpu_mmio_read32(XenonMachineState *xms, hwaddr off);
void xenon_xgpu_mmio_write32(XenonMachineState *xms, hwaddr off, uint32_t v);
bool xenon_xgpu_get_fb_info(XenonMachineState *xms, XenonXgpuFbInfo *info);

#endif /* HW_PPC_XENON_XGPU_H */
