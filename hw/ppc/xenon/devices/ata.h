/*
 * Xbox 360 Xenon ATA/ATAPI (ODD/HDD) minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_ATA_H
#define HW_PPC_XENON_ATA_H

#include "qemu/osdep.h"
#include "system/memory.h"

typedef struct XenonMachineState XenonMachineState;
typedef struct XenonAtaState XenonAtaState;

extern const MemoryRegionOps xenon_sata_ops;

void xenon_ata_state_init(XenonMachineState *xms);
void xenon_ata_state_destroy(XenonMachineState *xms);
void xenon_ata_init_pci_config(XenonMachineState *xms);

#endif /* HW_PPC_XENON_ATA_H */
