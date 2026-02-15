/*
 * QEMU Xbox 360 (Xenon) machine - glitch/bypass compatibility hooks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "target/ppc/spr_common.h"

#define PATCH_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_PATCH, __VA_ARGS__)

/*
 * Convert a firmware code address (CIA/EA-style) to a best-effort physical
 * address during very early boot.
 *
 * Purpose: patching often targets CB/CD code running from the secure SRAM/SROM
 * regions before full MMU state is established, so we need a simple mapping.
 */
static hwaddr xenon_bootstrap_cia_to_pa(uint64_t cia)
{
    if ((cia & 0xFF000000ULL) == 0x02000000ULL) {
        return (hwaddr)((cia & 0x000FFFFFULL) + 0x00010000ULL);
    }
    if ((cia & 0xFF000000ULL) == 0x01000000ULL) {
        return (hwaddr)(cia & 0x00FFFFFFULL);
    }
    return (hwaddr)cia;
}

/*
 * Write a guest instruction word at a given CIA.
 *
 * Purpose: apply RGH-style compatibility patches by either using the CPU debug
 * memory API (preferred, when available) or falling back to a physical write.
 */
static bool xenon_write_guest_insn(XenonMachineState *xms, uint64_t cia,
                                   uint32_t insn)
{
    uint32_t be_insn = cpu_to_be32(insn);
    CPUState *cs = NULL;

    if (xms->boot_cpu) {
        cs = CPU(xms->boot_cpu);
    }
    if (cs && cpu_memory_rw_debug(cs, (vaddr)cia, (uint8_t *)&be_insn,
                                  sizeof(be_insn), 1) == 0) {
        return true;
    }

    address_space_write(&address_space_memory, xenon_bootstrap_cia_to_pa(cia),
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&be_insn,
                        sizeof(be_insn));
    return false;
}

/*
 * Apply CB_A-era RGH2 compatibility patches.
 *
 * Purpose: mirror xenon-emu's bring-up patches that force SHA verification to
 * succeed for common glitch flows, to make modded images usable during bring-up.
 */
static void xenon_apply_rgh2_patches(XenonMachineState *xms)
{
    /*
     * Mirror xenon-emu's interpreter bring-up patches used to bypass
     * CB_A SHA-verify glitch points for common RGH2 flows.
     */
    static const struct {
        uint64_t cia;
        uint32_t insn;
        const char *name;
    } patches[] = {
        { 0x0200C7F0ULL, 0x38600001U, "li r3,1" }, /* force SHA-verify pass branch */
        { 0x0200C870ULL, 0x38A00000U, "li r5,0" }, /* GPR5 = 0 */
    };

    for (size_t i = 0; i < ARRAY_SIZE(patches); i++) {
        bool wrote_via_ea = xenon_write_guest_insn(xms, patches[i].cia,
                                                   patches[i].insn);
        hwaddr pa = xenon_bootstrap_cia_to_pa(patches[i].cia);

        if (xms->trace_boot) {
            PATCH_INFO("rgh2 patch %s at CIA=0x%08" PRIx64
                       " PA=0x%08" PRIx64 " via=%s",
                       patches[i].name, patches[i].cia, (uint64_t)pa,
                       wrote_via_ea ? "ea" : "pa");
        }
    }
    xms->rgh2_patches_applied = true;
    PATCH_INFO("applied CB_A RGH2 compatibility patches");
}

/*
 * Apply CD-stage RGH1 compatibility patches.
 *
 * Purpose: mirror xenon-emu's documented CD patch points that bypass SHA
 * verification without requiring accurate cryptographic state yet.
 */
static void xenon_apply_cd_rgh1_patches(XenonMachineState *xms)
{
    /*
     * Mirror xenon-emu's documented CD-stage RGH1 bypass points.
     * These force SHA-verify success without mutating guest stack buffers.
     */
    static const struct {
        uint64_t cia;
        uint32_t insn;
        const char *name;
    } patches[] = {
        { 0x03004994ULL, 0x38A00000U, "li r5,0" },
        { 0x03004BF0ULL, 0x38600001U, "li r3,1" },
        { 0x04004994ULL, 0x38A00000U, "li r5,0 (0x04 alias)" },
        { 0x04004BF0ULL, 0x38600001U, "li r3,1 (0x04 alias)" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(patches); i++) {
        bool wrote_via_ea = xenon_write_guest_insn(xms, patches[i].cia,
                                                   patches[i].insn);
        hwaddr pa = xenon_bootstrap_cia_to_pa(patches[i].cia);

        if (xms->trace_boot) {
            PATCH_INFO("cd-rgh1 patch %s at CIA=0x%08" PRIx64
                       " PA=0x%08" PRIx64 " via=%s",
                       patches[i].name, patches[i].cia, (uint64_t)pa,
                       wrote_via_ea ? "ea" : "pa");
        }
    }
    xms->cd_rgh1_patches_applied = true;
    PATCH_INFO("applied CD RGH1 compatibility patches");
}

/*
 * Hook invoked when the guest writes a POST code (0x61010).
 *
 * Purpose: drive optional, explicit bring-up compatibility behaviors (RGH2 and
 * CD SHA bypass) keyed off the same POST milestones the real boot flow uses.
 */
void xenon_patches_on_post_write(XenonMachineState *xms, uint8_t post,
                                 uint64_t post_raw, CPUPPCState *env)
{
    if (xms->rgh2_patches && !xms->rgh2_patches_applied &&
        post == 0xD1) {
        xenon_apply_rgh2_patches(xms);
    }
    if (xms->cd_sha_bypass && !xms->cd_rgh1_patches_applied &&
        post == 0x40) {
        xenon_apply_cd_rgh1_patches(xms);
    }
    if (xms->cd_sha_bypass && post == 0x49 && env) {
        uint8_t exp[20] = { 0 };
        uint64_t got_ea = env->gpr[1] + 0x90ULL;
        uint64_t exp_ea = env->gpr[25] + 0x24cULL;
        CPUState *cs = env_cpu(env);
        int exp_rc = -1;
        int got_rc = -1;

        if (cs) {
            exp_rc = cpu_memory_rw_debug(cs, (vaddr)exp_ea, exp, sizeof(exp), 0);
            if (exp_rc == 0) {
                got_rc = cpu_memory_rw_debug(cs, (vaddr)got_ea, exp, sizeof(exp), 1);
            }
        }
        env->gpr[3] = 1;
        env->gpr[5] = 0;
        if (xms->trace_boot) {
            PATCH_INFO("cd-sha-bypass post=0x49 force r3=1 r5=0"
                       " got_ea=0x%016" PRIx64 " exp_ea=0x%016" PRIx64
                       " rc(got)=%d rc(exp)=%d nip=0x%016" PRIx64
                       " lr=0x%016" PRIx64,
                       got_ea, exp_ea, got_rc, exp_rc, env->nip, env->lr);
        }
    }

    /*
     * Keep existing diagnostics for older flows:
     * if cd-sha-bypass is off and we hit CD SHA panic, the warning remains
     * in the POST decode table, so nothing to do here.
     */
    (void)post_raw;
}
