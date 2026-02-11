/*
 * Xbox 360 Xenon machine XGPU helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_XGPU_H
#define HW_PPC_XENON_XGPU_H

#include "hw/ppc/xenon/xenon-internal.h"

void xenon_xgpu_reset(XenonMachineState *xms);
void xenon_xgpu_init_pci_config(XenonMachineState *xms);
uint32_t xenon_xgpu_mmio_read32(XenonMachineState *xms, hwaddr off);
void xenon_xgpu_mmio_write32(XenonMachineState *xms, hwaddr off, uint32_t v);

#endif /* HW_PPC_XENON_XGPU_H */
