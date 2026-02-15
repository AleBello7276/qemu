/*
 * QEMU Xbox 360 (Xenon) machine - board init
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "hw/core/cpu.h"
#include "hw/ppc/ppc.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/ppc/xenon/config.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/exceptions.h"
#include "hw/ppc/xenon/iic.h"
#include "hw/ppc/xenon/xgpu.h"
#include "target/ppc/spr_common.h"
#include "target/ppc/mmu-hash32.h"
#include "target/ppc/mmu-hash64.h"

#define XENON_SECENG_ALIAS_BASE     0x00100000ULL
#define XENON_SECENG_ALIAS_4G_SIZE  (0x0000000100000000ULL - XENON_SECENG_ALIAS_BASE)
#define XENON_SECENG_ALIAS_1G_SIZE  (0x0000000040000000ULL - XENON_SECENG_ALIAS_BASE)
#define XENON_SECENG_DIRECT_ALIAS   1

/*
 * Load xenon.toml-style config (if provided) and merge it into machine state.
 *
 * Purpose: allow a single config file to set artifact paths and bring-up flags
 * while still letting explicit `-M xbox360,...` properties override those values.
 */
static void xenon_clear_watchpoints(XenonMachineState *xms)
{
    if (!xms) {
        return;
    }
    for (unsigned i = 0; i < xms->pc_watchpoint_count; i++) {
        g_free(xms->pc_watchpoints[i].label);
        xms->pc_watchpoints[i].label = NULL;
        xms->pc_watchpoints[i].triggered = false;
        xms->pc_watchpoints[i].pending_dump = false;
        xms->pc_watchpoints[i].ea = 0;
    }
    xms->pc_watchpoint_count = 0;
}

static void xenon_apply_watchpoints(XenonMachineState *xms,
                                    const GPtrArray *points)
{
    if (!xms) {
        return;
    }
    xenon_clear_watchpoints(xms);
    if (!points) {
        return;
    }
    unsigned inserted = 0;
    for (size_t i = 0; i < points->len && inserted < XENON_PC_WATCHPOINT_MAX; i++) {
        XenonLogWatchEntry *entry = g_ptr_array_index(points, i);
        xms->pc_watchpoints[inserted].ea = entry->ea;
        xms->pc_watchpoints[inserted].label =
            entry->label ? g_strdup(entry->label) : NULL;
        xms->pc_watchpoints[inserted].triggered = false;
        xms->pc_watchpoints[inserted].pending_dump = false;
        inserted++;
    }
    xms->pc_watchpoint_count = inserted;
    if (points->len > XENON_PC_WATCHPOINT_MAX) {
        warn_report("xbox360: ignoring extra WatchPC entries beyond %u",
                    XENON_PC_WATCHPOINT_MAX);
    }
}

