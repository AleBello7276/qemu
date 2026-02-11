/*
 * Xbox 360 Xenon machine config (.toml-like) helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/config.h"

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

static void xenon_set_str(char **dst, const char *src)
{
    g_free(*dst);
    *dst = g_strdup(src);
}

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

void xenon_toml_config_clear(XenonTomlConfig *cfg)
{
    g_free(cfg->nand);
    g_free(cfg->fuses);
    g_free(cfg->onebl);
    g_free(cfg->smc_uart);
    memset(cfg, 0, sizeof(*cfg));
}

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
            cfg->trace_boot = b;
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
                   (!g_ascii_strcasecmp(key, "ConsoleRevision") ||
                    !g_ascii_strcasecmp(key, "ConsoleRevison"))) {
            int64_t v;
            if (!xenon_parse_console_revision(val, &v)) {
                error_setg(errp, "invalid ConsoleRevision at line %d", i + 1);
                return false;
            }
            cfg->console_revision = v;
            cfg->have_console_revision = true;
        }
    }

    return true;
}
