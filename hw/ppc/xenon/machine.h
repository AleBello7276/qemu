/*
 * QEMU Xbox 360 (Xenon) machine type helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_MACHINE_H
#define HW_PPC_XENON_MACHINE_H

#include "hw/ppc/xenon/xenon-internal.h"

#define TYPE_XENON_MACHINE MACHINE_TYPE_NAME("xbox360")
OBJECT_DECLARE_SIMPLE_TYPE(XenonMachineState, XENON_MACHINE)

#endif /* HW_PPC_XENON_MACHINE_H */