void xenon_log_update_pc_timer(XenonMachineState *xms)
{
    if (!xms) {
        return;
    }
    bool need_timer = xms->trace_boot ||
                      xms->pc_watchpoint_count > 0 ||
                      (xms->log_module_mask & XENON_LOG_MODULE_PC);
    if (!need_timer) {
        if (xms->pc_log_timer) {
            timer_del(xms->pc_log_timer);
            timer_free(xms->pc_log_timer);
            xms->pc_log_timer = NULL;
        }
        return;
    }
    if (!xms->pc_log_timer) {
        xms->pc_log_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         xenon_pc_log_tick, xms);
    }
    timer_mod(xms->pc_log_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

static void xenon_apply_config_file(XenonMachineState *xms)
{
    XenonTomlConfig cfg = { 0 };
    g_autoptr(Error) local_err = NULL;

    if (!xms->config_path || !xms->config_path[0]) {
        return;
    }

    if (!xenon_toml_config_load(xms->config_path, &cfg, &local_err)) {
        error_report("xbox360: config load failed: %s",
                     error_get_pretty(local_err));
        exit(EXIT_FAILURE);
    }

    if (cfg.nand && !xms->user_set_nand) {
        g_free(xms->nand_path);
        xms->nand_path = g_strdup(cfg.nand);
    }
    if (cfg.fuses && !xms->user_set_fuses) {
        g_free(xms->fuses_path);
        xms->fuses_path = g_strdup(cfg.fuses);
    }
    if (cfg.onebl && !xms->user_set_onebl) {
        g_free(xms->onebl_path);
        xms->onebl_path = g_strdup(cfg.onebl);
    }
    if (cfg.odd_image && !xms->user_set_odd_image) {
        g_free(xms->odd_image_path);
        xms->odd_image_path = g_strdup(cfg.odd_image);
    }
    if (cfg.hdd_image && !xms->user_set_hdd_image) {
        g_free(xms->hdd_image_path);
        xms->hdd_image_path = g_strdup(cfg.hdd_image);
    }
    if (cfg.smc_uart && !xms->user_set_smc_uart) {
        g_free(xms->smc_uart);
        xms->smc_uart = g_strdup(cfg.smc_uart);
    }
    if (cfg.have_power_on_type && !xms->user_set_boot_mode) {
        if (cfg.power_on_type < 0 || cfg.power_on_type > 0xff) {
            error_report("xbox360: PowerOnType out of range in config: %" PRId64,
                         cfg.power_on_type);
            exit(EXIT_FAILURE);
        }
        xms->smc_power_on_reason = (uint8_t)cfg.power_on_type;
    }
    if (cfg.have_avpack_type && !xms->user_set_smc_avpack) {
        if (cfg.avpack_type < 0 || cfg.avpack_type > 0xff) {
            error_report("xbox360: AvPackType out of range in config: %" PRId64,
                         cfg.avpack_type);
            exit(EXIT_FAILURE);
        }
        xms->smc_avpack_type = (uint8_t)cfg.avpack_type;
    }
    if (cfg.have_trace_boot && !xms->user_set_trace_boot) {
        xms->trace_boot = cfg.trace_boot;
    }
    if (cfg.have_pretty_post && !xms->user_set_pretty_post) {
        xms->pretty_post = cfg.pretty_post;
    }
    if (cfg.have_rgh2_patches && !xms->user_set_rgh2_patches) {
        xms->rgh2_patches = cfg.rgh2_patches;
    }
    if (cfg.have_cd_sha_bypass && !xms->user_set_cd_sha_bypass) {
        xms->cd_sha_bypass = cfg.cd_sha_bypass;
    }
    if (cfg.have_console_revision && !xms->user_set_console_revision) {
        if (cfg.console_revision < XENON_CONSOLE_XENON ||
            cfg.console_revision > XENON_CONSOLE_WINCHESTER) {
            error_report("xbox360: ConsoleRevision out of range in config: %" PRId64,
                         cfg.console_revision);
            exit(EXIT_FAILURE);
        }
        xms->console_revision = (XenonConsoleRevision)cfg.console_revision;
    }

    if (cfg.log_level) {
        XenonLogLevel level = xms->log_level;
        if (!xenon_log_level_from_string(cfg.log_level, &level)) {
            error_report("xbox360: invalid LogLevel in config: %s", cfg.log_level);
            exit(EXIT_FAILURE);
        }
        xms->log_level = level;
    }
    if (cfg.log_modules) {
        bool ok = false;
        uint32_t mask = xenon_log_modules_from_string(cfg.log_modules, &ok);
        if (!ok) {
            error_report("xbox360: invalid LogModules in config: %s", cfg.log_modules);
            exit(EXIT_FAILURE);
        }
        xms->log_module_mask = mask;
    }
    if (cfg.have_stall_threshold && cfg.stall_threshold > 0) {
        xms->pc_repeat_threshold = (unsigned)cfg.stall_threshold;
    }
    if (cfg.have_disasm_length && cfg.disasm_length > 0) {
        xms->disasm_count = (unsigned)cfg.disasm_length;
    }
    if (cfg.watch_points) {
        xenon_apply_watchpoints(xms, cfg.watch_points);
    }
    xenon_log_update_pc_timer(xms);

    xenon_toml_config_clear(&cfg);
}

/*
 * Xenon board init entry point (MachineClass::init).
 *
 * Purpose: allocate/seed all Xenon machine state, install memory map and alias
 * windows (SROM/NAND/SecEng/MMIO), create CPUs, and initialize bring-up devices.
 */
void xenon_init(MachineState *machine)
{
    XenonMachineState *xms = XENON_MACHINE(machine);
    PowerPCCPU *cpu;
    CPUPPCState *env;
    uint64_t fuse_lines[XENON_FUSE_LINES] = { 0 };
    g_autofree uint8_t *nand_file_data = NULL;
    g_autofree uint8_t *onebl_data = NULL;
    gsize nand_size = 0, onebl_size = 0;
    GError *gerr = NULL;

    if (machine->smp.cpus > XENON_MAX_CPUS) {
        error_report("xbox360: supports up to %d CPUs in current scaffold",
                     XENON_MAX_CPUS);
        exit(EXIT_FAILURE);
    }

    xenon_apply_config_file(xms);

    if (!g_file_get_contents(xms->nand_path, (char **)&nand_file_data,
                             &nand_size, &gerr)) {
        error_report("xbox360: failed to read NAND image '%s': %s",
                     xms->nand_path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        exit(EXIT_FAILURE);
    }

    if (nand_size < XENON_NAND_MIN_SIZE || nand_size > XENON_NAND_MAX_SIZE) {
        error_report("xbox360: unexpected NAND size 0x%zx from '%s' "
                     "(expected between 0x%" PRIx64 " and 0x%" PRIx64 ")",
                     nand_size, xms->nand_path,
                     (uint64_t)XENON_NAND_MIN_SIZE, (uint64_t)XENON_NAND_MAX_SIZE);
        exit(EXIT_FAILURE);
    }

    if (!xenon_is_valid_nand_magic(nand_file_data, nand_size)) {
        error_report("xbox360: '%s' does not look like an Xbox 360 NAND "
                     "(expected magic FF4F/0F4F/0F3F)", xms->nand_path);
        exit(EXIT_FAILURE);
    }

    if (!xenon_parse_fuses(xms->fuses_path, fuse_lines)) {
        exit(EXIT_FAILURE);
    }

    if (!g_file_get_contents(xms->onebl_path, (char **)&onebl_data,
                             &onebl_size, &gerr)) {
        error_report("xbox360: failed to read 1BL image '%s': %s",
                     xms->onebl_path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        exit(EXIT_FAILURE);
    }

    if (onebl_size != XENON_1BL_SIZE) {
        error_report("xbox360: 1BL image '%s' size is 0x%zx, expected 0x%" PRIx64,
                     xms->onebl_path, onebl_size, (uint64_t)XENON_1BL_SIZE);
        exit(EXIT_FAILURE);
    }

    xms->srom_data = g_malloc0(XENON_SROM_SIZE);
    xms->nand_raw_data = g_malloc0(nand_size);
    xms->nand_raw_size = nand_size;
    xms->nand_mmio_data = g_malloc0(XENON_NAND_MMIO_SIZE);
    xms->nb_mmio_data = g_malloc0(XENON_NB_MMIO_SIZE);
    xms->soc_e1_data = g_malloc0(XENON_SOC_E1_SIZE);
    xms->iic_mmio_data = g_malloc0(XENON_IIC_MMIO_SIZE);
    xms->pci_cfg_data = g_malloc0(XENON_PCI_CFG_SIZE);
    xms->xgpu_mmio_data = g_malloc0(XENON_XGPU_MMIO_SIZE);

    memcpy(xms->srom_data, onebl_data, XENON_1BL_SIZE);
    xenon_fill_fuses_into_srom(xms->srom_data, fuse_lines);
    memcpy(xms->nand_raw_data, nand_file_data, nand_size);
    xenon_build_logical_nand_view(xms->nand_mmio_data, xms->nand_raw_data, nand_size);

    memory_region_add_subregion(get_system_memory(), XENON_RAM_BASE, machine->ram);
    xms->ram_ptr = memory_region_get_ram_ptr(machine->ram);
    xms->ram_size = memory_region_size(machine->ram);
    memory_region_init_ram_ptr(&xms->srom, OBJECT(machine), "xbox360.srom",
                               XENON_SROM_SIZE, xms->srom_data);
    /* SROM overlays the low RAM window during secure boot. */
    memory_region_add_subregion_overlap(get_system_memory(), XENON_SROM_BASE,
                                        &xms->srom, 10);

    memory_region_init_io(&xms->nand, OBJECT(machine), &xenon_nand_ops, xms,
                          "xbox360.nand", XENON_NAND_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_NAND_BASE, &xms->nand);
    xenon_init_soc_prv_defaults(xms);
    xenon_smc_reset(&xms->smc_state, xms->smc_power_on_reason,
                    xms->smc_avpack_type, xms->console_revision,
                    xms->smc_uart, xms->trace_boot);
    memory_region_init_io(&xms->smc, OBJECT(machine), &xenon_smc_ops, xms,
                          "xbox360.smc", XENON_SMC_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_SMC_BASE, &xms->smc);
    xenon_ata_state_init(xms);
    memory_region_init_io(&xms->sata_mmio, OBJECT(machine), &xenon_sata_ops, xms,
                          "xbox360.sata", XENON_SATA_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_SATA_MMIO_BASE, &xms->sata_mmio);
    memory_region_init_io(&xms->sfcx_mmio, OBJECT(machine), &xenon_sfcx_ops, xms,
                          "xbox360.sfcx", XENON_SFCX_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_SFCX_MMIO_BASE, &xms->sfcx_mmio);
    memory_region_init_io(&xms->nb_mmio, OBJECT(machine), &xenon_nb_mmio_ops, xms,
                          "xbox360.nb-mmio", XENON_NB_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_NB_MMIO_BASE, &xms->nb_mmio);
    memory_region_init_io(&xms->pci_cfg_flat, OBJECT(machine), &xenon_pci_cfg_ops, xms,
                          "xbox360.pci-cfg", XENON_PCI_CFG_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_PCI_CFG_BASE, &xms->pci_cfg_flat);
    memory_region_init_io(&xms->xgpu_bar0, OBJECT(machine), &xenon_xgpu_bar0_ops, xms,
                          "xbox360.xgpu-bar0", XENON_XGPU_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_XGPU_BAR0_BASE, &xms->xgpu_bar0);

    {
        static const hwaddr bases[] = {
            XENON_HASH_BASE_LO,
            XENON_SOC_BASE_LO,
            XENON_ENCR_BASE_LO,
            XENON_HASH_BASE_HI,
            XENON_SOC_BASE_HI,
            XENON_ENCR_BASE_HI,
        };

        for (int i = 0; i < ARRAY_SIZE(bases); i++) {
            XenonSecEngWindow *w = &xms->seceng_windows[i];
            uint64_t region;
            uint64_t alias_size = 0;

            w->base = bases[i];
            w->name = g_strdup_printf("xbox360.seceng[%d]", i);
            w->owner = xms;
            memory_region_init_io(&w->mr, OBJECT(machine), &xenon_seceng_ops, w,
                                  w->name, XENON_SECENG_REGION_SIZE);
            memory_region_add_subregion(get_system_memory(), w->base, &w->mr);

            /*
             * Keep low physical addresses (< 1 MiB) on the seceng callback so
             * POST/PRV/IIC bring-up hooks stay active. Overlay direct aliases
             * for the hot RAM range above that threshold.
             */
            region = (w->base & 0x00000F0000000000ULL) >> 32;
            if (region == 0x200) {
                alias_size = XENON_SECENG_ALIAS_4G_SIZE;
            } else if (region == 0x100 || region == 0x300) {
                alias_size = XENON_SECENG_ALIAS_1G_SIZE;
            }

            if (XENON_SECENG_DIRECT_ALIAS && alias_size != 0) {
                w->fast_alias_name = g_strdup_printf("xbox360.seceng[%d].fast", i);
                memory_region_init_alias(&w->fast_alias, OBJECT(machine),
                                         w->fast_alias_name, get_system_memory(),
                                         XENON_SECENG_ALIAS_BASE,
                                         alias_size);
                memory_region_add_subregion_overlap(&w->mr, XENON_SECENG_ALIAS_BASE,
                                                    &w->fast_alias, 1);
                w->fast_alias_enabled = true;
                if (xms->trace_boot) {
                    info_report("xbox360: seceng[%d] direct-alias enabled "
                                "base=0x%016" PRIx64 " size=0x%016" PRIx64,
                                i, (uint64_t)XENON_SECENG_ALIAS_BASE, alias_size);
                }
            }
        }
    }

    xms->dbg_con = graphic_console_init(NULL, 0, &xenon_dbg_display_ops, xms);
    qemu_console_resize(xms->dbg_con, 640, 360);
    xenon_dbg_update_display(xms);
    memset(xms->cpus, 0, sizeof(xms->cpus));
    xms->boot_cpu = NULL;
    xms->cpu_count = machine->smp.cpus;
    if (xms->cpu_count == 0) {
        xms->cpu_count = 1;
    }

    for (unsigned i = 0; i < xms->cpu_count; i++) {
        cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
        env = &cpu->env;
        xms->cpus[i] = cpu;
        if (i == 0) {
            xms->boot_cpu = cpu;
        }

        /*
         * Xenon boot code uses SPR 0x139 (HRMOR) very early. PPC970 in QEMU
         * normally runs with hypervisor facilities strapped off, so register
         * the minimal Xenon-required SPR surface here on every thread context.
         */
        spr_register(env, SPR_HRMOR, "HRMOR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     XENON_HRMOR_INIT);
        spr_register(env, SPR_RMOR, "RMOR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     0x0000000000000000ULL);
        spr_register(env, SPR_LPIDR, "LPIDR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_lpidr,
                     0x00000000U);
        spr_register(env, SPR_HDEC, "HDEC",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_hdecr, &spr_write_hdecr,
                     0x7FFFFFFFULL);
        spr_register(env, SPR_LPCR, "LPCR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_lpcr,
                     XENON_LPCR_INIT);
        spr_register(env, SPR_XENON_HID6, "HID6",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     XENON_HID6_INIT);
        spr_register(env, SPR_PIR, "PIR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, SPR_NOACCESS,
                     i);
        spr_register(env, SPR_XENON_PPE_TLB_INDEX, "PPE_TLB_INDEX",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     0x00000000);
        spr_register(env, SPR_XENON_PPE_TLB_INDEX_HINT, "PPE_TLB_INDEX_HINT",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     0x00000000);
        spr_register(env, SPR_XENON_PPE_TLB_VPN, "PPE_TLB_VPN",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_xenon_ppe_tlb,
                     0x00000000);
        spr_register(env, SPR_XENON_PPE_TLB_RPN, "PPE_TLB_RPN",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_xenon_ppe_tlb,
                     0x00000000);
        spr_register(env, SPR_TSCR, "TSCR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     0x00000000);
        spr_register(env, SPR_POWER_TTR, "TTR",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
                     0x00000000);

        cpu_ppc_tb_init(env, 50000000);
        cpu_reset(CPU(cpu));
        env->nip = 0x100;
        cpu_ppc_tb_reset(env);
        xenon_install_exception_profile(xms, env);
        /*
         * Match xenon-emu/PPE POR behavior: DEC starts positive and does not
         * present a pending interrupt at reset.
         */
        cpu_ppc_store_decr(env, 0x7FFFFFFF);
        ppc_set_irq(cpu, PPC_INTERRUPT_DECR, 0);
        ppc_set_irq(cpu, PPC_INTERRUPT_HDECR, 0);
        env->spr[SPR_HRMOR] = XENON_HRMOR_INIT;
        env->spr[SPR_RMOR] = 0;
        env->spr[SPR_LPIDR] = 0;
        env->spr[SPR_HDEC] = 0x7FFFFFFF;
        env->spr[SPR_LPCR] = XENON_LPCR_INIT;
        env->spr[SPR_XENON_HID6] = XENON_HID6_INIT;
        env->spr[SPR_PIR] = i;
        env->spr[SPR_XENON_PPE_TLB_INDEX_HINT] = 0;
        env->spr[SPR_TSCR] = 0;
        env->spr[SPR_POWER_TTR] = 0;
        xenon_install_boot_bat(env);

        if (i != 0) {
            CPU(cpu)->halted = true;
        }
    }
    xms->cpu_online_mask = 0x01;
    env = &xms->boot_cpu->env;

    xms->pc_log_timer = NULL;
    xms->last_logged_pc = ~(uint64_t)0;
    xms->pc_log_count = 0;
    xms->same_pc_log_count = 0;
    xms->last_pc_log_ms = 0;
    xms->trace_host_start_us = g_get_monotonic_time();
    xms->trace_last_pc_host_us = 0;
    xms->trace_last_post_host_us = 0;
    xms->cd_offset_probe_logged = false;
    xms->have_last_post_code = false;
    xms->last_post_code = 0;
    xms->last_exception_pc = ~(uint64_t)0;
    xms->last_exc_handler_pc = ~(uint64_t)0;
    xms->exc_handler_same_pc_count = 0;
    xms->nand_trace_reads = 0;
    xms->nand_trace_writes = 0;
    xms->soc_trace_reads = 0;
    xms->soc_trace_writes = 0;
    xms->secotp_trace_reads = 0;
    xms->secotp_trace_writes = 0;
    xms->xgpu_trace_reads = 0;
    xms->xgpu_trace_writes = 0;
    xms->sfcx_trace_reads = 0;
    xms->sfcx_trace_writes = 0;
    xms->smc_trace_reads = 0;
    xms->smc_trace_writes = 0;
    xms->hwinit_fetch_logs = 0;
    xms->hwinit_bytecode_dumped = false;
    xms->nb_training_done = false;
    xms->low_mmio_aliases_enabled = false;
    memset(xms->nb_mmio_data, 0, XENON_NB_MMIO_SIZE);
    memset(xms->soc_e1_data, 0, XENON_SOC_E1_SIZE);
    memset(xms->iic_mmio_data, 0, XENON_IIC_MMIO_SIZE);
    xenon_iic_reset(xms);
    xenon_xgpu_reset(xms);
    stl_le_p(xms->soc_e1_data + 0x00040000, 0x20000000U);
    memset(xms->pci_cfg_data, 0xFF, XENON_PCI_CFG_SIZE);
    /*
     * Minimal SFCX-like defaults (matching xenon-emu bring-up values).
     */
    stl_le_p(xms->pci_cfg_data + 0x8000,
             (xms->console_revision == XENON_CONSOLE_XENON) ?
             0x01198030U : 0x00043000U); /* config */
    stl_le_p(xms->pci_cfg_data + 0x8004, 0x00000600U); /* status */
    stl_le_p(xms->pci_cfg_data + 0x8008, 0x000000FFU); /* command (NO_CMD) */
    stl_le_p(xms->pci_cfg_data + 0x800c, 0x00F70030U); /* address */
    stl_le_p(xms->pci_cfg_data + 0x8014, 0x00000100U); /* logical */
    stl_le_p(xms->pci_cfg_data + 0x8018, 0x00000100U); /* physical */
    xenon_xgpu_init_pci_config(xms);
    xenon_ata_init_pci_config(xms);

    stl_le_p(xms->pci_cfg_data + 0x15000, 0x580D1414U); /* SMC reg0 */
    stl_le_p(xms->pci_cfg_data + 0x15004, 0x02000002U); /* SMC reg1 */
    stl_le_p(xms->pci_cfg_data + 0x15010, 0xEA001000U); /* SMC BAR0 */
    stl_le_p(xms->pci_cfg_data + 0x15034, 0x000000D0U); /* cap ptr */
    stl_le_p(xms->pci_cfg_data + 0x1503c, 0x00000100U); /* SMC dev size hint */
    stl_le_p(xms->pci_cfg_data + 0x15060, 0x0000003FU); /* interrupt pin/line */
    stl_le_p(xms->pci_cfg_data + 0x150b0, 0x0000D00DU); /* subsystem */
    xms->smc_last_status_valid = false;
    xms->smc_last_uart_status = 0;
    xms->rgh2_patches_applied = false;
    xenon_log_update_pc_timer(xms);

    warn_report("xbox360: VMX128 is not implemented yet; boot path currently uses stubs/scaffold");
    info_report("xbox360: loaded nand='%s' (0x%zx), fuses='%s', 1bl='%s'",
                xms->nand_path, nand_size, xms->fuses_path, xms->onebl_path);
    info_report("xbox360: media odd='%s' hdd='%s'",
                xms->odd_image_path && xms->odd_image_path[0] ? xms->odd_image_path : "(none)",
                xms->hdd_image_path && xms->hdd_image_path[0] ? xms->hdd_image_path : "(none)");
    info_report("xbox360: smc boot-mode=%s (0x%02x) avpack=0x%02x uart=%s",
                xenon_reason_to_boot_mode(xms->smc_power_on_reason),
                xms->smc_power_on_reason,
                xms->smc_avpack_type,
                xms->smc_uart ? xms->smc_uart : "null");
    info_report("xbox360: console-revision=%s (%u)",
                xenon_console_revision_name(xms->console_revision),
                (unsigned)xms->console_revision);
    if (xms->trace_boot) {
        info_report("xbox360: NAND logical view mapped (%u-byte pages from %u-byte raw pages)",
                    XENON_NAND_LOGICAL_PAGE, XENON_NAND_RAW_PAGE);
        info_report("xbox360: mapped SROM @ 0x%08" PRIx64 " (1BL+fuses), NAND @ 0x%08" PRIx64,
                    (uint64_t)XENON_SROM_BASE, (uint64_t)XENON_NAND_BASE);
        info_report("xbox360: cpu threads configured=%u online-mask=0x%02x",
                    xms->cpu_count, xms->cpu_online_mask);
        info_report("xbox360: cpu mmu_model=0x%x has_hv_mode=%d nb_BATs=%d",
                    env->mmu_model, env->has_hv_mode ? 1 : 0, env->nb_BATs);
        if (env->nb_BATs > 0) {
            info_report("xbox360: boot BAT0 set IBATU=0x%016" PRIx64 " IBATL=0x%016" PRIx64,
                        (uint64_t)env->IBAT[0][0], (uint64_t)env->IBAT[1][0]);
        }
        info_report("xbox360: seceng windows mapped at 0x%016" PRIx64
                    ", 0x%016" PRIx64 ", 0x%016" PRIx64
                    " and 0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64,
                    (uint64_t)XENON_HASH_BASE_LO, (uint64_t)XENON_SOC_BASE_LO, (uint64_t)XENON_ENCR_BASE_LO,
                    (uint64_t)XENON_HASH_BASE_HI, (uint64_t)XENON_SOC_BASE_HI, (uint64_t)XENON_ENCR_BASE_HI);
        {
            uint8_t probe[8] = { 0 };
            address_space_read(&address_space_memory, 0x100, MEMTXATTRS_UNSPECIFIED, probe, sizeof(probe));
            info_report("xbox360: probe @0x100 addrspace=%02x %02x %02x %02x %02x %02x %02x %02x srom=%02x %02x %02x %02x",
                        probe[0], probe[1], probe[2], probe[3], probe[4], probe[5], probe[6], probe[7],
                        xms->srom_data[0x100], xms->srom_data[0x101], xms->srom_data[0x102], xms->srom_data[0x103]);
        }
    }
}
