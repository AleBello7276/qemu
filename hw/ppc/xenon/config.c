/*
 * Xbox 360 Xenon machine config (.toml-like) helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/config.h"

static void xenon_log_watch_entry_free(gpointer data)
{
    XenonLogWatchEntry *entry = data;
    if (!entry) {
        return;
    }
    g_free(entry->label);
    g_free(entry);
}

/*
 * Trim leading and trailing ASCII whitespace in-place.
 *
 * Purpose: normalize config file tokens (keys/values/sections) without
 * allocating new strings.
 */
static char *xenon_trim(char *s)
{
    while (*s && g_ascii_isspace(*s)) {
        s++;
    }
    if (!*s) {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end > s && g_ascii_isspace(*end)) {
        *end-- = '\0';
    }
    return s;
}

/*
 * Replace a heap-owned string field.
 *
 * Purpose: centralize ownership rules for XenonTomlConfig strings (free old,
 * duplicate new).
 */
static void xenon_set_str(char **dst, const char *src)
{
    g_free(*dst);
    *dst = g_strdup(src);
}

/*
 * Parse a boolean value from common TOML-ish representations.
 *
 * Purpose: accept tolerant inputs ("true/false", "on/off", "1/0") while keeping
 * config parsing simple.
 */
static bool xenon_parse_bool(const char *s, bool *out)
{
    if (!g_ascii_strcasecmp(s, "true") || !strcmp(s, "1") ||
        !g_ascii_strcasecmp(s, "on")) {
        *out = true;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "false") || !strcmp(s, "0") ||
        !g_ascii_strcasecmp(s, "off")) {
        *out = false;
        return true;
    }
    return false;
}

/*
 * Parse PowerOnType (SMC "reason") selection.
 *
 * Purpose: map human-friendly strings ("power", "eject") to the numeric values
 * expected by the Xenon boot ROM / kernel path selection.
 */
static bool xenon_parse_power_on_type(const char *s, int64_t *out)
{
    uint64_t v;

    if (!g_ascii_strcasecmp(s, "power") || !g_ascii_strcasecmp(s, "powerbtn")) {
        *out = 0x11;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "eject")) {
        *out = 0x12;
        return true;
    }
    if (qemu_strtou64(s, NULL, 0, &v) == 0) {
        *out = (int64_t)v;
        return true;
    }
    return false;
}

/*
 * Parse console revision selection.
 *
 * Purpose: accept named revisions (xenon/zephyr/...) as well as numeric values,
 * to drive strap-derived differences (eg SFCX geometry fields).
 */
static bool xenon_parse_console_revision(const char *s, int64_t *out)
{
    uint64_t v;

    if (!g_ascii_strcasecmp(s, "xenon")) {
        *out = 0;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "zephyr")) {
        *out = 1;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "falcon")) {
        *out = 2;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "jasper")) {
        *out = 3;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "trinity")) {
        *out = 4;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "corona")) {
        *out = 5;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "corona4gb") ||
        !g_ascii_strcasecmp(s, "corona_4gb")) {
        *out = 6;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "winchester")) {
        *out = 7;
        return true;
    }
    if (qemu_strtou64(s, NULL, 0, &v) == 0) {
        *out = (int64_t)v;
        return true;
    }
    return false;
}

/*
 * Trim and remove matching single/double quotes around a value.
 *
 * Purpose: allow `Key="value"` / `Key='value'` in the simple config format.
 */
static char *xenon_unquote(char *v)
{
    v = xenon_trim(v);
    size_t n = strlen(v);
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
                   (v[0] == '\'' && v[n - 1] == '\''))) {
        v[n - 1] = '\0';
        return v + 1;
    }
    return v;
}

/*
 * Free heap-owned fields and reset the config struct to defaults.
 *
 * Purpose: allow `xenon_toml_config_load()` to reuse a config object safely.
 */
