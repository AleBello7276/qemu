/*
 * QEMU Xbox 360 (Xenon) machine - QOM type/props
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "hw/ppc/xenon/debug.h"

/*
 * Convert a user-facing boot mode string to the SMC power-on reason byte.
 *
 * Purpose: back the `-M xbox360,boot-mode=...` property while keeping the
 * internal machine state stored as the raw SMC reason code.
 */
static bool xenon_boot_mode_to_reason(const char *mode, uint8_t *reason)
{
    if (!mode || !*mode) {
        return false;
    }
    if (!g_ascii_strcasecmp(mode, "power") ||
        !g_ascii_strcasecmp(mode, "powerbtn")) {
        *reason = XENON_SMC_PWRBTN;
        return true;
    }
    if (!g_ascii_strcasecmp(mode, "eject")) {
        *reason = XENON_SMC_EJECT;
        return true;
    }
    return false;
}

/*
 * Convert an SMC power-on reason byte to a user-facing boot mode string.
 *
 * Purpose: property getter helper and stable naming for logs/config dumps.
 */
const char *xenon_reason_to_boot_mode(uint8_t reason)
{
    switch (reason) {
    case XENON_SMC_EJECT:
        return "eject";
    case XENON_SMC_PWRBTN:
    default:
        return "power";
    }
}

/*
 * Convert an internal console revision enum to a stable string.
 *
 * Purpose: expose console revision in properties/config and keep naming
 * consistent with xenon-emu/common scene terminology.
 */
const char *xenon_console_revision_name(XenonConsoleRevision rev)
{
    switch (rev) {
    case XENON_CONSOLE_XENON:
        return "xenon";
    case XENON_CONSOLE_ZEPHYR:
        return "zephyr";
    case XENON_CONSOLE_FALCON:
        return "falcon";
    case XENON_CONSOLE_JASPER:
        return "jasper";
    case XENON_CONSOLE_TRINITY:
        return "trinity";
    case XENON_CONSOLE_CORONA:
        return "corona";
    case XENON_CONSOLE_CORONA_4GB:
        return "corona4gb";
    case XENON_CONSOLE_WINCHESTER:
        return "winchester";
    default:
        return "unknown";
    }
}

/*
 * Parse a console revision string (or small integer) into the internal enum.
 *
 * Purpose: allow both human-friendly names and numeric overrides via QOM
 * properties/config.
 */
static bool xenon_console_revision_parse(const char *value, XenonConsoleRevision *rev)
{
    uint64_t v;
    const char *s = value ? value : "";

    if (!g_ascii_strcasecmp(s, "xenon")) {
        *rev = XENON_CONSOLE_XENON;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "zephyr")) {
        *rev = XENON_CONSOLE_ZEPHYR;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "falcon")) {
        *rev = XENON_CONSOLE_FALCON;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "jasper")) {
        *rev = XENON_CONSOLE_JASPER;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "trinity")) {
        *rev = XENON_CONSOLE_TRINITY;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "corona")) {
        *rev = XENON_CONSOLE_CORONA;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "corona4gb") ||
        !g_ascii_strcasecmp(s, "corona_4gb")) {
        *rev = XENON_CONSOLE_CORONA_4GB;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "winchester")) {
        *rev = XENON_CONSOLE_WINCHESTER;
        return true;
    }
    if (qemu_strtou64(s, NULL, 0, &v) == 0 && v <= XENON_CONSOLE_WINCHESTER) {
        *rev = (XenonConsoleRevision)v;
        return true;
    }
    return false;
}

/*
 * QOM property getter for `nand` (path to NAND image).
 *
 * Purpose: allow `-M xbox360,nand=...` and introspection via QMP/HMP.
 */
static char *xenon_machine_get_nand(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->nand_path);
}

/*
 * QOM property setter for `nand`.
 *
 * Purpose: store user override and mark it as explicitly set (so config file
 * defaults do not stomp it).
 */
static void xenon_machine_set_nand(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->nand_path);
    xms->nand_path = g_strdup(value);
    xms->user_set_nand = true;
}

