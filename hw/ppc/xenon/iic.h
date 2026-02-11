/*
 * Xbox 360 Xenon machine IIC helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_IIC_H
#define HW_PPC_XENON_IIC_H

#include "qemu/osdep.h"
#include "hw/ppc/xenon/xenon-internal.h"

void xenon_iic_reset(XenonMachineState *xms);
uint64_t xenon_iic_read(XenonMachineState *xms, hwaddr off, unsigned size);
void xenon_iic_write(XenonMachineState *xms, hwaddr off, uint64_t data,
                     unsigned size);

#endif /* HW_PPC_XENON_IIC_H */
