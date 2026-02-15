/*
 * Xbox 360 Xenon machine config (.toml-like) helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_CONFIG_H
#define HW_PPC_XENON_CONFIG_H

#include "qemu/osdep.h"
#include "qapi/error.h"
#include <glib.h>

typedef struct XenonLogWatchEntry {
    uint64_t ea;
    char *label;
} XenonLogWatchEntry;

typedef struct XenonTomlConfig {
    char *nand;
    char *fuses;
    char *onebl;
    char *odd_image;
    char *hdd_image;
    char *smc_uart;
    char *log_level;
    char *log_modules;
    int64_t power_on_type;
    bool have_power_on_type;
    int64_t avpack_type;
    bool have_avpack_type;
    bool trace_boot;
    bool have_trace_boot;
    bool pretty_post;
    bool have_pretty_post;
    bool rgh2_patches;
    bool have_rgh2_patches;
    bool cd_sha_bypass;
    bool have_cd_sha_bypass;
    int64_t console_revision;
    bool have_console_revision;
    int64_t stall_threshold;
    bool have_stall_threshold;
    int64_t disasm_length;
    bool have_disasm_length;
    GPtrArray *watch_points;
} XenonTomlConfig;

void xenon_toml_config_clear(XenonTomlConfig *cfg);
bool xenon_toml_config_load(const char *path, XenonTomlConfig *cfg, Error **errp);

#endif