/*
 * QOM property getter for `fuses` (path to fuse dump).
 *
 * Purpose: expose the fuse dump path for introspection/debugging.
 */
static char *xenon_machine_get_fuses(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->fuses_path);
}

/*
 * QOM property setter for `fuses`.
 *
 * Purpose: allow overriding the fuse dump path via `-M xbox360,fuses=...`.
 */
static void xenon_machine_set_fuses(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->fuses_path);
    xms->fuses_path = g_strdup(value);
    xms->user_set_fuses = true;
}

/*
 * QOM property getter for `onebl` (path to 1BL binary).
 *
 * Purpose: expose the 1BL binary path for introspection/debugging.
 */
static char *xenon_machine_get_onebl(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->onebl_path);
}

/*
 * QOM property setter for `onebl`.
 *
 * Purpose: allow overriding the 1BL path via `-M xbox360,onebl=...`.
 */
static void xenon_machine_set_onebl(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->onebl_path);
    xms->onebl_path = g_strdup(value);
    xms->user_set_onebl = true;
}

/*
 * QOM property getter for `config` (path to xenon.toml-like config).
 *
 * Purpose: expose the config path for introspection/debugging.
 */
static char *xenon_machine_get_config(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->config_path);
}

/*
 * QOM property setter for `config`.
 *
 * Purpose: set the config path early; actual loading/merging is done during
 * board init in `machine.c`.
 */
static void xenon_machine_set_config(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->config_path);
    xms->config_path = g_strdup(value);
}

/*
 * QOM property getter for `boot-mode`.
 *
 * Purpose: present the internal SMC reason as a stable string.
 */
static char *xenon_machine_get_boot_mode(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xenon_reason_to_boot_mode(xms->smc_power_on_reason));
}

/*
 * QOM property setter for `boot-mode`.
 *
 * Purpose: map "power"/"eject" to the internal SMC reason byte used by the
 * boot ROM/kernel to choose the boot path.
 */
static void xenon_machine_set_boot_mode(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    uint8_t reason = 0;

    if (!xenon_boot_mode_to_reason(value, &reason)) {
        error_setg(errp, "xbox360: invalid boot-mode '%s' (expected 'power' or 'eject')",
                   value ? value : "");
        return;
    }
    xms->smc_power_on_reason = reason;
    xms->user_set_boot_mode = true;
}

/*
 * QOM property getter for `smc-uart`.
 *
 * Purpose: expose which UART backend (stdio/null/print) the SMC should use.
 */
static char *xenon_machine_get_smc_uart(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->smc_uart);
}

/*
 * QOM property setter for `smc-uart`.
 *
 * Purpose: normalize the user string and store it for SMC init.
 */
static void xenon_machine_set_smc_uart(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_autofree char *lower = g_ascii_strdown(value ? value : "", -1);
    g_free(xms->smc_uart);
    xms->smc_uart = g_strdup(lower);
    xms->user_set_smc_uart = true;
}

/*
 * QOM property getter for `console-revision`.
 *
 * Purpose: expose the active revision selection (affects strap-derived fields).
 */
static char *xenon_machine_get_console_revision(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xenon_console_revision_name(xms->console_revision));
}

/*
 * QOM property setter for `console-revision`.
 *
 * Purpose: configure revision-dependent defaults (eg HANA baseline, SFCX strap
 * fields) before machine init consumes them.
 */
static void xenon_machine_set_console_revision(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    XenonConsoleRevision rev;

    if (!xenon_console_revision_parse(value, &rev)) {
        error_setg(errp,
                   "xbox360: invalid console-revision '%s' "
                   "(expected xenon|zephyr|falcon|jasper|trinity|corona|corona4gb|winchester or 0..7)",
                   value ? value : "");
        return;
    }
    xms->console_revision = rev;
    xms->user_set_console_revision = true;
}

/*
 * QOM property getter for `trace-boot`.
 *
 * Purpose: expose whether verbose Xenon bring-up logging is enabled.
 */
static bool xenon_machine_get_trace_boot(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->trace_boot;
}

