/*
 * QEMU Xbox 360 (Xenon) machine - boot/image helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "target/ppc/mmu-hash32.h"

#define BOOT_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_BOOT, __VA_ARGS__)

/*
 * Validate the NAND "magic" / identification bytes from a raw NAND dump.
 *
 * Purpose: sanity-check that the configured NAND image looks like a Xenon-era
 * NAND before we attempt to build a logical view and boot from it.
 */
bool xenon_is_valid_nand_magic(const uint8_t *nand, size_t nand_size)
{
    uint16_t magic;

    if (nand_size < 2) {
        return false;
    }

    magic = ((uint16_t)nand[0] << 8) | nand[1];
    return magic == 0xFF4F || magic == 0x0F4F || magic == 0x0F3F;
}

/*
 * Parse fuse lines from a text file into the internal fuse array.
 *
 * Purpose: populate the per-console fuses that early boot code expects, from
 * a human-readable dump (one value per line, optional "label: hex" format).
 */
bool xenon_parse_fuses(const char *path, uint64_t fuse_lines[XENON_FUSE_LINES])
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

/*
 * Copy fuse values into the guest-visible SROM fuse replicas.
 *
 * Purpose: model the real Xenon behavior where fuse lines appear replicated in
 * the secure ROM address space at fixed offsets.
 */
void xenon_fill_fuses_into_srom(uint8_t *srom,
                                const uint64_t fuse_lines[XENON_FUSE_LINES])
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
 * Convert a raw NAND dump (data+spare per page) into the logical byte stream
 * that Xenon firmware reads via the NAND controller window.
 *
 * Purpose: firmware generally consumes the "logical" pages (without spare),
 * while our artifacts are often full raw pages; this builds the flattened view.
 */
void xenon_build_logical_nand_view(uint8_t *logical, const uint8_t *raw,
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

/*
 * Initialize PRV (pervasive) defaults at power-on-reset.
 *
 * Purpose: CB checks PRV POR status / PM control registers early; we seed those
 * registers and keep a mirrored copy in RAM for existing debug/tooling paths.
 */
void xenon_init_soc_prv_defaults(XenonMachineState *xms)
{
    uint64_t por = cpu_to_be64(XENON_PRV_POR_STATUS_INIT);
    uint64_t pmc = cpu_to_be64(XENON_PRV_PMCTRL_INIT);
    uint64_t por_rb;
    hwaddr por_off = XENON_PRV_POR_STATUS_ADDR - XENON_PRV_POR_STATUS_ADDR;
    hwaddr pmc_off = XENON_PRV_PMCTRL_ADDR - XENON_PRV_POR_STATUS_ADDR;

    memset(xms->prv_mmio_data, 0, sizeof(xms->prv_mmio_data));
    stq_be_p(xms->prv_mmio_data + por_off, XENON_PRV_POR_STATUS_INIT);
    stq_be_p(xms->prv_mmio_data + pmc_off, XENON_PRV_PMCTRL_INIT);

    /*
     * Keep a mirror in plain RAM for existing tooling/debug paths that inspect
     * low physical memory directly.
     */
    address_space_write(&address_space_memory, XENON_PRV_POR_STATUS_ADDR,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&por, sizeof(por));
    address_space_write(&address_space_memory, XENON_PRV_PMCTRL_ADDR,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&pmc, sizeof(pmc));
    por_rb = ldq_be_p(xms->prv_mmio_data + por_off);

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_BOOT)) {
        BOOT_INFO("initialized PRV defaults "
                  "POR=0x%016" PRIx64 " PMCTRL=0x%016" PRIx64
                  " readback.POR=0x%016" PRIx64,
                  (uint64_t)XENON_PRV_POR_STATUS_INIT,
                  (uint64_t)XENON_PRV_PMCTRL_INIT,
                  por_rb);
    }
}

/*
 * 1BL hands off with SRR0=0x0100xxxx very early. On real Xenon this is backed
 * by a custom MMU path driven by PPE_TLB_* SPRs. Until that path exists in
 * QEMU, install a coarse BAT mapping so instruction fetch can proceed.
 *
 * Purpose: temporary bring-up mapping so early secure boot can run without the
 * full Xenon soft-TLB/MMU behavior implemented yet.
 */
void xenon_install_boot_bat(CPUPPCState *env)
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
