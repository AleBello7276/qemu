/*
 * Xbox 360 Xenon machine exception helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/ppc/ppc.h"
#include "hw/ppc/xenon/exceptions.h"

typedef struct XenonVectorMap {
    int excp;
    uint16_t vector;
    const char *name;
} XenonVectorMap;

/*
 * Exception vectors validated against xenon-emu PPU exception handlers.
 * Keep this table machine-local so Xenon does not implicitly depend on
 * whatever defaults a generic CPU model happens to provide.
 */
static const XenonVectorMap xenon_vector_map[] = {
    { POWERPC_EXCP_RESET,    0x0100, "SystemReset" },
    { POWERPC_EXCP_MCHECK,   0x0200, "MachineCheck" },
    { POWERPC_EXCP_DSI,      0x0300, "DataStorage" },
    { POWERPC_EXCP_DSEG,     0x0380, "DataSegment" },
    { POWERPC_EXCP_ISI,      0x0400, "InstructionStorage" },
    { POWERPC_EXCP_ISEG,     0x0480, "InstructionSegment" },
    { POWERPC_EXCP_EXTERNAL, 0x0500, "External" },
    { POWERPC_EXCP_ALIGN,    0x0600, "Alignment" },
    { POWERPC_EXCP_PROGRAM,  0x0700, "Program" },
    { POWERPC_EXCP_FPU,      0x0800, "FPUnavailable" },
    { POWERPC_EXCP_DECR,     0x0900, "Decrementer" },
    { POWERPC_EXCP_HDECR,    0x0980, "HypervisorDecrementer" },
    { POWERPC_EXCP_SYSCALL,  0x0c00, "SystemCall" },
    { POWERPC_EXCP_TRACE,    0x0d00, "Trace" },
    { POWERPC_EXCP_PERFM,    0x0f00, "PerformanceMonitor" },
    { POWERPC_EXCP_VPU,      0x0f20, "VXUnavailable" },
    { POWERPC_EXCP_IABR,     0x1300, "IABR" },
    { POWERPC_EXCP_MAINT,    0x1600, "Maintenance" },
    { POWERPC_EXCP_VPUA,     0x1700, "VPUAssist" },
    { POWERPC_EXCP_THERM,    0x1800, "Thermal" },
};

/*
 * Install Xenon-specific exception vector mapping into a CPU instance.
 *
 * Purpose: align QEMU's exception vectors/IVPR behavior with xenon-emu (and
 * observed Xenon firmware expectations), avoiding reliance on generic PPC
 * defaults that may not match Xenon.
 */
void xenon_install_exception_profile(XenonMachineState *xms, CPUPPCState *env)
{
    uint32_t old_tb_flags = env->tb_env ? env->tb_env->flags : 0;

    for (size_t i = 0; i < ARRAY_SIZE(xenon_vector_map); i++) {
        env->excp_vectors[xenon_vector_map[i].excp] = xenon_vector_map[i].vector;
    }
    /*
     * Xenon firmware/XeLL handlers run from the HRMOR mirror region
     * (0x800000001c000000 + vector).
     */
    env->spr[SPR_PPE42_IVPR] = XENON_EXC_ALIAS_BASE;
    env->hreset_vector = 0x0000000000000100ULL;

    if (env->tb_env) {
        /*
         * xenon-emu models DECR as edge-like pending state. Keep Xenon in
         * triggered mode so delivery clears pending and avoids level retrigger
         * loops after handler return.
         */
        env->tb_env->flags |= PPC_DECR_UNDERFLOW_TRIGGERED;
        env->tb_env->flags &= ~PPC_DECR_UNDERFLOW_LEVEL;
    }

    if (xms && xms->trace_boot) {
        info_report("xbox360: installed xenon exception profile "
                    "(hreset=0x%04x ivpr=0x%016" PRIx64
                    " tb_flags 0x%08x -> 0x%08x)",
                    0x0100, env->spr[SPR_PPE42_IVPR],
                    old_tb_flags, env->tb_env ? env->tb_env->flags : 0);
    }
}

/*
 * Decode whether a program counter value corresponds to a Xenon exception
 * vector entry point (direct vector or HRMOR alias mirror).
 *
 * Purpose: make trace/debug output more readable by attributing PCs to named
 * exception vectors during bring-up.
 */
bool xenon_decode_exception_vector_pc(uint64_t pc, uint64_t *vector, bool *is_alias)
{
    for (size_t i = 0; i < ARRAY_SIZE(xenon_vector_map); i++) {
        if (pc == xenon_vector_map[i].vector) {
            *vector = xenon_vector_map[i].vector;
            *is_alias = false;
            return true;
        }
        if (pc == (XENON_EXC_ALIAS_BASE + xenon_vector_map[i].vector)) {
            *vector = xenon_vector_map[i].vector;
            *is_alias = true;
            return true;
        }
    }
    return false;
}

/*
 * Convert an exception vector number to a human-friendly name.
 *
 * Purpose: stable labeling for logs (especially around ISI/DSI/DSEG/ISEG).
 */
const char *xenon_exception_vector_name(uint64_t vector)
{
    for (size_t i = 0; i < ARRAY_SIZE(xenon_vector_map); i++) {
        if (vector == xenon_vector_map[i].vector) {
            return xenon_vector_map[i].name;
        }
    }
    return "Unknown";
}
