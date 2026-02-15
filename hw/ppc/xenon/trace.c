/*
 * QEMU Xbox 360 (Xenon) machine - tracing/logging helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/core/cpu.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "hw/ppc/xenon/exceptions.h"
#include "hw/ppc/xenon/postcodes.h"

#define POST_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_POST, __VA_ARGS__)
#define POST_DEBUG(...) XENON_LOG_DEBUG(xms, XENON_LOG_MODULE_POST, __VA_ARGS__)
#define TRACE_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_TRACE, __VA_ARGS__)
#define TRACE_DEBUG(...) XENON_LOG_DEBUG(xms, XENON_LOG_MODULE_TRACE, __VA_ARGS__)
#define PC_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_PC, __VA_ARGS__)
#define PC_DEBUG(...) XENON_LOG_DEBUG(xms, XENON_LOG_MODULE_PC, __VA_ARGS__)

/*
 * Emit a single normalized POST log entry.
 *
 * Purpose: provide a stable "POST timeline" for bring-up and regression
 * checking, with optional ANSI pretty output and monotonic host timing fields.
 */
static void xenon_post_log(XenonMachineState *xms, uint64_t post_raw,
                           const char *desc, uint64_t ea)
{
    uint64_t now_us = g_get_monotonic_time();
    uint64_t host_us;
    uint64_t dpost_us;

    if (!xms->trace_host_start_us) {
        xms->trace_host_start_us = now_us;
    }
    host_us = now_us - xms->trace_host_start_us;
    dpost_us = xms->trace_last_post_host_us ?
               (now_us - xms->trace_last_post_host_us) : 0;
    xms->trace_last_post_host_us = now_us;

    if (xms->pretty_post && isatty(STDERR_FILENO)) {
        if (desc) {
            fprintf(stderr,
                    "\x1b[32m[xbox360][POST]\x1b[0m code=0x%016" PRIx64
                    " \x1b[32m%s\x1b[0m @EA=0x%016" PRIx64
                    " host_us=%" PRIu64 " dpost_us=%" PRIu64 "\n",
                    post_raw, desc, ea, host_us, dpost_us);
        } else {
            fprintf(stderr,
                    "\x1b[32m[xbox360][POST]\x1b[0m code=0x%016" PRIx64
                    " @EA=0x%016" PRIx64
                    " host_us=%" PRIu64 " dpost_us=%" PRIu64 "\n",
                    post_raw, ea, host_us, dpost_us);
        }
        return;
    }

    if (desc) {
        POST_INFO("POST write code=0x%016" PRIx64
                  " (%s) @EA=0x%016" PRIx64
                  " host_us=%" PRIu64 " dpost_us=%" PRIu64,
                  post_raw, desc, ea, host_us, dpost_us);
    } else {
        POST_INFO("POST write code=0x%016" PRIx64
                  " @EA=0x%016" PRIx64
                  " host_us=%" PRIu64 " dpost_us=%" PRIu64,
                  post_raw, ea, host_us, dpost_us);
    }
}

/*
 * Dump the CB HWINIT bytecode range to disk once, using runtime register values.
 *
 * Purpose: preserve the exact bytecode the guest is about to execute so it can
 * be disassembled/reversed out-of-band and compared across runs.
 */
