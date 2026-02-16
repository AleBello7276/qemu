/*
 * Xbox 360 Xenon debug/logging helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include <glib.h>

#include "qemu/error-report.h"
#include "disas/disas.h"
#include "disas/disas-internal.h"
#include "hw/core/cpu.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "hw/ppc/xenon/xenon-internal.h"
#include "system/address-spaces.h"

#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"

#define ANSI_RED     "\033[31m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_GRAY    "\033[90m"

static const char *xenon_log_level_color(XenonLogLevel level)
{
    switch (level) {
    case XENON_LOG_LEVEL_ERROR: return ANSI_RED;
    case XENON_LOG_LEVEL_WARN:  return ANSI_YELLOW;
    case XENON_LOG_LEVEL_INFO: return ANSI_GREEN;
    case XENON_LOG_LEVEL_DEBUG: return ANSI_CYAN;
    case XENON_LOG_LEVEL_TRACE: return ANSI_GRAY;
    default: return ANSI_RESET;
    }
}

static const struct {
    uint32_t mask;
    const char *name;
    const char *color;
} xenon_log_modules[] = {
    { XENON_LOG_MODULE_POST,    "post",    ANSI_BLUE },
    { XENON_LOG_MODULE_PC,      "pc",      ANSI_MAGENTA },
    { XENON_LOG_MODULE_NAND,    "nand",    ANSI_CYAN },
    { XENON_LOG_MODULE_SOC,     "soc",     ANSI_GREEN },
    { XENON_LOG_MODULE_SECENG,  "seceng",  ANSI_YELLOW },
    { XENON_LOG_MODULE_SMC,     "smc",     ANSI_RED },
    { XENON_LOG_MODULE_SATA,    "sata",    ANSI_GRAY },
    { XENON_LOG_MODULE_XGPU,    "xgpu",    ANSI_MAGENTA },
    { XENON_LOG_MODULE_PATCH,   "patch",   ANSI_YELLOW },
    { XENON_LOG_MODULE_BOOT,    "boot",    ANSI_GREEN },
    { XENON_LOG_MODULE_MACHINE, "machine", ANSI_CYAN },
    { XENON_LOG_MODULE_IIC,     "iic",     ANSI_GRAY },
    { XENON_LOG_MODULE_TRACE,   "trace",   ANSI_BLUE },
};


static const char *xenon_log_module_name(uint32_t module)
{
    if (module == XENON_LOG_MODULE_NONE) {
        return "none";
    }
    for (size_t i = 0; i < ARRAY_SIZE(xenon_log_modules); i++) {
        if (module == xenon_log_modules[i].mask) {
            return xenon_log_modules[i].name;
        }
    }
    return "unknown";
}

static const char *xenon_log_module_color(uint32_t module)
{
    if (module == XENON_LOG_MODULE_NONE) {
        return ANSI_RESET;
    }
    for (size_t i = 0; i < ARRAY_SIZE(xenon_log_modules); i++) {
        if (module == xenon_log_modules[i].mask) {
            return xenon_log_modules[i].color;
        }
    }
    return ANSI_RESET;
}

static int xenon_debug_read_memory(bfd_vma memaddr, bfd_byte *myaddr, int length,
                                   struct disassemble_info *info)
{
    CPUDebug *s = container_of(info, CPUDebug, info);
    int rc = cpu_memory_rw_debug(s->cpu, memaddr, myaddr, length, 0);

    if (rc == 0) {
        return 0;
    }

    hwaddr pa = xenon_seceng_translate(memaddr);
    MemTxResult res = address_space_read(&address_space_memory, pa,
                                         MEMTXATTRS_UNSPECIFIED,
                                         myaddr, length);
    return res == MEMTX_OK ? 0 : EIO;
}

char *xenon_log_modules_to_string(uint32_t mask)
{
    if (mask == XENON_LOG_MODULE_NONE) {
        return g_strdup("none");
    }
    if (mask == XENON_LOG_MODULE_ALL) {
        return g_strdup("all");
    }
    GString *out = g_string_new(NULL);
    for (size_t i = 0; i < ARRAY_SIZE(xenon_log_modules); i++) {
        if (mask & xenon_log_modules[i].mask) {
            if (out->len > 0) {
                g_string_append(out, ",");
            }
            g_string_append(out, xenon_log_modules[i].name);
        }
    }
    if (out->len == 0) {
        g_string_append(out, "none");
    }
    return g_string_free(out, false);
}

static bool xenon_log_modules_parse_token(const char *tok, uint32_t *module)
{
    if (!tok || !*tok) {
        return false;
    }
    if (!g_ascii_strcasecmp(tok, "all")) {
        *module = XENON_LOG_MODULE_ALL;
        return true;
    }
    if (!g_ascii_strcasecmp(tok, "none")) {
        *module = XENON_LOG_MODULE_NONE;
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(xenon_log_modules); i++) {
        if (!g_ascii_strcasecmp(tok, xenon_log_modules[i].name)) {
            *module = xenon_log_modules[i].mask;
            return true;
        }
    }
    return false;
}

uint32_t xenon_log_modules_from_string(const char *value, bool *ok)
{
    uint32_t mask = 0;
    if (!value || !*value) {
        mask = XENON_LOG_MODULE_ALL;
        if (ok) {
            *ok = true;
        }
        return mask;
    }

    char **parts = g_strsplit(value, ",", -1);
    bool parsed_any = false;

    for (int i = 0; parts[i] != NULL; i++) {
        char *tok = g_strdup(parts[i]);
        char *trimmed = g_strstrip(tok);
        uint32_t part_mask;
        if (*trimmed) {
            if (!xenon_log_modules_parse_token(trimmed, &part_mask)) {
                g_free(tok);
                g_strfreev(parts);
                if (ok) {
                    *ok = false;
                }
                return 0;
            }
            if (!parsed_any || part_mask == XENON_LOG_MODULE_ALL) {
                mask = part_mask;
            } else {
                mask |= part_mask;
            }
            parsed_any = true;
        }
        g_free(tok);
    }

    g_strfreev(parts);
    if (!parsed_any) {
        mask = XENON_LOG_MODULE_NONE;
    }

    if (ok) {
        *ok = true;
    }
    return mask;
}

bool xenon_log_level_from_string(const char *value, XenonLogLevel *level)
{
    const char *s = value ? value : "";
    if (!g_ascii_strcasecmp(s, "off")) {
        *level = XENON_LOG_LEVEL_OFF;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "error")) {
        *level = XENON_LOG_LEVEL_ERROR;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "warn")) {
        *level = XENON_LOG_LEVEL_WARN;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "info")) {
        *level = XENON_LOG_LEVEL_INFO;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "debug")) {
        *level = XENON_LOG_LEVEL_DEBUG;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "trace")) {
        *level = XENON_LOG_LEVEL_TRACE;
        return true;
    }
    return false;
}

const char *xenon_log_level_name(XenonLogLevel level)
{
    switch (level) {
    case XENON_LOG_LEVEL_OFF:   return "off";
    case XENON_LOG_LEVEL_ERROR: return "error";
    case XENON_LOG_LEVEL_WARN:  return "warn";
    case XENON_LOG_LEVEL_INFO:  return "info";
    case XENON_LOG_LEVEL_DEBUG: return "debug";
    case XENON_LOG_LEVEL_TRACE: return "trace";
    default: return "unknown";
    }
}

void xenon_log_set_level(XenonMachineState *xms, XenonLogLevel level)
{
    if (!xms) {
        return;
    }
    xms->log_level = level;
}

void xenon_log_set_modules(XenonMachineState *xms, uint32_t mask)
{
    if (!xms) {
        return;
    }
    xms->log_module_mask = mask;
}

bool xenon_log_enabled(const XenonMachineState *xms, XenonLogLevel level,
                       uint32_t module)
{
    if (!xms) {
        return false;
    }
    if (level > xms->log_level) {
        return false;
    }
    if (module != XENON_LOG_MODULE_NONE &&
        !(xms->log_module_mask & module)) {
        return false;
    }
    return true;
}

bool xenon_log_enabled_raw(XenonLogLevel level, uint32_t module_mask,
                          uint32_t module)
{
    if (level == XENON_LOG_LEVEL_OFF) {
        return false;
    }
    if (module != XENON_LOG_MODULE_NONE &&
        !(module_mask & module)) {
        return false;
    }
    return true;
}

static void xenon_log_emit(const char *prefix, XenonLogLevel level,
                           const char *message)
{
    if (!message) {
        return;
    }
    switch (level) {
    case XENON_LOG_LEVEL_ERROR:
        error_report("%s %s", prefix, message);
        break;
    case XENON_LOG_LEVEL_WARN:
        warn_report("%s %s", prefix, message);
        break;
    default:
        info_report("%s %s", prefix, message);
        break;
    }
}

void xenon_log(const XenonMachineState *xms, XenonLogLevel level,
               uint32_t module, const char *fmt, ...)
{
    if (!xms || !fmt) {
        return;
    }
    if (!xenon_log_enabled(xms, level, module)) {
        return;
    }

    const char *lvl_color = xenon_log_level_color(level);
    const char *mod_color = xenon_log_module_color(module);
    const char *lvl_name = xenon_log_level_name(level);
    const char *module_name = xenon_log_module_name(module);

    g_autofree char *prefix = NULL;
    char buf[128];
    g_snprintf(buf, sizeof(buf), "%sxbox360%s%s[%s%s%s%s]:%s ",
               lvl_color, ANSI_RESET, mod_color, lvl_name, ANSI_RESET,
               mod_color, module_name, ANSI_RESET);
    prefix = g_strdup(buf);

    g_autofree GString *msg = g_string_new(NULL);
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(msg, fmt, ap);
    va_end(ap);

    xenon_log_emit(prefix, level, msg->str);
}

void xenon_log_dump_disasm(XenonMachineState *xms, CPUPPCState *env,
                           uint64_t start_pc, unsigned count,
                           const char *context, XenonLogLevel level,
                           uint32_t module)
{
    if (!xms || !env || count == 0) {
        return;
    }
    if (!xenon_log_enabled(xms, level, module)) {
        return;
    }

    CPUDebug debug;
    g_autoptr(GString) ds = g_string_new("");
    disas_initialize_debug_target(&debug, env_cpu(env));
    debug.info.fprintf_func = disas_gstring_printf;
    debug.info.stream = (FILE *)ds;
    debug.info.read_memory_func = xenon_debug_read_memory;
    debug.info.buffer_vma = start_pc;
    debug.info.buffer_length = 4;
    debug.info.show_opcodes = true;
    if (!debug.info.print_insn) {
        debug.info.print_insn = print_insn_od_target;
    }

    uint64_t pc = start_pc;
    for (unsigned i = 0; i < count; i++) {
        if (!debug.info.print_insn) {
            break;
        }
        int len = debug.info.print_insn(pc, &debug.info);
        if (len <= 0) {
            break;
        }
        pc += len;
    }

    if (ds->len == 0) {
        return;
    }
    xenon_log(xms, level, module, "%s disasm @ 0x%016" PRIx64 ":\n%s",
              context ? context : "watch", start_pc, ds->str);
}
