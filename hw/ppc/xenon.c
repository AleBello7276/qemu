/*
 * QEMU Xbox 360 (Xenon) machine - early bring-up scaffold
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "hw/ppc/ppc.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/ppc/xenon/config.h"
#include "hw/ppc/xenon/xenon-internal.h"
#include "hw/ppc/xenon/exceptions.h"
#include "hw/ppc/xenon/postcodes.h"
#include "hw/ppc/xenon/iic.h"
#include "hw/ppc/xenon/xgpu.h"
#include "target/ppc/spr_common.h"
#include "target/ppc/mmu-hash32.h"
#include "ui/pixel_ops.h"

#define TYPE_XENON_MACHINE MACHINE_TYPE_NAME("xbox360")
OBJECT_DECLARE_SIMPLE_TYPE(XenonMachineState, XENON_MACHINE)

static inline hwaddr xenon_seceng_translate(uint64_t in_ea)
{
    uint64_t region = (in_ea & 0x00000F0000000000ULL) >> 32;
    uint64_t low32 = in_ea & 0xFFFFFFFFULL;

    switch (region) {
    case 0x000:
    case 0x200:
        return low32;
    case 0x100:
    case 0x300:
        return low32 & 0x3FFFFFFFULL;
    default:
        return low32;
    }
}

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

static const char *xenon_reason_to_boot_mode(uint8_t reason)
{
    switch (reason) {
    case XENON_SMC_EJECT:
        return "eject";
    case XENON_SMC_PWRBTN:
    default:
        return "power";
    }
}

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
    if (cfg.have_console_revision && !xms->user_set_console_revision) {
        if (cfg.console_revision < XENON_CONSOLE_XENON ||
            cfg.console_revision > XENON_CONSOLE_WINCHESTER) {
            error_report("xbox360: ConsoleRevision out of range in config: %" PRId64,
                         cfg.console_revision);
            exit(EXIT_FAILURE);
        }
        xms->console_revision = (XenonConsoleRevision)cfg.console_revision;
    }

    xenon_toml_config_clear(&cfg);
}

static void xenon_build_logical_nand_view(uint8_t *logical, const uint8_t *raw,
                                          size_t raw_size)
{
    size_t pages = raw_size / XENON_NAND_RAW_PAGE;
    size_t logical_off = 0;

    memset(logical, 0xFF, XENON_NAND_MMIO_SIZE);
    for (size_t i = 0; i < pages && logical_off < XENON_NAND_MMIO_SIZE; i++) {
        size_t raw_off = i * XENON_NAND_RAW_PAGE;
        size_t copy_sz = XENON_NAND_LOGICAL_PAGE;

        if (raw_off + copy_sz > raw_size) {
            break;
        }
        if (logical_off + copy_sz > XENON_NAND_MMIO_SIZE) {
            copy_sz = XENON_NAND_MMIO_SIZE - logical_off;
        }
        memcpy(logical + logical_off, raw + raw_off, copy_sz);
        logical_off += XENON_NAND_LOGICAL_PAGE;
    }
}

static void xenon_init_soc_prv_defaults(XenonMachineState *xms)
{
    uint64_t por = cpu_to_be64(XENON_PRV_POR_STATUS_INIT);
    uint64_t pmc = cpu_to_be64(XENON_PRV_PMCTRL_INIT);
    uint8_t verify[8] = { 0 };
    uint64_t por_rb;

    address_space_write(&address_space_memory, XENON_PRV_POR_STATUS_ADDR,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&por, sizeof(por));
    address_space_write(&address_space_memory, XENON_PRV_PMCTRL_ADDR,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&pmc, sizeof(pmc));
    address_space_read(&address_space_memory, XENON_PRV_POR_STATUS_ADDR,
                       MEMTXATTRS_UNSPECIFIED, verify, sizeof(verify));
    por_rb = ldq_be_p(verify);

    if (xms->trace_boot) {
        info_report("xbox360: initialized PRV defaults "
                    "POR=0x%016" PRIx64 " PMCTRL=0x%016" PRIx64
                    " readback.POR=0x%016" PRIx64,
                    (uint64_t)XENON_PRV_POR_STATUS_INIT,
                    (uint64_t)XENON_PRV_PMCTRL_INIT,
                    por_rb);
    }
}

static void xenon_post_log(XenonMachineState *xms, uint64_t post_raw,
                           const char *desc, uint64_t ea)
{
    if (xms->pretty_post && isatty(STDERR_FILENO)) {
        if (desc) {
            fprintf(stderr,
                    "\x1b[32m[xbox360][POST]\x1b[0m code=0x%016" PRIx64
                    " \x1b[32m%s\x1b[0m @EA=0x%016" PRIx64 "\n",
                    post_raw, desc, ea);
        } else {
            fprintf(stderr,
                    "\x1b[32m[xbox360][POST]\x1b[0m code=0x%016" PRIx64
                    " @EA=0x%016" PRIx64 "\n",
                    post_raw, ea);
        }
        return;
    }

    if (desc) {
        info_report("xbox360: POST write code=0x%016" PRIx64 " (%s) @EA=0x%016" PRIx64,
                    post_raw, desc, ea);
    } else {
        info_report("xbox360: POST write code=0x%016" PRIx64 " @EA=0x%016" PRIx64,
                    post_raw, ea);
    }
}

static void xenon_dbg_invalidate_display(void *opaque)
{
    XenonMachineState *xms = opaque;

    if (xms->dbg_con) {
        dpy_gfx_update_full(xms->dbg_con);
    }
}

static void xenon_dbg_update_display(void *opaque)
{
    XenonMachineState *xms = opaque;
    DisplaySurface *surface;
    uint8_t *dst;
    int bpp;
    uint32_t c0, c1, c2;
    uint8_t band;

    if (!xms->dbg_con) {
        return;
    }

    surface = qemu_console_surface(xms->dbg_con);
    if (!surface || surface_bits_per_pixel(surface) == 0) {
        return;
    }

    bpp = (surface_bits_per_pixel(surface) + 7) >> 3;
    c0 = rgb_to_pixel32(0x1d, 0x2d, 0x44);
    c1 = rgb_to_pixel32(0x2b, 0x90, 0xd9);
    c2 = rgb_to_pixel32(0x31, 0xc4, 0x8d);
    band = (uint8_t)((xms->last_post_code >> 56) & 0xff);

    for (int y = 0; y < surface_height(surface); y++) {
        dst = surface_data(surface) + y * surface_stride(surface);
        for (int x = 0; x < surface_width(surface); x++) {
            uint32_t color;
            uint8_t t = (uint8_t)((x ^ y) + band);

            if (t < 0x55) {
                color = c0;
            } else if (t < 0xaa) {
                color = c1;
            } else {
                color = c2;
            }
            switch (bpp) {
            case 4:
                ((uint32_t *)dst)[x] = color;
                break;
            case 2:
                ((uint16_t *)dst)[x] = rgb_to_pixel16((color >> 16) & 0xff,
                                                      (color >> 8) & 0xff,
                                                      color & 0xff);
                break;
            case 1:
                dst[x] = rgb_to_pixel8((color >> 16) & 0xff,
                                       (color >> 8) & 0xff,
                                       color & 0xff);
                break;
            default:
                break;
            }
        }
    }

    dpy_gfx_update_full(xms->dbg_con);
}

static const GraphicHwOps xenon_dbg_display_ops = {
    .invalidate = xenon_dbg_invalidate_display,
    .gfx_update = xenon_dbg_update_display,
};

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

    info_report("xbox360: dumped HWINIT bytecode r3=0x%016" PRIx64 " r4=0x%016" PRIx64
                " -> %s (size=0x%" PRIx64 ")",
                start_ea, end_ea, bin_path, size);
}

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
        uint32_t be_insn = cpu_to_be32(patches[i].insn);
        hwaddr pa = xenon_bootstrap_cia_to_pa(patches[i].cia);

        address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&be_insn, sizeof(be_insn));
        if (xms->trace_boot) {
            info_report("xbox360: rgh2 patch %s at CIA=0x%08" PRIx64
                        " PA=0x%08" PRIx64,
                        patches[i].name, patches[i].cia, (uint64_t)pa);
        }
    }
    xms->rgh2_patches_applied = true;
    info_report("xbox360: applied CB_A RGH2 compatibility patches");
}

static uint64_t xenon_seceng_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonSecEngWindow *w = opaque;
    uint64_t ea = w->base + offset;
    hwaddr pa = xenon_seceng_translate(ea);
    uint8_t buf[8] = {0};
    XenonMachineState *xms = w->owner;

    if (pa >= XENON_PCI_CFG_BASE &&
        pa < (XENON_PCI_CFG_BASE + XENON_PCI_CFG_SIZE) &&
        size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_PCI_CFG_BASE;

        if (off + size <= XENON_PCI_CFG_SIZE) {
            memcpy(buf, xms->pci_cfg_data + off, size);
        } else {
            memset(buf, 0xFF, size);
        }
    } else if (pa >= XENON_SOC_E1_BASE &&
               pa < (XENON_SOC_E1_BASE + XENON_SOC_E1_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_SOC_E1_BASE;

        if (off + size <= XENON_SOC_E1_SIZE) {
            memcpy(buf, xms->soc_e1_data + off, size);
        } else {
            memset(buf, 0, size);
        }
    } else if (pa >= XENON_XGPU_MMIO_BASE &&
               pa < (XENON_XGPU_MMIO_BASE + XENON_XGPU_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_XGPU_MMIO_BASE;

        if (off + size <= XENON_XGPU_MMIO_SIZE) {
            if (size == 4 && (off & 3) == 0) {
                stl_be_p(buf, xenon_xgpu_mmio_read32(xms, off));
            } else {
                memcpy(buf, xms->xgpu_mmio_data + off, size);
            }
        } else {
            memset(buf, 0, size);
        }
    } else if (pa >= XENON_IIC_MMIO_BASE &&
               pa < (XENON_IIC_MMIO_BASE + XENON_IIC_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        uint64_t v = xenon_iic_read(xms, pa - XENON_IIC_MMIO_BASE, size);

        switch (size) {
        case 1:
            buf[0] = (uint8_t)v;
            break;
        case 2:
            stw_be_p(buf, (uint16_t)v);
            break;
        case 4:
            stl_be_p(buf, (uint32_t)v);
            break;
        case 8:
            stq_be_p(buf, v);
            break;
        default:
            break;
        }
    } else {
        address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, buf, size);
    }
    if (xms && xms->trace_boot &&
        pa >= XENON_NAND_BASE &&
        pa < (XENON_NAND_BASE + XENON_NAND_MMIO_SIZE) &&
        xms->nand_trace_reads < 32) {
        uint64_t v = 0;

        switch (size) {
        case 1: v = buf[0]; break;
        case 2: v = lduw_be_p(buf); break;
        case 4: v = ldl_be_p(buf); break;
        case 8: v = ldq_be_p(buf); break;
        default: break;
        }
        info_report("xbox360: nand-read EA=0x%016" PRIx64 " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, v);
        xms->nand_trace_reads++;
    }
    if (xms && xms->trace_boot &&
        pa < 0x00100000 &&
        xms->soc_trace_reads < 96 &&
        pa != 0x61010) {
        uint64_t v = 0;

        switch (size) {
        case 1: v = buf[0]; break;
        case 2: v = lduw_be_p(buf); break;
        case 4: v = ldl_be_p(buf); break;
        case 8: v = ldq_be_p(buf); break;
        default: break;
        }
        info_report("xbox360: soc-read  EA=0x%016" PRIx64 " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, v);
        xms->soc_trace_reads++;
    }
    if (xms && xms->trace_boot &&
        ((pa >= 0x00020000 && pa < 0x00024000) ||
         (pa >= 0x00061000 && pa < 0x00061200)) &&
        xms->secotp_trace_reads < 128) {
        uint64_t v = 0;

        switch (size) {
        case 1: v = buf[0]; break;
        case 2: v = lduw_be_p(buf); break;
        case 4: v = ldl_be_p(buf); break;
        case 8: v = ldq_be_p(buf); break;
        default: break;
        }
        info_report("xbox360: secotp/prv-read EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64 " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, v);
        xms->secotp_trace_reads++;
    }
    if (xms && xms->trace_boot &&
        pa >= XENON_XGPU_MMIO_BASE &&
        pa < (XENON_XGPU_MMIO_BASE + XENON_XGPU_MMIO_SIZE) &&
        xms->xgpu_trace_reads < 64) {
        uint64_t v = 0;

        switch (size) {
        case 1: v = buf[0]; break;
        case 2: v = lduw_be_p(buf); break;
        case 4: v = ldl_be_p(buf); break;
        case 8: v = ldq_be_p(buf); break;
        default: break;
        }
        info_report("xbox360: xgpu-mmio-read EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64 " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, v);
        xms->xgpu_trace_reads++;
    }
    switch (size) {
    case 1:
        return buf[0];
    case 2:
        return lduw_be_p(buf);
    case 4:
        return ldl_be_p(buf);
    case 8:
        return ldq_be_p(buf);
    default:
        return 0;
    }
}

static void xenon_seceng_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonSecEngWindow *w = opaque;
    uint64_t ea = w->base + offset;
    hwaddr pa = xenon_seceng_translate(ea);
    uint8_t buf[8];
    XenonMachineState *xms = w->owner;

    switch (size) {
    case 1:
        buf[0] = data;
        break;
    case 2:
        stw_be_p(buf, data);
        break;
    case 4:
        stl_be_p(buf, data);
        break;
    case 8:
        stq_be_p(buf, data);
        break;
    default:
        return;
    }

    if (pa >= XENON_PCI_CFG_BASE &&
               pa < (XENON_PCI_CFG_BASE + XENON_PCI_CFG_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_PCI_CFG_BASE;

        if (off + size <= XENON_PCI_CFG_SIZE) {
            memcpy(xms->pci_cfg_data + off, buf, size);
            if (off == 0x8004 && size == 2) {
                /*
                 * SFCX status write semantics: write then readback returns
                 * WP/BY pins asserted.
                 */
                stl_le_p(xms->pci_cfg_data + 0x8004, 0x00000600U);
            }
        }
    } else if (pa >= XENON_SOC_E1_BASE &&
               pa < (XENON_SOC_E1_BASE + XENON_SOC_E1_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_SOC_E1_BASE;

        if (off + size <= XENON_SOC_E1_SIZE) {
            memcpy(xms->soc_e1_data + off, buf, size);
        }
    } else if (pa >= XENON_XGPU_MMIO_BASE &&
               pa < (XENON_XGPU_MMIO_BASE + XENON_XGPU_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_XGPU_MMIO_BASE;

        if (off + size <= XENON_XGPU_MMIO_SIZE) {
            if (size == 4 && (off & 3) == 0) {
                xenon_xgpu_mmio_write32(xms, off, ldl_be_p(buf));
            } else {
                memcpy(xms->xgpu_mmio_data + off, buf, size);
            }
        }
    } else if (pa >= XENON_IIC_MMIO_BASE &&
               pa < (XENON_IIC_MMIO_BASE + XENON_IIC_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        xenon_iic_write(xms, pa - XENON_IIC_MMIO_BASE, data, size);
    } else {
        address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, buf, size);
    }
    if (xms && xms->trace_boot &&
        pa >= XENON_NAND_BASE &&
        pa < (XENON_NAND_BASE + XENON_NAND_MMIO_SIZE) &&
        xms->nand_trace_writes < 16) {
        info_report("xbox360: nand-write EA=0x%016" PRIx64 " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data);
        xms->nand_trace_writes++;
    }
    if (xms && xms->trace_boot &&
        pa >= 0x00100000 &&
        pa < 0x00400000 &&
        xms->soc_trace_writes < 256) {
        info_report("xbox360: stage-write EA=0x%016" PRIx64 " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data);
        xms->soc_trace_writes++;
    } else if (xms && xms->trace_boot &&
               pa < 0x00100000 &&
               pa != 0x61010 &&
               data != 0 &&
               xms->soc_trace_writes < 512) {
        info_report("xbox360: soc-write-nz EA=0x%016" PRIx64 " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data);
        xms->soc_trace_writes++;
    }
    if (xms && xms->trace_boot &&
        ((pa >= 0x00020000 && pa < 0x00024000) ||
         (pa >= 0x00061000 && pa < 0x00061200)) &&
        xms->secotp_trace_writes < 128) {
        info_report("xbox360: secotp/prv-write EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64 " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data);
        xms->secotp_trace_writes++;
    }
    if (xms && xms->trace_boot &&
        pa >= XENON_XGPU_MMIO_BASE &&
        pa < (XENON_XGPU_MMIO_BASE + XENON_XGPU_MMIO_SIZE) &&
        xms->xgpu_trace_writes < 64) {
        info_report("xbox360: xgpu-mmio-write EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64 " size=%u val=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data);
        xms->xgpu_trace_writes++;
    }
    if (w->owner && pa <= 0x61017 && (pa + size) > 0x61010) {
        uint8_t post_buf[8];
        uint64_t post_raw;
        uint64_t post;
        const char *desc;
        CPUPPCState *env = xms->boot_cpu ? &xms->boot_cpu->env : NULL;

        address_space_read(&address_space_memory, 0x61010, MEMTXATTRS_UNSPECIFIED,
                           post_buf, sizeof(post_buf));
        post_raw = ldq_be_p(post_buf);
        post = post_raw >> 56;
        desc = xenon_decode_post_code(post);

        if (xms->rgh2_patches && !xms->rgh2_patches_applied &&
            post == 0xD1) {
            xenon_apply_rgh2_patches(xms);
        }
        if (xms->trace_boot && post == 0xF2 && env) {
            info_report("xbox360: F2 context NIP=0x%016" PRIx64
                        " SRR0=0x%016" PRIx64 " LR=0x%016" PRIx64
                        " CTR=0x%016" PRIx64,
                        env->nip, env->spr[SPR_SRR0],
                        env->spr[SPR_LR], env->spr[SPR_CTR]);
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

            info_report("xbox360: AF context NIP=0x%016" PRIx64
                        " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                        " r0=0x%016" PRIx64 " r1=0x%016" PRIx64
                        " r2=0x%016" PRIx64 " r3=0x%016" PRIx64
                        " r4=0x%016" PRIx64 " r5=0x%016" PRIx64,
                        env->nip, env->spr[SPR_LR], env->spr[SPR_CTR],
                        env->gpr[0], env->gpr[1], env->gpr[2],
                        env->gpr[3], env->gpr[4], env->gpr[5]);
            info_report("xbox360: AF regs e1040000(be)=0x%08" PRIx32
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
            info_report("xbox360: AE context NIP=0x%016" PRIx64
                        " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                        " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                        " HSRR0=0x%016" PRIx64 " HSRR1=0x%016" PRIx64,
                        env->nip, env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                        env->spr[SPR_DAR], env->spr[SPR_DSISR],
                        env->spr[SPR_HSRR0], env->spr[SPR_HSRR1]);
        }
        if (xms->trace_boot && post == 0x84 && env) {
            info_report("xbox360: 84 context NIP=0x%016" PRIx64
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
                info_report("xbox360: trace counters reset at HWINIT entry");
            }
            if (xms->trace_boot && post == 0x40) {
                xms->xgpu_trace_reads = 0;
                xms->xgpu_trace_writes = 0;
                xms->smc_trace_reads = 0;
                xms->smc_trace_writes = 0;
                info_report("xbox360: trace counters reset at CD entry");
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

                info_report("xbox360: context SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                            " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64,
                            env->spr[SPR_SRR0], env->spr[SPR_SRR1],
                            env->spr[SPR_DAR], env->spr[SPR_DSISR]);
                address_space_read(&address_space_memory, srr0, MEMTXATTRS_UNSPECIFIED,
                                   srr0_bytes, sizeof(srr0_bytes));
                info_report("xbox360: bytes@SRR0[0x%016" PRIx64 "]=%02x %02x %02x %02x "
                            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                            srr0,
                            srr0_bytes[0], srr0_bytes[1], srr0_bytes[2], srr0_bytes[3],
                            srr0_bytes[4], srr0_bytes[5], srr0_bytes[6], srr0_bytes[7],
                            srr0_bytes[8], srr0_bytes[9], srr0_bytes[10], srr0_bytes[11],
                            srr0_bytes[12], srr0_bytes[13], srr0_bytes[14], srr0_bytes[15]);
                address_space_read(&address_space_memory, real_ra, MEMTXATTRS_UNSPECIFIED,
                                   real_bytes, sizeof(real_bytes));
                info_report("xbox360: bytes@RealRA[0x%016" PRIx64 "]=%02x %02x %02x %02x "
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
}

static const MemoryRegionOps xenon_seceng_ops = {
    .read = xenon_seceng_read,
    .write = xenon_seceng_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void xenon_pc_log_tick(void *opaque)
{
    XenonMachineState *xms = opaque;
    uint64_t pc;
    int64_t now_ms;
    CPUPPCState *env;

    if (!xms->boot_cpu) {
        return;
    }

    if (!xms->trace_boot) {
        return;
    }

    env = &xms->boot_cpu->env;
    pc = env->nip;
    now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (pc != xms->last_logged_pc) {
        if (xms->pc_log_count < 64 ||
            (pc >> 12) != (xms->last_logged_pc >> 12) ||
            (now_ms - xms->last_pc_log_ms) >= 200) {
            info_report("xbox360: pc=0x%016" PRIx64, pc);
            xms->last_pc_log_ms = now_ms;
        }
        xms->last_logged_pc = pc;
        xms->pc_log_count++;
    }

    {
        uint64_t vector = 0;
        bool is_alias = false;

        if (xenon_decode_exception_vector_pc(pc, &vector, &is_alias) &&
            pc != xms->last_exception_pc) {
            info_report("xbox360: exception vector 0x%04" PRIx64
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
                        env->spr[SPR_LR], env->spr[SPR_CTR],
                        env->msr, env->spr[SPR_LPCR],
                        env->spr[SPR_RMOR], env->spr[SPR_HRMOR],
                        env->spr[SPR_XENON_PPE_TLB_INDEX],
                        env->spr[SPR_XENON_PPE_TLB_VPN],
                        env->spr[SPR_XENON_PPE_TLB_RPN],
                        env->pending_interrupts);
            if (vector == 0x400) {
                uint8_t srr0_bytes[16] = { 0 };
                uint64_t srr0 = env->spr[SPR_SRR0];

                address_space_read(&address_space_memory, srr0, MEMTXATTRS_UNSPECIFIED,
                                   srr0_bytes, sizeof(srr0_bytes));
                info_report("xbox360: ISI detail SRR1[0x40000000]=%d bytes@SRR0=%02x %02x %02x %02x",
                            (env->spr[SPR_SRR1] & 0x40000000ULL) ? 1 : 0,
                            srr0_bytes[0], srr0_bytes[1], srr0_bytes[2], srr0_bytes[3]);
            }
            xms->last_exception_pc = pc;
        }
    }

    if (pc >= XENON_EXC_ALIAS_BASE &&
        pc < (XENON_EXC_ALIAS_BASE + XENON_EXC_ALIAS_SIZE)) {
        if (pc != xms->last_exc_handler_pc) {
            info_report("xbox360: exc-handler pc=0x%016" PRIx64
                        " off=0x%04" PRIx64
                        " LR=0x%016" PRIx64 " CTR=0x%016" PRIx64
                        " SRR0=0x%016" PRIx64 " SRR1=0x%016" PRIx64
                        " DAR=0x%016" PRIx64 " DSISR=0x%016" PRIx64
                        " XTLBV=0x%016" PRIx64 " XTLBR=0x%016" PRIx64
                        " pending=0x%08x",
                        pc, (uint64_t)(pc - XENON_EXC_ALIAS_BASE),
                        env->spr[SPR_LR], env->spr[SPR_CTR],
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
                info_report("xbox360: exc-handler repeat pc=0x%016" PRIx64
                            " count=%u LR=0x%016" PRIx64 " CTR=0x%016" PRIx64,
                            pc, xms->exc_handler_same_pc_count,
                            env->spr[SPR_LR], env->spr[SPR_CTR]);
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
            info_report("xbox360: cd-poll @0x478 r10(EA)=0x%016" PRIx64
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
        info_report("xbox360: hwinit-fetch[%u] ip_ea=0x%016" PRIx64
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

        info_report("xbox360: hwinit-decode[%u] insn=0x%08" PRIx32
                    " op=0x%02" PRIx32 " o1=0x%02" PRIx32 " o2=0x%02" PRIx32
                    " ip_ea=0x%016" PRIx64,
                    xms->hwinit_fetch_logs, insn, op, o1, o2, env->gpr[16]);
        xms->hwinit_fetch_logs++;
    }
    if (pc == 0x30036e0 && xms->trace_boot) {
        info_report("xbox360: hwinit-outfailure ip_ea=0x%016" PRIx64
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
        info_report("xbox360: cd-thread-check r2=0x%016" PRIx64
                    " base=0x%016" PRIx64
                    " slots=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
                    " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                    " tb=%" PRIu64 " r3=0x%016" PRIx64,
                    env->gpr[2], base,
                    slot[0], slot[1], slot[2], slot[3], slot[4], slot[5],
                    cpu_ppc_load_tbl(env), env->gpr[3]);
    }
    timer_mod(xms->pc_log_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

static bool xenon_is_valid_nand_magic(const uint8_t *nand, size_t nand_size)
{
    uint16_t magic;

    if (nand_size < 2) {
        return false;
    }

    magic = ((uint16_t)nand[0] << 8) | nand[1];
    return magic == 0xFF4F || magic == 0x0F4F || magic == 0x0F3F;
}

static bool xenon_parse_fuses(const char *path, uint64_t fuse_lines[XENON_FUSE_LINES])
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    int out_idx = 0;

    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        error_report("xbox360: failed to read fuses file '%s': %s",
                     path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        return false;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL && out_idx < XENON_FUSE_LINES; i++) {
        const char *line = lines[i];
        const char *hex = line;
        char *trimmed;
        char *colon;
        uint64_t value;

        trimmed = g_strstrip((char *)line);
        if (!trimmed[0] || trimmed[0] == '#') {
            continue;
        }

        colon = strstr(trimmed, ":");
        if (colon) {
            hex = colon + 1;
        } else {
            hex = trimmed;
        }
        while (*hex && g_ascii_isspace(*hex)) {
            hex++;
        }

        if (qemu_strtou64(hex, NULL, 16, &value) != 0) {
            error_report("xbox360: invalid fuse line '%s' in '%s'", trimmed, path);
            return false;
        }

        fuse_lines[out_idx++] = value;
    }

    if (out_idx < XENON_FUSE_LINES) {
        error_report("xbox360: fuses file '%s' has %d values, expected %d",
                     path, out_idx, XENON_FUSE_LINES);
        return false;
    }

    return true;
}

static void xenon_fill_fuses_into_srom(uint8_t *srom, const uint64_t fuse_lines[XENON_FUSE_LINES])
{
    for (int i = 0; i < XENON_FUSE_LINES; i++) {
        uint64_t be = cpu_to_be64(fuse_lines[i]);
        hwaddr line_off = XENON_FUSE_BASE + (i * XENON_FUSE_LINE_STRIDE);

        for (int r = 0; r < XENON_FUSE_REPLICA_COUNT; r++) {
            memcpy(srom + line_off + (r * sizeof(be)), &be, sizeof(be));
        }
    }
}

/*
 * 1BL hands off with SRR0=0x0100xxxx very early. On real Xenon this is backed
 * by a custom MMU path driven by PPE_TLB_* SPRs. Until that path exists in
 * QEMU, install a coarse BAT mapping so instruction fetch can proceed.
 */
static void xenon_install_boot_bat(CPUPPCState *env)
{
    const target_ulong bepi_boot = 0x01000000UL & BATU32_BEPI;
    const target_ulong bepi_cb = 0x03000000UL & BATU32_BEPI;
    const target_ulong bl_16mb = (0x7fUL << 2) & BATU32_BL;
    const target_ulong batu_boot = bepi_boot | bl_16mb | BATU32_VS | BATU32_VP;
    const target_ulong batl_boot = (0x00000000UL & BATL32_BRPN) | 0x2UL; /* RWX */
    const target_ulong batu_cb = bepi_cb | bl_16mb | BATU32_VS | BATU32_VP;
    const target_ulong batl_cb = (0x03000000UL & BATL32_BRPN) | 0x2UL; /* RWX */

    if (env->nb_BATs == 0) {
        return;
    }

    env->IBAT[0][0] = batu_boot;
    env->IBAT[1][0] = batl_boot;
    env->DBAT[0][0] = batu_boot;
    env->DBAT[1][0] = batl_boot;
    if (env->nb_BATs > 1) {
        env->IBAT[0][1] = batu_cb;
        env->IBAT[1][1] = batl_cb;
        env->DBAT[0][1] = batu_cb;
        env->DBAT[1][1] = batl_cb;
    }
}

static void xenon_init(MachineState *machine)
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
                    xms->smc_avpack_type,
                    xms->smc_uart, xms->trace_boot);
    memory_region_init_io(&xms->smc, OBJECT(machine), &xenon_smc_ops, xms,
                          "xbox360.smc", XENON_SMC_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_SMC_BASE, &xms->smc);
    memory_region_init_io(&xms->nb_mmio, OBJECT(machine), &xenon_nb_mmio_ops, xms,
                          "xbox360.nb-mmio", XENON_NB_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), XENON_NB_MMIO_BASE, &xms->nb_mmio);

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

            w->base = bases[i];
            w->name = g_strdup_printf("xbox360.seceng[%d]", i);
            w->owner = xms;
            memory_region_init_io(&w->mr, OBJECT(machine), &xenon_seceng_ops, w,
                                  w->name, XENON_SECENG_REGION_SIZE);
            memory_region_add_subregion(get_system_memory(), w->base, &w->mr);
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
                     &spr_read_generic, &spr_write_generic,
                     0x00000000);
        spr_register(env, SPR_XENON_PPE_TLB_RPN, "PPE_TLB_RPN",
                     SPR_NOACCESS, SPR_NOACCESS,
                     &spr_read_generic, &spr_write_generic,
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
    xms->last_pc_log_ms = 0;
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
    xms->smc_trace_reads = 0;
    xms->smc_trace_writes = 0;
    xms->hwinit_fetch_logs = 0;
    xms->hwinit_bytecode_dumped = false;
    xms->nb_training_done = false;
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
    stl_le_p(xms->pci_cfg_data + 0x8000, 0x01198030U); /* config */
    stl_le_p(xms->pci_cfg_data + 0x8004, 0x00000600U); /* status */
    stl_le_p(xms->pci_cfg_data + 0x8008, 0x000000FFU); /* command (NO_CMD) */
    stl_le_p(xms->pci_cfg_data + 0x800c, 0x00F70030U); /* address */
    stl_le_p(xms->pci_cfg_data + 0x8014, 0x00000100U); /* logical */
    stl_le_p(xms->pci_cfg_data + 0x8018, 0x00000100U); /* physical */
    xenon_xgpu_init_pci_config(xms);

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
    if (xms->trace_boot) {
        xms->pc_log_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, xenon_pc_log_tick, xms);
        timer_mod(xms->pc_log_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }

    warn_report("xbox360: VMX128 is not implemented yet; boot path currently uses stubs/scaffold");
    info_report("xbox360: loaded nand='%s' (0x%zx), fuses='%s', 1bl='%s'",
                xms->nand_path, nand_size, xms->fuses_path, xms->onebl_path);
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

static char *xenon_machine_get_nand(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->nand_path);
}

static void xenon_machine_set_nand(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->nand_path);
    xms->nand_path = g_strdup(value);
    xms->user_set_nand = true;
}

static char *xenon_machine_get_fuses(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->fuses_path);
}

static void xenon_machine_set_fuses(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->fuses_path);
    xms->fuses_path = g_strdup(value);
    xms->user_set_fuses = true;
}

static char *xenon_machine_get_onebl(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->onebl_path);
}

static bool xenon_machine_get_trace_boot(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->trace_boot;
}

static void xenon_machine_set_trace_boot(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->trace_boot = value;
    xms->user_set_trace_boot = true;
}

static bool xenon_machine_get_pretty_post(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->pretty_post;
}

static void xenon_machine_set_pretty_post(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->pretty_post = value;
    xms->user_set_pretty_post = true;
}

static bool xenon_machine_get_rgh2_patches(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return xms->rgh2_patches;
}

static void xenon_machine_set_rgh2_patches(Object *obj, bool value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    xms->rgh2_patches = value;
    xms->user_set_rgh2_patches = true;
}

static void xenon_machine_set_onebl(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->onebl_path);
    xms->onebl_path = g_strdup(value);
    xms->user_set_onebl = true;
}

static char *xenon_machine_get_config(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->config_path);
}

static void xenon_machine_set_config(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_free(xms->config_path);
    xms->config_path = g_strdup(value);
}

static char *xenon_machine_get_boot_mode(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xenon_reason_to_boot_mode(xms->smc_power_on_reason));
}

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

static char *xenon_machine_get_smc_uart(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xms->smc_uart);
}

static void xenon_machine_set_smc_uart(Object *obj, const char *value, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    g_autofree char *lower = g_ascii_strdown(value ? value : "", -1);
    g_free(xms->smc_uart);
    xms->smc_uart = g_strdup(lower);
    xms->user_set_smc_uart = true;
}

static char *xenon_machine_get_console_revision(Object *obj, Error **errp)
{
    XenonMachineState *xms = XENON_MACHINE(obj);
    return g_strdup(xenon_console_revision_name(xms->console_revision));
}

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

static void xenon_machine_instance_init(Object *obj)
{
    XenonMachineState *xms = XENON_MACHINE(obj);

    xms->nand_path = g_strdup("nand.bin");
    xms->fuses_path = g_strdup("fuses.txt");
    xms->onebl_path = g_strdup("1bl.bin");
    xms->config_path = g_strdup("");
    xms->smc_uart = g_strdup("stdio");
    xms->console_revision = XENON_CONSOLE_JASPER;
    xms->smc_power_on_reason = XENON_SMC_PWRBTN;
    xms->smc_avpack_type = XENON_SMC_AVPACK_DEFAULT;
    xms->trace_boot = false;
    xms->pretty_post = true;
    xms->rgh2_patches = true;
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
    xms->user_set_boot_mode = false;
    xms->user_set_smc_uart = false;
    xms->user_set_smc_avpack = false;
    xms->user_set_console_revision = false;
    xms->rgh2_patches_applied = false;
}

static void xenon_machine_finalize(Object *obj)
{
    XenonMachineState *xms = XENON_MACHINE(obj);

    g_free(xms->nand_path);
    g_free(xms->fuses_path);
    g_free(xms->onebl_path);
    g_free(xms->config_path);
    g_free(xms->smc_uart);
    g_free(xms->srom_data);
    g_free(xms->nand_raw_data);
    g_free(xms->nand_mmio_data);
    g_free(xms->nb_mmio_data);
    g_free(xms->soc_e1_data);
    g_free(xms->iic_mmio_data);
    g_free(xms->pci_cfg_data);
    g_free(xms->xgpu_mmio_data);
    if (xms->pc_log_timer) {
        timer_free(xms->pc_log_timer);
    }
    for (int i = 0; i < ARRAY_SIZE(xms->seceng_windows); i++) {
        g_free(xms->seceng_windows[i].name);
    }
}

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
}

static const TypeInfo xenon_machine_typeinfo = {
    .name          = TYPE_XENON_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(XenonMachineState),
    .instance_init = xenon_machine_instance_init,
    .instance_finalize = xenon_machine_finalize,
    .class_init    = xenon_machine_class_init,
};

static void xenon_machine_register_types(void)
{
    type_register_static(&xenon_machine_typeinfo);
}

type_init(xenon_machine_register_types)