/*
 * QOM property setter for `trace-boot`.
 *
 * Purpose: enable verbose Xenon bring-up logging without requiring a config
 * file.
 */
static void xenon_machine_set_trace_boot(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->trace_boot = value;
    xms->user_set_trace_boot = true;
}

/*
 * QOM property getter for `pretty-post`.
 *
 * Purpose: choose between stable "POST write ..." logs and ANSI pretty output.
 */
static bool xenon_machine_get_pretty_post(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->pretty_post;
}

/*
 * QOM property setter for `pretty-post`.
 *
 * Purpose: switch between stable info_report formatting and ANSI pretty output.
 */
static void xenon_machine_set_pretty_post(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->pretty_post = value;
    xms->user_set_pretty_post = true;
}

/*
 * QOM property getter for `rgh2-patches`.
 *
 * Purpose: enable/disable explicit, traceable CB_A patch hooks.
 */
static bool xenon_machine_get_rgh2_patches(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->rgh2_patches;
}

/*
 * QOM property setter for `rgh2-patches`.
 *
 * Purpose: toggle explicit, traceable CB_A patch hooks for modded images.
 */
static void xenon_machine_set_rgh2_patches(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->rgh2_patches = value;
    xms->user_set_rgh2_patches = true;
}

/*
 * QOM property getter for `cd-sha-bypass`.
 *
 * Purpose: expose the current setting of the temporary CD SHA-verify bypass.
 */
static bool xenon_machine_get_cd_sha_bypass(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->cd_sha_bypass;
}

/*
 * QOM property setter for `cd-sha-bypass`.
 *
 * Purpose: toggle the temporary CD SHA-verify bypass hook used during bring-up.
 */
static void xenon_machine_set_cd_sha_bypass(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->cd_sha_bypass = value;
    xms->user_set_cd_sha_bypass = true;
}

static char *xenon_machine_get_log_level(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xenon_log_level_name(xms->log_level));
}

static void xenon_machine_set_log_level(Object *obj, const char *value,
                                        Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    XenonLogLevel level;

    if (!xenon_log_level_from_string(value, &level)) {
        error_setg(errp,
                   "xbox360: invalid log-level '%s' "
                   "(expected off|error|warn|info|debug|trace)",
                   value ? value : "");
        return;
    }
    xenon_log_set_level(xms, level);
    xenon_log_update_pc_timer(xms);
}

static char *xenon_machine_get_log_modules(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xenon_log_modules_to_string(xms->log_module_mask);
}

static void xenon_machine_set_log_modules(Object *obj, const char *value,
                                          Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    bool ok = false;
    uint32_t mask = xenon_log_modules_from_string(value, &ok);

    if (!ok) {
        error_setg(errp,
                   "xbox360: invalid log-modules '%s' (expecting comma-separated "
                   "values from post,pc,nand,soc,seceng,smc,sata,xgpu,patch,boot,"
                   "machine,iic,trace,all,none)",
                   value ? value : "");
        return;
    }
    xenon_log_set_modules(xms, mask);
    xenon_log_update_pc_timer(xms);
}

/*
 * XenonMachineState instance initializer (constructor).
 *
 * Purpose: set property defaults and reset all internal pointers/counters so
 * board init can assume a consistent starting state.
 */