void xenon_toml_config_clear(XenonTomlConfig *cfg)
{
    g_free(cfg->nand);
    g_free(cfg->fuses);
    g_free(cfg->onebl);
    g_free(cfg->odd_image);
    g_free(cfg->hdd_image);
    g_free(cfg->smc_uart);
    g_free(cfg->log_level);
    g_free(cfg->log_modules);
    if (cfg->watch_points) {
        g_ptr_array_free(cfg->watch_points, TRUE);
        cfg->watch_points = NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
}

/*
 * Load and parse the Xenon machine config file.
 *
 * Purpose: translate the user-provided TOML-ish config into a XenonTomlConfig
 * struct that board init uses to locate artifacts (nand/fuses/1bl/...) and
 * configure bring-up flags (trace-boot/pretty-post/patch toggles).
 */
bool xenon_toml_config_load(const char *path, XenonTomlConfig *cfg, Error **errp)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    enum {
        SEC_NONE,
        SEC_FILEPATHS,
        SEC_SMC,
        SEC_BOOT,
        SEC_LOG,
    } sec = SEC_NONE;

    xenon_toml_config_clear(cfg);

    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        error_setg(errp, "failed to read config '%s': %s",
                   path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        return false;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        char *line = xenon_trim(lines[i]);
        char *eq;
        char *key;
        char *val;

        if (!*line || *line == '#') {
            continue;
        }

        if (*line == '[') {
            size_t n = strlen(line);
            if (n < 3 || line[n - 1] != ']') {
                error_setg(errp, "invalid section header at line %d", i + 1);
                return false;
            }
            line[n - 1] = '\0';
            line++;
            line = xenon_trim(line);
            if (!g_ascii_strcasecmp(line, "filepaths")) {
                sec = SEC_FILEPATHS;
            } else if (!g_ascii_strcasecmp(line, "smc")) {
                sec = SEC_SMC;
            } else if (!g_ascii_strcasecmp(line, "boot")) {
                sec = SEC_BOOT;
            } else if (!g_ascii_strcasecmp(line, "log")) {
                sec = SEC_LOG;
            } else {
                sec = SEC_NONE;
            }
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        key = xenon_trim(line);
        val = xenon_unquote(eq + 1);

        /* strip trailing inline comment */
        for (char *p = val; *p; p++) {
            if (*p == '#') {
                *p = '\0';
                break;
            }
        }
        val = xenon_trim(val);

        if ((sec == SEC_FILEPATHS || sec == SEC_NONE) &&
            !g_ascii_strcasecmp(key, "Nand")) {
            xenon_set_str(&cfg->nand, val);
        } else if ((sec == SEC_FILEPATHS || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "Fuses")) {
            xenon_set_str(&cfg->fuses, val);
        } else if ((sec == SEC_FILEPATHS || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "OneBL") || !g_ascii_strcasecmp(key, "onebl"))) {
            xenon_set_str(&cfg->onebl, val);
        } else if ((sec == SEC_FILEPATHS || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "ODDImage") ||
                    !g_ascii_strcasecmp(key, "OddImage"))) {
            xenon_set_str(&cfg->odd_image, val);
        } else if ((sec == SEC_FILEPATHS || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "HDDImage") ||
                    !g_ascii_strcasecmp(key, "HddImage"))) {
            xenon_set_str(&cfg->hdd_image, val);
        } else if ((sec == SEC_SMC || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "PowerOnType")) {
            int64_t v;
            if (!xenon_parse_power_on_type(val, &v)) {
                error_setg(errp, "invalid PowerOnType at line %d", i + 1);
                return false;
            }
            cfg->power_on_type = v;
            cfg->have_power_on_type = true;
        } else if ((sec == SEC_SMC || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "AvPackType")) {
            uint64_t v;
            if (qemu_strtou64(val, NULL, 0, &v) != 0) {
                error_setg(errp, "invalid AvPackType at line %d", i + 1);
                return false;
            }
            cfg->avpack_type = (int64_t)v;
            cfg->have_avpack_type = true;
        } else if ((sec == SEC_SMC || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "UARTSystem")) {
            for (char *p = val; *p; p++) {
                *p = g_ascii_tolower(*p);
            }
            xenon_set_str(&cfg->smc_uart, val);
        } else if ((sec == SEC_BOOT || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "TraceBoot")) {
            bool b;
            if (!xenon_parse_bool(val, &b)) {
                error_setg(errp, "invalid TraceBoot at line %d", i + 1);
                return false;
            }
            /*
             * TraceBoot is deprecated. Setting TraceBoot=true now sets
             * LogLevel=trace. TraceBoot=false is ignored (log_level stays as-is).
             */
            if (b && !cfg->log_level) {
                cfg->log_level = g_strdup("trace");
            }
            cfg->have_trace_boot = true;
        } else if ((sec == SEC_BOOT || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "PrettyPost")) {
            bool b;
            if (!xenon_parse_bool(val, &b)) {
                error_setg(errp, "invalid PrettyPost at line %d", i + 1);
                return false;
            }
            cfg->pretty_post = b;
            cfg->have_pretty_post = true;
        } else if ((sec == SEC_BOOT || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "Rgh2Patches")) {
            bool b;
            if (!xenon_parse_bool(val, &b)) {
                error_setg(errp, "invalid Rgh2Patches at line %d", i + 1);
                return false;
            }
            cfg->rgh2_patches = b;
            cfg->have_rgh2_patches = true;
        } else if ((sec == SEC_BOOT || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "CdShaBypass") ||
                    !g_ascii_strcasecmp(key, "Rgh1Patches"))) {
            bool b;
            if (!xenon_parse_bool(val, &b)) {
                error_setg(errp, "invalid %s at line %d", key, i + 1);
                return false;
            }
            cfg->cd_sha_bypass = b;
            cfg->have_cd_sha_bypass = true;
        } else if ((sec == SEC_BOOT || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "ConsoleRevision") ||
                    !g_ascii_strcasecmp(key, "ConsoleRevison"))) {
            int64_t v;
            if (!xenon_parse_console_revision(val, &v)) {
                error_setg(errp, "invalid ConsoleRevision at line %d", i + 1);
                return false;
            }
            cfg->console_revision = v;
            cfg->have_console_revision = true;
        } else if ((sec == SEC_BOOT || sec == SEC_LOG || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "LogLevel") ||
                    !g_ascii_strcasecmp(key, "Level"))) {
            xenon_set_str(&cfg->log_level, val);
        } else if ((sec == SEC_BOOT || sec == SEC_LOG || sec == SEC_NONE) &&
                   (!g_ascii_strcasecmp(key, "LogModules") ||
                    !g_ascii_strcasecmp(key, "Modules"))) {
            xenon_set_str(&cfg->log_modules, val);
        } else if ((sec == SEC_BOOT || sec == SEC_LOG || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "StallThreshold")) {
            uint64_t v;
            if (qemu_strtou64(val, NULL, 0, &v) != 0) {
                error_setg(errp, "invalid StallThreshold at line %d", i + 1);
                return false;
            }
            cfg->stall_threshold = (int64_t)v;
            cfg->have_stall_threshold = true;
        } else if ((sec == SEC_BOOT || sec == SEC_LOG || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "DisasmLength")) {
            uint64_t v;
            if (qemu_strtou64(val, NULL, 0, &v) != 0) {
                error_setg(errp, "invalid DisasmLength at line %d", i + 1);
                return false;
            }
            cfg->disasm_length = (int64_t)v;
            cfg->have_disasm_length = true;
        } else if ((sec == SEC_BOOT || sec == SEC_LOG || sec == SEC_NONE) &&
                   !g_ascii_strcasecmp(key, "WatchPC")) {
            if (!cfg->watch_points) {
                cfg->watch_points = g_ptr_array_new_with_free_func(
                    (GDestroyNotify)xenon_log_watch_entry_free);
            }
            char *watch = g_strdup(val);
            char *label = NULL;
            char *colon = strchr(watch, ':');
            if (colon) {
                *colon = '\0';
                char *trimmed = xenon_trim(colon + 1);
                if (*trimmed) {
                    label = g_strdup(trimmed);
                }
            }
            char *addr_str = xenon_trim(watch);
            if (!*addr_str) {
                g_free(watch);
                g_free(label);
                continue;
            }
            uint64_t ea;
            if (qemu_strtou64(addr_str, NULL, 0, &ea) != 0) {
                g_free(watch);
                g_free(label);
                error_setg(errp, "invalid WatchPC at line %d", i + 1);
                return false;
            }
            XenonLogWatchEntry *entry = g_new0(XenonLogWatchEntry, 1);
            entry->ea = ea;
            entry->label = label;
            g_ptr_array_add(cfg->watch_points, entry);
            g_free(watch);
        }
    }

    return true;
}