static void xenon_dump_hwinit_bytecode(XenonMachineState *xms, CPUPPCState *env)
{
    g_autofree char *out_dir = NULL;
    g_autofree char *bin_path = NULL;
    g_autofree char *meta_path = NULL;
    g_autofree uint8_t *buf = NULL;
    g_autoptr(GString) meta = NULL;
    uint64_t start_ea = env->gpr[3];
    uint64_t end_ea = env->gpr[4];
    hwaddr start_pa, end_pa;
    uint64_t size;
    GError *gerr = NULL;

    if (xms->hwinit_bytecode_dumped) {
        return;
    }

    xms->hwinit_bytecode_dumped = true;
    start_pa = xenon_seceng_translate(start_ea);
    end_pa = xenon_seceng_translate(end_ea);
    if (end_pa <= start_pa) {
        warn_report("xbox360: hwinit bytecode range invalid: r3=0x%016" PRIx64
                    " r4=0x%016" PRIx64 " pa_start=0x%08" PRIx64
                    " pa_end=0x%08" PRIx64,
                    start_ea, end_ea, (uint64_t)start_pa, (uint64_t)end_pa);
        return;
    }
    size = end_pa - start_pa;
    if (size > 0x02000000ULL) {
        warn_report("xbox360: hwinit bytecode range too large: 0x%" PRIx64, size);
        return;
    }

    out_dir = g_build_filename(g_get_home_dir(), "qemu360", "hwinitDisassembly", NULL);
    bin_path = g_build_filename(out_dir, "hwinit_bytecode_runtime.bin", NULL);
    meta_path = g_build_filename(out_dir, "hwinit_bytecode_runtime.meta.txt", NULL);
    if (g_mkdir_with_parents(out_dir, 0755) != 0) {
        warn_report("xbox360: failed to create output directory '%s'", out_dir);
        return;
    }

    buf = g_malloc(size);
    address_space_read(&address_space_memory, start_pa, MEMTXATTRS_UNSPECIFIED, buf, size);
    if (!g_file_set_contents(bin_path, (const char *)buf, size, &gerr)) {
        warn_report("xbox360: failed to write '%s': %s",
                    bin_path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        return;
    }

    meta = g_string_new("");
    g_string_append_printf(meta, "r3_start_ea=0x%016" PRIx64 "\n", start_ea);
    g_string_append_printf(meta, "r4_end_ea=0x%016" PRIx64 "\n", end_ea);
    g_string_append_printf(meta, "start_pa=0x%08" PRIx64 "\n", (uint64_t)start_pa);
    g_string_append_printf(meta, "end_pa=0x%08" PRIx64 "\n", (uint64_t)end_pa);
    g_string_append_printf(meta, "size=0x%" PRIx64 " (%" PRIu64 ")\n", size, size);
    g_string_append_printf(meta, "first_word_be=0x%08" PRIx32 "\n",
                           size >= 4 ? ldl_be_p(buf) : 0);
    g_file_set_contents(meta_path, meta->str, -1, NULL);

    TRACE_INFO("dumped HWINIT bytecode r3=0x%016" PRIx64 " r4=0x%016" PRIx64
               " -> %s (size=0x%" PRIx64 ")",
               start_ea, end_ea, bin_path, size);
}

/*
 * Observe a POST value (typically after a write to the POST register) and run
 * trace/bring-up hooks tied to specific milestones.
 *
 * Purpose: central place for POST-driven diagnostics (context dumps, counter
 * resets, alias enabling) without polluting device/MMIO handlers.
 */
void xenon_trace_on_post_observed(XenonMachineState *xms, uint64_t post_raw,
                                  uint8_t post, const char *desc, uint64_t ea,
                                  CPUPPCState *env)
{
    if (xms->trace_boot && post == 0xF2 && env) {
        POST_INFO("F2 context NIP=0x%016" PRIx64
                  " SRR0=0x%016" PRIx64 " LR=0x%016" PRIx64
                  " CTR=0x%016" PRIx64,
                  env->nip, env->spr[SPR_SRR0],
                  env->lr, env->ctr);
    }
    if (!xms->rgh2_patches && post == 0xF2) {
        warn_report("xbox360: CB_A SHA verify failed (POST=0xF2). "
                    "For XeLL/RGH images, enable -M xbox360,rgh2-patches=on.");
    }
    if (xms->trace_boot && post == 0xAF && env) {
        uint32_t e104_be = ldl_be_p(xms->soc_e1_data + 0x00040000);
        uint32_t e104_le = ldl_le_p(xms->soc_e1_data + 0x00040000);
        uint32_t d8000_be = ldl_be_p(xms->pci_cfg_data + 0x00008000);
        uint32_t d8000_le = ldl_le_p(xms->pci_cfg_data + 0x00008000);
        uint32_t d8004_be = ldl_be_p(xms->pci_cfg_data + 0x00008004);
        uint32_t d8004_le = ldl_le_p(xms->pci_cfg_data + 0x00008004);
        uint32_t d8008_be = ldl_be_p(xms->pci_cfg_data + 0x00008008);
        uint32_t d8008_le = ldl_le_p(xms->pci_cfg_data + 0x00008008);
        uint32_t e15e0 = ldl_be_p(xms->nb_mmio_data + 0x000015E0);
        uint32_t e15e8 = ldl_be_p(xms->nb_mmio_data + 0x000015E8);
        uint32_t e15ec = ldl_be_p(xms->nb_mmio_data + 0x000015EC);

        POST_INFO("AF context NIP=0x%016" PRIx64
                  " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                  " r0=0x%016" PRIx64 " r1=0x%016" PRIx64
                  " r2=0x%016" PRIx64 " r3=0x%016" PRIx64
                  " r4=0x%016" PRIx64 " r5=0x%016" PRIx64,
                  env->nip, env->lr, env->ctr,
                  env->gpr[0], env->gpr[1], env->gpr[2],
                  env->gpr[3], env->gpr[4], env->gpr[5]);
        POST_INFO("AF regs e1040000(be)=0x%08" PRIx32
                  " e1040000(le)=0x%08" PRIx32
                  " d0008000(be)=0x%08" PRIx32 " d0008000(le)=0x%08" PRIx32
                  " d0008004(be)=0x%08" PRIx32 " d0008004(le)=0x%08" PRIx32
                  " d0008008(be)=0x%08" PRIx32 " d0008008(le)=0x%08" PRIx32
                  " e40015e0=0x%08" PRIx32
                  " e40015e8=0x%08" PRIx32 " e40015ec=0x%08" PRIx32,
                  e104_be, e104_le,
                  d8000_be, d8000_le,
                  d8004_be, d8004_le,
                  d8008_be, d8008_le,
                  e15e0, e15e8, e15ec);
    }
    if (xms->trace_boot && post == 0xAE && env) {
        POST_INFO("AE context NIP=0x%016" PRIx64
                  " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                  " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                  " HSRR0=0x%016" PRIx64 " HSRR1=0x%016" PRIx64,
                  env->nip, env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                  env->spr[SPR_DAR], env->spr[SPR_DSISR],
                  env->spr[SPR_HSRR0], env->spr[SPR_HSRR1]);
    }
    if (xms->trace_boot && post == 0x84 && env) {
        POST_INFO("84 context NIP=0x%016" PRIx64
                  " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                  " LPCR=0x%016" PRIx64
                  " PPE_TLB_INDEX=0x%016" PRIx64
                  " PPE_TLB_VPN=0x%016" PRIx64
                  " PPE_TLB_RPN=0x%016" PRIx64,
                  env->nip, env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                  env->spr[SPR_LPCR],
                  env->spr[SPR_XENON_PPE_TLB_INDEX],
                  env->spr[SPR_XENON_PPE_TLB_VPN],
                  env->spr[SPR_XENON_PPE_TLB_RPN]);
    }
    if (xms->trace_boot && post == 0x4B && env) {
        POST_INFO("4B context NIP=0x%016" PRIx64
                  " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                  " r1=0x%016" PRIx64 " r2=0x%016" PRIx64
                  " r3=0x%016" PRIx64 " r4=0x%016" PRIx64
                  " r5=0x%016" PRIx64 " r6=0x%016" PRIx64
                  " r7=0x%016" PRIx64 " r8=0x%016" PRIx64,
                  env->nip, env->lr, env->ctr,
                  env->gpr[1], env->gpr[2], env->gpr[3], env->gpr[4],
                  env->gpr[5], env->gpr[6], env->gpr[7], env->gpr[8]);
    }
    if (xms->trace_boot && post == 0x83 && env) {
        uint8_t dar_bytes[16] = { 0 };
        int dar_rc = -1;
        CPUState *cs = env_cpu(env);

        if (cs) {
            dar_rc = cpu_memory_rw_debug(cs, (vaddr)env->spr[SPR_DAR],
                                         dar_bytes, sizeof(dar_bytes), 0);
        }
        POST_INFO("83 context NIP=0x%016" PRIx64
                  " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                  " r1=0x%016" PRIx64 " r2=0x%016" PRIx64
                  " r3=0x%016" PRIx64 " r4=0x%016" PRIx64
                  " r5=0x%016" PRIx64 " r6=0x%016" PRIx64
                  " r7=0x%016" PRIx64 " r8=0x%016" PRIx64
                  " r9=0x%016" PRIx64 " r10=0x%016" PRIx64
                  " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                  " LPCR=0x%016" PRIx64
                  " PPE_TLB_INDEX=0x%016" PRIx64
                  " PPE_TLB_VPN=0x%016" PRIx64
                  " PPE_TLB_RPN=0x%016" PRIx64,
                  env->nip, env->lr, env->ctr,
                  env->gpr[1], env->gpr[2], env->gpr[3], env->gpr[4],
                  env->gpr[5], env->gpr[6], env->gpr[7], env->gpr[8],
                  env->gpr[9], env->gpr[10],
                  env->spr[SPR_DAR], env->spr[SPR_DSISR],
                  env->spr[SPR_LPCR],
                  env->spr[SPR_XENON_PPE_TLB_INDEX],
                  env->spr[SPR_XENON_PPE_TLB_VPN],
                  env->spr[SPR_XENON_PPE_TLB_RPN]);
        POST_INFO("83 DAR bytes rc=%d [%02x %02x %02x %02x %02x %02x %02x %02x"
                  " %02x %02x %02x %02x %02x %02x %02x %02x]",
                  dar_rc,
                  dar_bytes[0], dar_bytes[1], dar_bytes[2], dar_bytes[3],
                  dar_bytes[4], dar_bytes[5], dar_bytes[6], dar_bytes[7],
                  dar_bytes[8], dar_bytes[9], dar_bytes[10], dar_bytes[11],
                  dar_bytes[12], dar_bytes[13], dar_bytes[14], dar_bytes[15]);
    }

    if (!xms->have_last_post_code || xms->last_post_code != post_raw) {
        xenon_post_log(xms, post_raw, desc, ea);
        if (xms->trace_boot && post == 0x2E) {
            xms->nand_trace_reads = 0;
            xms->nand_trace_writes = 0;
            xms->soc_trace_reads = 0;
            xms->soc_trace_writes = 0;
            xms->secotp_trace_reads = 0;
            xms->secotp_trace_writes = 0;
            xms->smc_trace_reads = 0;
            xms->smc_trace_writes = 0;
            xms->hwinit_fetch_logs = 0;
            xms->smc_last_status_valid = false;
            xms->smc_last_uart_status = 0;
            TRACE_INFO("trace counters reset at HWINIT entry");
        }
        if (post == 0x40 && !xms->low_mmio_aliases_enabled) {
            xms->low_mmio_aliases_enabled = true;
            if (xms->trace_boot) {
                uint32_t sfcx_cfg = ldl_le_p(xms->pci_cfg_data + 0x8000);
                uint32_t sfcx_sts = ldl_le_p(xms->pci_cfg_data + 0x8004);
                TRACE_INFO("enabled low MMIO aliases for CD/XeLL stage");
                TRACE_INFO("sfcx-pci seed cfg=0x%08" PRIx32
                           " sts=0x%08" PRIx32,
                           sfcx_cfg, sfcx_sts);
            }
        }
        if (xms->trace_boot && post == 0x40) {
            uint8_t cd_bootblk[0x20] = { 0 };
            bool cd_bootblk_empty = true;
            vaddr cd_ea = 0x0000000000280000ULL;
            hwaddr cd_pa_page = cpu_get_phys_page_debug(CPU(xms->boot_cpu), cd_ea);
            hwaddr cd_pa = 0x0000000000280000ULL;
            bool cd_pa_from_soft_tlb = false;

            if (env && xenon_soft_tlb_translate_ea(env, cd_ea, &cd_pa)) {
                cd_pa_from_soft_tlb = true;
            } else if (cd_pa_page != (hwaddr)-1) {
                uint64_t page_off_mask = (1ULL << TARGET_PAGE_BITS) - 1ULL;

                cd_pa = cd_pa_page + (cd_ea & page_off_mask);
            }

            address_space_read(&address_space_memory, cd_pa,
                               MEMTXATTRS_UNSPECIFIED,
                               cd_bootblk, sizeof(cd_bootblk));
            for (size_t i = 0; i < sizeof(cd_bootblk); i++) {
                if (cd_bootblk[i] != 0) {
                    cd_bootblk_empty = false;
                    break;
                }
            }
            if (cd_bootblk_empty) {
                POST_INFO("CD bootblk remains empty at CD entry "
                          "(EA=0x%016" PRIx64 " -> PA=0x%016" HWADDR_PRIx ")",
                          (uint64_t)cd_ea, cd_pa);
            }
            POST_INFO("CD bootblk EA=0x%016" PRIx64
                      " PA=0x%016" HWADDR_PRIx " src=%s "
                      "%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                      " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                      (uint64_t)cd_ea, cd_pa,
                      cd_pa_from_soft_tlb ? "soft-tlb" :
                      (cd_pa_page != (hwaddr)-1 ? "debug-page" : "fallback"),
                      ldl_be_p(cd_bootblk + 0x00), ldl_be_p(cd_bootblk + 0x04),
                      ldl_be_p(cd_bootblk + 0x08), ldl_be_p(cd_bootblk + 0x0C),
                      ldl_be_p(cd_bootblk + 0x10), ldl_be_p(cd_bootblk + 0x14),
                      ldl_be_p(cd_bootblk + 0x18), ldl_be_p(cd_bootblk + 0x1C));
            xms->xgpu_trace_reads = 0;
            xms->xgpu_trace_writes = 0;
            xms->sfcx_trace_reads = 0;
            xms->sfcx_trace_writes = 0;
            xms->smc_trace_reads = 0;
            xms->smc_trace_writes = 0;
            TRACE_INFO("trace counters reset at CD entry");
        }
        if (post == 0x2E && env) {
            xenon_dump_hwinit_bytecode(xms, env);
        }
        if (xms->trace_boot && env) {
            uint8_t srr0_bytes[16] = { 0 };
            uint8_t real_bytes[16] = { 0 };
            uint64_t srr0 = env->spr[SPR_SRR0];
            uint64_t hrmor = env->spr[SPR_HRMOR];
            uint64_t real_ra =
                ((srr0 & 0x3FFFFF00000ULL) | (hrmor & 0x3FFFFF00000ULL)) |
                (srr0 & 0xFFFFFULL);

            TRACE_INFO("context SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                       " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64,
                       env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                       env->spr[SPR_DAR], env->spr[SPR_DSISR]);
            address_space_read(&address_space_memory, srr0, MEMTXATTRS_UNSPECIFIED,
                               srr0_bytes, sizeof(srr0_bytes));
            TRACE_INFO("bytes@SRR0[0x%016" PRIx64 "]=%02x %02x %02x %02x "
                       "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                       srr0,
                       srr0_bytes[0], srr0_bytes[1], srr0_bytes[2], srr0_bytes[3],
                       srr0_bytes[4], srr0_bytes[5], srr0_bytes[6], srr0_bytes[7],
                       srr0_bytes[8], srr0_bytes[9], srr0_bytes[10], srr0_bytes[11],
                       srr0_bytes[12], srr0_bytes[13], srr0_bytes[14], srr0_bytes[15]);
            address_space_read(&address_space_memory, real_ra, MEMTXATTRS_UNSPECIFIED,
                               real_bytes, sizeof(real_bytes));
            TRACE_INFO("bytes@RealRA[0x%016" PRIx64 "]=%02x %02x %02x %02x "
                       "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                       real_ra,
                       real_bytes[0], real_bytes[1], real_bytes[2], real_bytes[3],
                       real_bytes[4], real_bytes[5], real_bytes[6], real_bytes[7],
                       real_bytes[8], real_bytes[9], real_bytes[10], real_bytes[11],
                       real_bytes[12], real_bytes[13], real_bytes[14], real_bytes[15]);
        }
        xms->last_post_code = post_raw;
        xms->have_last_post_code = true;
    }
}

/*
 * Periodic timer callback used by trace-boot to log PC progress.
 *
 * Purpose: provide low-overhead "is it looping" visibility when the guest
 * stalls, without enabling full instruction tracing.
 */
void xenon_pc_log_tick(void *opaque)
{
    XenonMachineState *xms = opaque;
    uint64_t pc;
    int64_t now_ms;
    uint64_t now_host_us;
    uint64_t host_us;
    uint64_t dhost_us;
    CPUPPCState *env;
    bool rc4_hot_loop;
    bool sha_hot_loop;

    if (!xms->boot_cpu) {
        return;
    }

    env = &xms->boot_cpu->env;
    pc = env->nip;
    for (unsigned i = 0; i < xms->pc_watchpoint_count; i++) {
        XenonPcWatchpoint *watch = &xms->pc_watchpoints[i];
        if (watch->triggered) {
            continue;
        }
        if (watch->ea == pc) {
            watch->triggered = true;
            xenon_log_dump_disasm(xms, env, pc, xms->disasm_count,
                                  watch->label ? watch->label : "watchpoint",
                                  XENON_LOG_LEVEL_DEBUG, XENON_LOG_MODULE_PC);
        }
    }
    rc4_hot_loop = (pc >= 0x0000000004001e5cULL &&
                    pc <= 0x0000000004001e9cULL);
    sha_hot_loop = (pc >= 0x0000000004001270ULL &&
                    pc <= 0x00000000040012e8ULL);
    now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    now_host_us = g_get_monotonic_time();
    if (!xms->trace_host_start_us) {
        xms->trace_host_start_us = now_host_us;
    }
    host_us = now_host_us - xms->trace_host_start_us;
    dhost_us = xms->trace_last_pc_host_us ?
               (now_host_us - xms->trace_last_pc_host_us) : 0;

    if (pc != xms->last_logged_pc) {
        xms->same_pc_log_count = 0;
        if (xms->pc_log_count < 24 ||
            (pc >> 16) != (xms->last_logged_pc >> 16) ||
            (now_ms - xms->last_pc_log_ms) >= 1000) {
            if (rc4_hot_loop || sha_hot_loop) {
                PC_INFO("pc=0x%016" PRIx64
                        " host_us=%" PRIu64
                        " dhost_us=%" PRIu64
                        " vms=%" PRIi64
                        " CTR=0x%016" PRIx64
                        " r3=0x%016" PRIx64
                        " r4=0x%016" PRIx64
                        " r10=0x%016" PRIx64,
                        pc, host_us, dhost_us, now_ms,
                        env->ctr, env->gpr[3],
                        env->gpr[4], env->gpr[10]);
            } else {
                PC_INFO("pc=0x%016" PRIx64
                        " host_us=%" PRIu64
                        " dhost_us=%" PRIu64
                        " vms=%" PRIi64,
                        pc, host_us, dhost_us, now_ms);
            }
            xms->last_pc_log_ms = now_ms;
            xms->trace_last_pc_host_us = now_host_us;
        }
        xms->last_logged_pc = pc;
        xms->pc_log_count++;
    } else {
        xms->same_pc_log_count++;
        if ((xms->pc_repeat_threshold && xms->same_pc_log_count >= xms->pc_repeat_threshold) ||
            (!xms->pc_repeat_threshold && (xms->same_pc_log_count % 16384) == 0)) {
            uint8_t r8_byte = 0;
            int r8_dbg_rc = -1;
            CPUState *cs = env_cpu(env);

            if (cs) {
                r8_dbg_rc = cpu_memory_rw_debug(cs, env->gpr[8], &r8_byte, 1, 0);
            }
            PC_DEBUG("pc repeat pc=0x%016" PRIx64
                     " host_us=%" PRIu64
                     " dhost_us=%" PRIu64
                     " vms=%" PRIi64
                     " count=%u LR=0x%016" PRIx64
                     " CTR=0x%016" PRIx64
                     " CR=0x%08" PRIx32
                     " SRR0=0x%016" PRIx64
                     " SRR1=0x%016" PRIx64
                     " DAR=0x%016" PRIx64
                     " DSISR=0x%016" PRIx64
                     " r3=0x%016" PRIx64
                     " r4=0x%016" PRIx64
                     " r5=0x%016" PRIx64
                     " r8=0x%016" PRIx64
                     " [r8]=0x%02x rc=%d"
                     " r10=0x%016" PRIx64
                     " r29=0x%016" PRIx64
                     " r30=0x%016" PRIx64,
                     pc, host_us, dhost_us, now_ms, xms->same_pc_log_count,
                     env->lr, env->ctr,
                     env->crf[0],
                     env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                     env->spr[SPR_DAR], env->spr[SPR_DSISR],
                     env->gpr[3], env->gpr[4], env->gpr[5],
                     env->gpr[8], r8_byte, r8_dbg_rc,
                     env->gpr[10], env->gpr[29], env->gpr[30]);
            xenon_log_dump_disasm(xms, env, pc, xms->disasm_count,
                                  "stall", XENON_LOG_LEVEL_DEBUG,
                                  XENON_LOG_MODULE_PC);
            xms->trace_last_pc_host_us = now_host_us;
            xms->same_pc_log_count = 0;
        }
    }

    {
        uint64_t vector = 0;
        bool is_alias = false;

        if (xenon_decode_exception_vector_pc(pc, &vector, &is_alias) &&
            pc != xms->last_exception_pc) {
            PC_INFO("exception vector 0x%04" PRIx64
                    " (%s) pc=0x%016" PRIx64 "%s"
                    " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                    " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                    " HSRR0=0x%016" PRIx64 " HSRR1=0x%016" PRIx64
                    " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                    " MSR=0x%016" PRIx64
                    " LPCR=0x%016" PRIx64
                    " RMOR=0x%016" PRIx64
                    " HRMOR=0x%016" PRIx64
                    " XTLBI=0x%016" PRIx64
                    " XTLBV=0x%016" PRIx64
                    " XTLBR=0x%016" PRIx64
                    " pending=0x%08x",
                    vector, xenon_exception_vector_name(vector),
                    pc, is_alias ? " (hrmor-alias)" : "",
                    env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                    env->spr[SPR_DAR], env->spr[SPR_DSISR],
                    env->spr[SPR_HSRR0], env->spr[SPR_HSRR1],
                    env->lr, env->ctr,
                    env->msr, env->spr[SPR_LPCR],
                    env->spr[SPR_RMOR], env->spr[SPR_HRMOR],
                    env->spr[SPR_XENON_PPE_TLB_INDEX],
                    env->spr[SPR_XENON_PPE_TLB_VPN],
                    env->spr[SPR_XENON_PPE_TLB_RPN],
                    env->pending_interrupts);
            xms->last_exception_pc = pc;
        }
    }

    if (pc == 0x0000000004000ae0ULL && !xms->cd_offset_probe_logged) {
        uint64_t ea = env->gpr[29] + 0x0cULL;
        uint8_t vbuf[4] = { 0 };
        uint8_t pbuf[4] = { 0 };
        hwaddr pa_page = cpu_get_phys_page_debug(CPU(xms->boot_cpu), (vaddr)ea);
        hwaddr pa = (hwaddr)-1;
        int dbg_rc = cpu_memory_rw_debug(CPU(xms->boot_cpu), (vaddr)ea, vbuf, sizeof(vbuf), 0);
        bool pa_valid = pa_page != (hwaddr)-1;

        if (pa_valid) {
            uint64_t page_off_mask = (1ULL << TARGET_PAGE_BITS) - 1ULL;

            pa = pa_page + (ea & page_off_mask);
            address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                               pbuf, sizeof(pbuf));
        }

        POST_INFO("CD offset probe EA=0x%016" PRIx64
                  " dbg_rc=%d vread_be=0x%08" PRIx32
                  " pa_page=0x%016" HWADDR_PRIx
                  " pa=0x%016" HWADDR_PRIx
                  " pread_be=0x%08" PRIx32,
                  ea, dbg_rc, ldl_be_p(vbuf),
                  pa_page, pa, ldl_be_p(pbuf));
        xms->cd_offset_probe_logged = true;
    }

    if (pc >= XENON_EXC_ALIAS_BASE &&
        pc < (XENON_EXC_ALIAS_BASE + XENON_EXC_ALIAS_SIZE)) {
        if (pc != xms->last_exc_handler_pc) {
            PC_INFO("exc-handler pc=0x%016" PRIx64
                    " off=0x%04" PRIx64
                    " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                    " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                    " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                    " XTLBV=0x%016" PRIx64 " XTLBR=0x%016" PRIx64
                    " pending=0x%08x",
                    pc, (uint64_t)(pc - XENON_EXC_ALIAS_BASE),
                    env->lr, env->ctr,
                    env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                    env->spr[SPR_DAR], env->spr[SPR_DSISR],
                    env->spr[SPR_XENON_PPE_TLB_VPN],
                    env->spr[SPR_XENON_PPE_TLB_RPN],
                    env->pending_interrupts);
            xms->last_exc_handler_pc = pc;
            xms->exc_handler_same_pc_count = 0;
        } else if (pc == (XENON_EXC_ALIAS_BASE + 0x0e80)) {
            xms->exc_handler_same_pc_count++;
            if ((xms->exc_handler_same_pc_count % 1024) == 0) {
                PC_INFO("exc-handler repeat pc=0x%016" PRIx64
                        " count=%u LR=0x%016" PRIx64 " CTR=0x%016" PRIx64,
                        pc, xms->exc_handler_same_pc_count,
                        env->lr, env->ctr);
            }
        }
        if (pc == (XENON_EXC_ALIAS_BASE + 0x0478) && xms->trace_boot) {
            uint64_t base = env->gpr[2] - 0x7fc0ULL;
            uint8_t raw[6 * 4] = { 0 };
            uint32_t slot[6] = { 0 };
            uint32_t mask = 0;

            address_space_read(&address_space_memory, base, MEMTXATTRS_UNSPECIFIED,
                               raw, sizeof(raw));
            for (int i = 0; i < 6; i++) {
                slot[i] = ldl_be_p(raw + (i * 4));
                if (slot[i] != 0) {
                    mask |= (1u << i);
                }
            }
        POST_INFO("cd-poll @0x478 r10(EA)=0x%016" PRIx64
                  " r3=0x%016" PRIx64 " r9=0x%016" PRIx64
                  " r2=0x%016" PRIx64 " mask=0x%02" PRIx32,
                  env->gpr[10], env->gpr[3], env->gpr[9],
                  env->gpr[2], mask);
        }
    }

    /*
     * HWINIT interpreter fetch loop (see tools/hwinit/hwinit.asm):
     *   0x30039a0: lwz r17,0(r16)
     *   0x30039a4: addi r16,r16,4
     */
    if (pc == 0x30039a0 && xms->hwinit_fetch_logs < 128) {
        uint64_t ip_ea = env->gpr[16];
        hwaddr ip_pa = xenon_seceng_translate(ip_ea);
        uint8_t buf[4] = { 0 };
        uint32_t word;

        address_space_read(&address_space_memory, ip_pa, MEMTXATTRS_UNSPECIFIED,
                           buf, sizeof(buf));
        word = ldl_be_p(buf);
        TRACE_INFO("hwinit-fetch[%u] ip_ea=0x%016" PRIx64
                   " ip_pa=0x%08" PRIx64 " word=0x%08" PRIx32,
                   xms->hwinit_fetch_logs, ip_ea, (uint64_t)ip_pa, word);
        xms->hwinit_fetch_logs++;
    }
    if ((pc == 0x30039bc || pc == 0x3003a78 || pc == 0x3003ad8) &&
        xms->hwinit_fetch_logs < 192) {
        uint32_t insn = (uint32_t)env->gpr[17];
        uint32_t op = insn >> 26;
        uint32_t o1 = (insn >> 21) & 0x1f;
        uint32_t o2 = (insn >> 16) & 0x1f;

        TRACE_INFO("hwinit-decode[%u] insn=0x%08" PRIx32
                   " op=0x%02" PRIx32 " o1=0x%02" PRIx32 " o2=0x%02" PRIx32
                   " ip_ea=0x%016" PRIx64,
                   xms->hwinit_fetch_logs, insn, op, o1, o2, env->gpr[16]);
        xms->hwinit_fetch_logs++;
    }
    if (pc == 0x30036e0 && xms->trace_boot) {
        TRACE_INFO("hwinit-outfailure ip_ea=0x%016" PRIx64
                   " end_ea=0x%016" PRIx64 " insn=0x%08" PRIx64
                   " r5=0x%016" PRIx64 " r6=0x%016" PRIx64
                   " r7=0x%016" PRIx64,
                   env->gpr[16], env->gpr[4], env->gpr[17],
                   env->gpr[5], env->gpr[6], env->gpr[7]);
    }
    if (pc == 0x800000001c000c70 && xms->trace_boot) {
        uint64_t base = env->gpr[2] - 0x7fc0ULL;
        uint8_t raw[6 * 4] = { 0 };
        uint32_t slot[6] = { 0 };

        address_space_read(&address_space_memory, base, MEMTXATTRS_UNSPECIFIED,
                           raw, sizeof(raw));
        for (int i = 0; i < 6; i++) {
            slot[i] = ldl_be_p(raw + (i * 4));
        }
        POST_INFO("cd-thread-check r2=0x%016" PRIx64
                  " base=0x%016" PRIx64
                  " slots=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
                  " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                  " tb=%" PRIu64 " r3=0x%016" PRIx64,
                  env->gpr[2], base,
                  slot[0], slot[1], slot[2], slot[3], slot[4], slot[5],
                  cpu_ppc_load_tbl(env), env->gpr[3]);
    }

    timer_mod(xms->pc_log_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}
