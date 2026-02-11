/*
 * Xbox 360 Xenon machine exception helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_EXCEPTIONS_H
#define HW_PPC_XENON_EXCEPTIONS_H

#include "qemu/osdep.h"
#include "hw/ppc/xenon/xenon-internal.h"

#define XENON_EXC_ALIAS_BASE 0x800000001c000000ULL
#define XENON_EXC_ALIAS_SIZE 0x0000000000002000ULL

void xenon_install_exception_profile(XenonMachineState *xms, CPUPPCState *env);
bool xenon_decode_exception_vector_pc(uint64_t pc, uint64_t *vector, bool *is_alias);
const char *xenon_exception_vector_name(uint64_t vector);

#endif /* HW_PPC_XENON_EXCEPTIONS_H */
