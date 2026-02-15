/*
 * QEMU Xbox 360 (Xenon) machine private interfaces (internal only)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_MACHINE_PRIV_H
#define HW_PPC_XENON_MACHINE_PRIV_H

#include "hw/ppc/xenon/machine.h"
#include "system/memory.h"

/* boot.c */
bool xenon_is_valid_nand_magic(const uint8_t *nand, size_t nand_size);
bool xenon_parse_fuses(const char *path, uint64_t fuse_lines[XENON_FUSE_LINES]);
void xenon_fill_fuses_into_srom(uint8_t *srom,
                                const uint64_t fuse_lines[XENON_FUSE_LINES]);
void xenon_build_logical_nand_view(uint8_t *logical, const uint8_t *raw,
                                   size_t raw_size);
void xenon_init_soc_prv_defaults(XenonMachineState *xms);
void xenon_install_boot_bat(CPUPPCState *env);

/* seceng.c */
hwaddr xenon_seceng_translate(uint64_t in_ea);
bool xenon_seceng_ram_fastpath_ok(const XenonMachineState *xms,
                                  hwaddr pa, unsigned size);
bool xenon_soft_tlb_translate_ea(CPUPPCState *env, uint64_t ea, hwaddr *pa_out);
extern const MemoryRegionOps xenon_seceng_ops;

/* patches.c */
void xenon_patches_on_post_write(XenonMachineState *xms, uint8_t post,
                                 uint64_t post_raw, CPUPPCState *env);

/* trace.c */
void xenon_trace_on_post_observed(XenonMachineState *xms, uint64_t post_raw,
                                  uint8_t post, const char *desc, uint64_t ea,
                                  CPUPPCState *env);
void xenon_pc_log_tick(void *opaque);
void xenon_log_update_pc_timer(XenonMachineState *xms);

/* qom.c (shared helper used for logging + getter) */
const char *xenon_reason_to_boot_mode(uint8_t reason);

/* machine.c */
void xenon_init(MachineState *machine);

#endif /* HW_PPC_XENON_MACHINE_PRIV_H */