static void xenon_machine_instance_init(Object *obj)
{
    XenonMachineState *xms = XENON_MACHINE(obj);

    xms->nand_path = g_strdup("nand.bin");
    xms->fuses_path = g_strdup("fuses.txt");
    xms->onebl_path = g_strdup("1bl.bin");
    xms->odd_image_path = g_strdup("");
    xms->hdd_image_path = g_strdup("");
    xms->config_path = g_strdup("");
    xms->smc_uart = g_strdup("stdio");
    xms->console_revision = XENON_CONSOLE_JASPER;
    xms->smc_power_on_reason = XENON_SMC_PWRBTN;
    xms->smc_avpack_type = XENON_SMC_AVPACK_DEFAULT;
    xms->trace_boot = false;
    xms->pretty_post = true;
    xms->rgh2_patches = true;
    xms->cd_sha_bypass = false;
    memset(xms->cpus, 0, sizeof(xms->cpus));
    xms->cpu_count = 0;
    xms->cpu_online_mask = 0;
    xms->iic_migr2_flip = false;
    xms->user_set_nand = false;
    xms->user_set_fuses = false;
    xms->user_set_onebl = false;
    xms->user_set_trace_boot = false;
    xms->user_set_pretty_post = false;
    xms->user_set_rgh2_patches = false;
    xms->user_set_cd_sha_bypass = false;
    xms->user_set_boot_mode = false;
    xms->user_set_smc_uart = false;
    xms->user_set_smc_avpack = false;
    xms->user_set_console_revision = false;
    xms->user_set_odd_image = false;
    xms->user_set_hdd_image = false;
    xms->rgh2_patches_applied = false;
    xms->cd_rgh1_patches_applied = false;
    xms->ata = NULL;
    xms->ram_ptr = NULL;
    xms->ram_size = 0;
    for (int i = 0; i < ARRAY_SIZE(xms->seceng_windows); i++) {
        xms->seceng_windows[i].fast_alias_name = NULL;
        xms->seceng_windows[i].fast_alias_enabled = false;
    }
    xms->log_level = XENON_LOG_LEVEL_INFO;
    xms->log_module_mask = XENON_LOG_MODULE_ALL;
    xms->pc_repeat_threshold = 16384;
    xms->disasm_count = 8;
    xms->pc_watchpoint_count = 0;
    for (unsigned i = 0; i < XENON_PC_WATCHPOINT_MAX; i++) {
        xms->pc_watchpoints[i].ea = 0;
        xms->pc_watchpoints[i].label = NULL;
        xms->pc_watchpoints[i].triggered = false;
    }
}

/*
 * XenonMachineState finalizer (destructor).
 *
 * Purpose: free heap-owned fields and any sub-device state allocated during
 * bring-up.
 */
static void xenon_machine_finalize(Object *obj)
{
    XenonMachineState *xms = XENON_MACHINE(obj);

    g_free(xms->nand_path);
    g_free(xms->fuses_path);
    g_free(xms->onebl_path);
    g_free(xms->odd_image_path);
    g_free(xms->hdd_image_path);
    g_free(xms->config_path);
    g_free(xms->smc_uart);
    xenon_ata_state_destroy(xms);
    g_free(xms->srom_data);
    g_free(xms->nand_raw_data);
    g_free(xms->nand_mmio_data);
    g_free(xms->nb_mmio_data);
    g_free(xms->soc_e1_data);
    g_free(xms->iic_mmio_data);
    g_free(xms->pci_cfg_data);
    g_free(xms->xgpu_mmio_data);
    g_free(xms->dbg_fb_shadow);
    if (xms->pc_log_timer) {
        timer_free(xms->pc_log_timer);
    }
    for (int i = 0; i < ARRAY_SIZE(xms->seceng_windows); i++) {
        g_free(xms->seceng_windows[i].name);
        g_free(xms->seceng_windows[i].fast_alias_name);
    }
    for (unsigned i = 0; i < xms->pc_watchpoint_count; i++) {
        g_free(xms->pc_watchpoints[i].label);
    }
}

/*
 * Xenon machine class initialization.
 *
 * Purpose: register the `xbox360` machine type, set MachineClass defaults, and
 * expose Xenon-specific `-M xbox360,...` properties.
 */
static void xenon_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Microsoft Xbox 360 (Xenon) early bring-up machine";
    mc->init = xenon_init;
    mc->max_cpus = XENON_MAX_CPUS;
    mc->default_cpus = XENON_MAX_CPUS;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
    mc->default_ram_id = "xbox360.ram";
    mc->default_ram_size = 512 * MiB;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->default_display = "none";

    object_class_property_add_str(oc, "nand",
                                  xenon_machine_get_nand,
                                  xenon_machine_set_nand);
    object_class_property_set_description(oc, "nand",
                                          "Path to Xbox 360 NAND image");

    object_class_property_add_str(oc, "fuses",
                                  xenon_machine_get_fuses,
                                  xenon_machine_set_fuses);
    object_class_property_set_description(oc, "fuses",
                                          "Path to Xbox 360 fuses text file");

    object_class_property_add_str(oc, "onebl",
                                  xenon_machine_get_onebl,
                                  xenon_machine_set_onebl);
    object_class_property_set_description(oc, "onebl",
                                          "Path to Xbox 360 1BL binary");

    object_class_property_add_str(oc, "config",
                                  xenon_machine_get_config,
                                  xenon_machine_set_config);
    object_class_property_set_description(oc, "config",
                                          "Path to xenon.toml-style config");

    object_class_property_add_str(oc, "boot-mode",
                                  xenon_machine_get_boot_mode,
                                  xenon_machine_set_boot_mode);
    object_class_property_set_description(oc, "boot-mode",
                                          "SMC power reason: power or eject");

    object_class_property_add_str(oc, "smc-uart",
                                  xenon_machine_get_smc_uart,
                                  xenon_machine_set_smc_uart);
    object_class_property_set_description(oc, "smc-uart",
                                          "SMC UART backend: null, stdio, print (socket/vcom currently stubbed)");

    object_class_property_add_str(oc, "console-revision",
                                  xenon_machine_get_console_revision,
                                  xenon_machine_set_console_revision);
    object_class_property_set_description(oc, "console-revision",
                                          "Console motherboard revision: xenon, zephyr, falcon, jasper, trinity, corona, corona4gb, winchester");

    object_class_property_add_bool(oc, "trace-boot",
                                   xenon_machine_get_trace_boot,
                                   xenon_machine_set_trace_boot);
    object_class_property_set_description(oc, "trace-boot",
                                          "Enable verbose Xenon boot tracing");

    object_class_property_add_bool(oc, "pretty-post",
                                   xenon_machine_get_pretty_post,
                                   xenon_machine_set_pretty_post);
    object_class_property_set_description(oc, "pretty-post",
                                          "Pretty-print POST codes (ANSI color on TTY)");

    object_class_property_add_bool(oc, "rgh2-patches",
                                   xenon_machine_get_rgh2_patches,
                                   xenon_machine_set_rgh2_patches);
    object_class_property_set_description(oc, "rgh2-patches",
                                          "Enable CB_A RGH2 compatibility register patches");

    object_class_property_add_str(oc, "log-level",
                                  xenon_machine_get_log_level,
                                  xenon_machine_set_log_level);
    object_class_property_set_description(oc, "log-level",
                                          "Debug log level: off,error,warn,info,debug,trace");

    object_class_property_add_str(oc, "log-modules",
                                  xenon_machine_get_log_modules,
                                  xenon_machine_set_log_modules);
    object_class_property_set_description(oc, "log-modules",
                                          "Comma-separated log modules (post,pc,nand,soc,seceng,smc,sata,xgpu,patch,boot,machine,iic,trace,all,none)");

    object_class_property_add_bool(oc, "cd-sha-bypass",
                                   xenon_machine_get_cd_sha_bypass,
                                   xenon_machine_set_cd_sha_bypass);
    object_class_property_set_description(oc, "cd-sha-bypass",
                                          "Temporary bypass for CD SHA-verify panic (POST 0xB3)");
    object_class_property_add_bool(oc, "rgh1-patches",
                                   xenon_machine_get_cd_sha_bypass,
                                   xenon_machine_set_cd_sha_bypass);
    object_class_property_set_description(oc, "rgh1-patches",
                                          "Alias of cd-sha-bypass for RGH1-style CD SHA-verify bypass");
}

static const TypeInfo xenon_machine_typeinfo = {
    .name          = TYPE_XENON_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(XenonMachineState),
    .instance_init = xenon_machine_instance_init,
    .instance_finalize = xenon_machine_finalize,
    .class_init    = xenon_machine_class_init,
};

/*
 * Register the Xenon machine type with QOM.
 *
 * Purpose: hook into QEMU's type system so `-M xbox360` resolves.
 */
static void xenon_machine_register_types(void)
{
    type_register_static(&xenon_machine_typeinfo);
}

type_init(xenon_machine_register_types)
