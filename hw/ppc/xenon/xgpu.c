/*
 * Xbox 360 Xenon machine XGPU helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/xgpu.h"

#define XGPU_REG(off) ((uint32_t)((off) >> 2))
#define XGPU_REG_CONFIG_CNTL            0x0038
#define XGPU_REG_RBBM_CNTL              0x003B
#define XGPU_REG_RBBM_SOFT_RESET        0x003C
#define XGPU_REG_CP_RB_BASE             0x01C0
#define XGPU_REG_CP_RB_CNTL             0x01C1
#define XGPU_REG_CP_RB_WPTR             0x01C5
#define XGPU_REG_CP_INT_STATUS          0x01F3
#define XGPU_REG_CP_INT_ACK             0x01F4
#define XGPU_REG_CP_ME_STATUS           0x01F7
#define XGPU_REG_CP_ME_RAM_WADDR        0x01F8
#define XGPU_REG_CP_ME_RAM_RADDR        0x01F9
#define XGPU_REG_CP_ME_RAM_DATA         0x01FA
#define XGPU_REG_RBBM_DEBUG             0x039B
#define XGPU_REG_CP_PFP_UCODE_ADDR      0x045F
#define XGPU_REG_CP_PFP_UCODE_DATA      0x0460
#define XGPU_REG_RBBM_STATUS            0x05D0
#define XGPU_REG_D1GRPH_ALT_SURFACE_ADDRESS 0x0A10
#define XGPU_REG_D1GRPH_ALT_PITCH       0x0A11
#define XGPU_REG_D1GRPH_ALT_MODE        0x0A12
#define XGPU_REG_MH_STATUS              0x0A07
#define XGPU_REG_COHER_SIZE_HOST        0x0A2F
#define XGPU_REG_COHER_BASE_HOST        0x0A30
#define XGPU_REG_COHER_STATUS_HOST      0x0A31
#define XGPU_REG_RB_EDRAM_TIMING        0x0F00
#define XGPU_REG_RB_EDRAM_INFO          0x0F02
#define XGPU_REG_D1CRTC_CONTROL         0x180A
#define XGPU_REG_D1GRPH_ENABLE          0x1840
#define XGPU_REG_D1GRPH_PRIMARY_SURFACE_ADDRESS 0x1844
#define XGPU_REG_D1GRPH_PITCH           0x1848
#define XGPU_REG_D1GRPH_X_END           0x184D
#define XGPU_REG_D1GRPH_Y_END           0x184E
#define XGPU_REG_DC_LUT_AUTOFILL        0x1928
#define XGPU_REG_D1MODE_V_COUNTER       0x194C
#define XGPU_REG_D1MODE_VBLANK_STATUS   0x194D
#define XGPU_REG_D1MODE_INT_MASK        0x1950
#define XGPU_REG_D1MODE_VBLANK_VLINE_STATUS 0x1951
#define XGPU_REG_D1MODE_VIEWPORT_SIZE   0x1961
#define XGPU_REG_ANA_ADDR               0x1E54
#define XGPU_REG_ANA_DATA               0x1E55

#define XGPU_REG_SPLL_CNTL              0x0084
#define XGPU_REG_RPLL_CNTL              0x0091
#define XGPU_REG_FPLL_CNTL              0x0099
#define XGPU_REG_MPLL_CNTL              0x00A1
#define XGPU_REG_MDLL_CNTL1             0x00A3

#define XGPU_MH_STATUS_READY_BIT        0x02000000U
#define XGPU_D1MODE_V_COUNTER_DEFAULT   720U

/*
 * Load a 32-bit big-endian XGPU register from the MMIO shadow buffer.
 *
 * Purpose: centralize register indexing and endian handling for the XGPU model.
 */
static inline uint32_t xenon_xgpu_load32(XenonMachineState *xms, uint32_t reg)
{
    return ldl_be_p(xms->xgpu_mmio_data + ((hwaddr)reg << 2));
}

/*
 * Store a 32-bit big-endian XGPU register into the MMIO shadow buffer.
 *
 * Purpose: keep the shadow buffer authoritative for non-special registers and
 * provide deterministic readback.
 */
static inline void xenon_xgpu_store32(XenonMachineState *xms, uint32_t reg, uint32_t v)
{
    stl_be_p(xms->xgpu_mmio_data + ((hwaddr)reg << 2), v);
}

/*
 * Select XGPU device/revision profile based on console motherboard revision.
 *
 * Purpose: some PCI IDs / register availability differ across revisions; keep
 * behavior aligned with xenon-emu baselines.
 */
static void xenon_xgpu_profile(XenonConsoleRevision rev,
                               uint16_t *device_id,
                               uint8_t *rev_id,
                               bool *has_mdll_cntl1)
{
    *device_id = 0x5841;
    *rev_id = 0x01;
    *has_mdll_cntl1 = true;

    switch (rev) {
    case XENON_CONSOLE_XENON:
        *device_id = 0x5801;
        *rev_id = 0x02;
        *has_mdll_cntl1 = false;
        break;
    case XENON_CONSOLE_ZEPHYR:
        *device_id = 0x5821;
        *rev_id = 0x02;
        *has_mdll_cntl1 = false;
        break;
    case XENON_CONSOLE_FALCON:
        *device_id = 0x5821;
        *rev_id = 0x10;
        *has_mdll_cntl1 = false;
        break;
    case XENON_CONSOLE_JASPER:
        *device_id = 0x5831;
        *rev_id = 0x11;
        *has_mdll_cntl1 = false;
        break;
    case XENON_CONSOLE_TRINITY:
        *device_id = 0x5841;
        *rev_id = 0x00;
        *has_mdll_cntl1 = true;
        break;
    case XENON_CONSOLE_CORONA:
    case XENON_CONSOLE_CORONA_4GB:
        *device_id = 0x5841;
        *rev_id = 0x01;
        *has_mdll_cntl1 = true;
        break;
    case XENON_CONSOLE_WINCHESTER:
        *device_id = 0x5851;
        *rev_id = 0x01;
        *has_mdll_cntl1 = true;
        break;
    }
}

/*
 * Reset XGPU state to power-on defaults.
 *
 * Purpose: seed the minimal register set and microcode buffers needed for
 * libxenon/XeLL bring-up, and log the selected profile when tracing.
 */
void xenon_xgpu_reset(XenonMachineState *xms)
{
    bool has_mdll_cntl1;
    uint16_t device_id;
    uint8_t rev_id;

    memset(xms->xgpu_mmio_data, 0, XENON_XGPU_MMIO_SIZE);
    memset(xms->xgpu_me_ucode, 0, sizeof(xms->xgpu_me_ucode));
    memset(xms->xgpu_pfp_ucode, 0, sizeof(xms->xgpu_pfp_ucode));
    xms->xgpu_me_waddr = 0;
    xms->xgpu_me_raddr = 0;
    xms->xgpu_pfp_addr = 0;

    xenon_xgpu_profile(xms->console_revision, &device_id, &rev_id, &has_mdll_cntl1);

    xenon_xgpu_store32(xms, XGPU_REG_CONFIG_CNTL, 0x10000000U);
    xenon_xgpu_store32(xms, XGPU_REG_RBBM_DEBUG, 0x000F0000U);
    xenon_xgpu_store32(xms, XGPU_REG_RBBM_STATUS, 0x00000600U);
    /*
     * libxenon video init polls MH_STATUS bit1 at BAR0+0x281c, while
     * existing warm-boot paths also observe the high ready bit.
     */
    xenon_xgpu_store32(xms, XGPU_REG_MH_STATUS, XGPU_MH_STATUS_READY_BIT);
    xenon_xgpu_store32(xms, XGPU_REG_CP_ME_STATUS, 0x00000000U);
    xenon_xgpu_store32(xms, XGPU_REG_COHER_STATUS_HOST, 0x80000000U);
    xenon_xgpu_store32(xms, XGPU_REG_D1MODE_V_COUNTER, XGPU_D1MODE_V_COUNTER_DEFAULT);
    xenon_xgpu_store32(xms, XGPU_REG_D1MODE_VBLANK_VLINE_STATUS, 0x00000001U);
    xenon_xgpu_store32(xms, XGPU_REG_D1MODE_VIEWPORT_SIZE, 0x050002D0U);
    xenon_xgpu_store32(xms, XGPU_REG_RB_EDRAM_TIMING, 0x00060000U);

    xenon_xgpu_store32(xms, XGPU_REG_SPLL_CNTL, 0x09000000U);
    xenon_xgpu_store32(xms, XGPU_REG_RPLL_CNTL, 0x11000C00U);
    xenon_xgpu_store32(xms, XGPU_REG_FPLL_CNTL, 0x1A000001U);
    xenon_xgpu_store32(xms, XGPU_REG_MPLL_CNTL, 0x19100000U);
    if (has_mdll_cntl1) {
        xenon_xgpu_store32(xms, XGPU_REG_MDLL_CNTL1, 0x19100000U);
    }

    if (xms->trace_boot) {
        info_report("xbox360: xgpu profile revision=%s devid=0x%04x revid=0x%02x",
                    xenon_console_revision_name(xms->console_revision),
                    device_id, rev_id);
    }
}

/*
 * Initialize the XGPU portion of the PCI config shadow.
 *
 * Purpose: expose plausible vendor/device/class/revision IDs so guest code that
 * enumerates PCI sees an XGPU-like device.
 */
void xenon_xgpu_init_pci_config(XenonMachineState *xms)
{
    bool has_mdll_cntl1;
    uint16_t device_id;
    uint8_t rev_id;

    xenon_xgpu_profile(xms->console_revision, &device_id, &rev_id, &has_mdll_cntl1);

    stl_le_p(xms->pci_cfg_data + 0x10000, ((uint32_t)device_id << 16) | 0x1414U);
    stl_le_p(xms->pci_cfg_data + 0x10004, 0x00100002U);
    stl_le_p(xms->pci_cfg_data + 0x10008, 0x03800000U | rev_id);
    stl_le_p(xms->pci_cfg_data + 0x10010, 0xEC800000U);
    stl_le_p(xms->pci_cfg_data + 0x10034, 0x00000050U);
    stl_le_p(xms->pci_cfg_data + 0x10050, 0x06020001U);
    stl_le_p(xms->pci_cfg_data + 0x100DC, 0x0000C421U);
}

/*
 * Read a 32-bit value from the XGPU MMIO model.
 *
 * Purpose: implement special-case register semantics (status bits, ucode FIFO,
 * ready flags) while falling back to the MMIO shadow for ordinary registers.
 */
uint32_t xenon_xgpu_mmio_read32(XenonMachineState *xms, hwaddr off)
{
    uint32_t reg = XGPU_REG(off);
    uint32_t v = xenon_xgpu_load32(xms, reg);

    switch (reg) {
    case XGPU_REG_RBBM_STATUS:
        if (!(v & 0x00000400U)) {
            v |= 0x00000400U;
        }
        if (v & 0x00000080U) {
            v &= ~0x00000080U;
        }
        if (!(v & 0x00000600U)) {
            v |= 0x00000600U;
        }
        xenon_xgpu_store32(xms, reg, v);
        break;
    case XGPU_REG_RBBM_DEBUG:
        /*
         * xenon-emu returns the latched register value here.
         */
        break;
    case XGPU_REG_MH_STATUS:
        /*
         * Preserve guest-controlled low bits, force the MH ready bit.
         */
        v |= XGPU_MH_STATUS_READY_BIT;
        xenon_xgpu_store32(xms, reg, v);
        break;
    case XGPU_REG_CP_ME_STATUS:
        v = 0x00000000U;
        break;
    case XGPU_REG_CP_ME_RAM_DATA:
        v = xms->xgpu_me_ucode[xms->xgpu_me_raddr % ARRAY_SIZE(xms->xgpu_me_ucode)];
        xms->xgpu_me_raddr = (xms->xgpu_me_raddr + 1) % ARRAY_SIZE(xms->xgpu_me_ucode);
        break;
    case XGPU_REG_CP_PFP_UCODE_DATA:
        v = xms->xgpu_pfp_ucode[xms->xgpu_pfp_addr % ARRAY_SIZE(xms->xgpu_pfp_ucode)];
        xms->xgpu_pfp_addr = (xms->xgpu_pfp_addr + 1) % ARRAY_SIZE(xms->xgpu_pfp_ucode);
        break;
    case XGPU_REG_COHER_STATUS_HOST:
        if (v & 0x80000000U) {
            v &= ~0x80000000U;
            xenon_xgpu_store32(xms, reg, v);
        }
        break;
    case XGPU_REG_DC_LUT_AUTOFILL:
        if (v == 0x00000001U || v == 0x01000000U) {
            /*
             * libxenon waits for bit1 to signal LUT autofill completion.
             */
            v = 0x00000002U;
            xenon_xgpu_store32(xms, reg, v);
        }
        break;
    case XGPU_REG_D1MODE_V_COUNTER:
        break;
    case XGPU_REG_D1MODE_VBLANK_STATUS: {
        int64_t us = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
        v = ((us % 16667) < 500) ? 0x0000FFFFU : 0x00000000U;
        break;
    }
    default:
        break;
    }

    return v;
}

/*
 * Write a 32-bit value to the XGPU MMIO model.
 *
 * Purpose: accept guest register writes and apply modeled side effects (reset
 * behavior, MH ready enforcement, ucode RAM ports), while keeping the shadow
 * buffer in sync.
 */
void xenon_xgpu_mmio_write32(XenonMachineState *xms, hwaddr off, uint32_t v)
{
    uint32_t reg = XGPU_REG(off);

    xenon_xgpu_store32(xms, reg, v);

    switch (reg) {
    case XGPU_REG_RBBM_SOFT_RESET:
        if (v == 0) {
            xenon_xgpu_store32(xms, XGPU_REG_CONFIG_CNTL, 0x10000000U);
            xenon_xgpu_store32(xms, XGPU_REG_RBBM_STATUS, 0x00000600U);
        }
        break;
    case XGPU_REG_MH_STATUS:
        /*
         * Match xenon-emu: preserve guest value but keep MH ready set.
         */
        v |= XGPU_MH_STATUS_READY_BIT;
        xenon_xgpu_store32(xms, reg, v);
        break;
    case XGPU_REG_RB_EDRAM_TIMING:
        if ((v & 0x00060000U) < 0x00000400U) {
            v |= 0x00060000U;
            xenon_xgpu_store32(xms, reg, v);
        }
        break;
    case XGPU_REG_CP_RB_BASE:
    case XGPU_REG_CP_RB_CNTL:
    case XGPU_REG_CP_RB_WPTR:
        break;
    case XGPU_REG_CP_INT_ACK: {
        uint32_t cur = xenon_xgpu_load32(xms, XGPU_REG_CP_INT_STATUS);
        xenon_xgpu_store32(xms, XGPU_REG_CP_INT_STATUS, cur & ~v);
        break;
    }
    case XGPU_REG_CP_ME_RAM_WADDR:
        xms->xgpu_me_waddr = v % ARRAY_SIZE(xms->xgpu_me_ucode);
        break;
    case XGPU_REG_CP_ME_RAM_RADDR:
        xms->xgpu_me_raddr = v % ARRAY_SIZE(xms->xgpu_me_ucode);
        break;
    case XGPU_REG_CP_ME_RAM_DATA:
        xms->xgpu_me_ucode[xms->xgpu_me_waddr % ARRAY_SIZE(xms->xgpu_me_ucode)] = v;
        xms->xgpu_me_waddr = (xms->xgpu_me_waddr + 1) % ARRAY_SIZE(xms->xgpu_me_ucode);
        break;
    case XGPU_REG_CP_PFP_UCODE_ADDR:
        xms->xgpu_pfp_addr = v % ARRAY_SIZE(xms->xgpu_pfp_ucode);
        break;
    case XGPU_REG_CP_PFP_UCODE_DATA:
        xms->xgpu_pfp_ucode[xms->xgpu_pfp_addr % ARRAY_SIZE(xms->xgpu_pfp_ucode)] = v;
        xms->xgpu_pfp_addr = (xms->xgpu_pfp_addr + 1) % ARRAY_SIZE(xms->xgpu_pfp_ucode);
        break;
    case XGPU_REG_COHER_STATUS_HOST:
        if (!(v & 0x80000000U)) {
            xenon_xgpu_store32(xms, reg, v | 0x80000000U);
        }
        break;
    case XGPU_REG_RBBM_CNTL:
    case XGPU_REG_DC_LUT_AUTOFILL:
        if (v == 0x00000001U || v == 0x01000000U) {
            xenon_xgpu_store32(xms, reg, 0x00000002U);
        }
        break;
    case XGPU_REG_D1CRTC_CONTROL:
        /*
         * libxenon/xell writes bit24 on D1CRTC_CONTROL (BAR0+0x6028) and
         * then polls MH_STATUS bit1 (BAR0+0x281c). Model the resulting MH
         * handshake instead of seeding bit1 at reset.
         */
        if (v & 0x01000000U) {
            uint32_t mh = xenon_xgpu_load32(xms, XGPU_REG_MH_STATUS);
            mh |= 0x00000002U;
            mh |= XGPU_MH_STATUS_READY_BIT;
            xenon_xgpu_store32(xms, XGPU_REG_MH_STATUS, mh);
        }
        break;
    case XGPU_REG_D1MODE_V_COUNTER:
    case XGPU_REG_D1MODE_VBLANK_STATUS:
    case XGPU_REG_D1MODE_INT_MASK:
    case XGPU_REG_D1MODE_VBLANK_VLINE_STATUS:
    case XGPU_REG_D1MODE_VIEWPORT_SIZE:
    case XGPU_REG_RB_EDRAM_INFO:
    case XGPU_REG_COHER_SIZE_HOST:
    case XGPU_REG_COHER_BASE_HOST:
    case XGPU_REG_RBBM_DEBUG:
    case XGPU_REG_ANA_ADDR:
        break;
    case XGPU_REG_ANA_DATA: {
        /*
         * ANA writes are posted through this pair (0x7950/0x7954). Once
         * data is written, hardware advances the address latch so polling
         * code observes completion.
         */
        uint32_t addr = xenon_xgpu_load32(xms, XGPU_REG_ANA_ADDR);
        xenon_xgpu_store32(xms, XGPU_REG_ANA_ADDR, addr ^ 1U);
        break;
    }
    default:
        break;
    }
}

/*
 * Extract the guest scanout framebuffer parameters from XGPU registers.
 *
 * Purpose: used by the debug display path to mirror the guest's Xenos scanout
 * surface into a QEMU console during XeLL/libxenon bring-up.
 */
bool xenon_xgpu_get_fb_info(XenonMachineState *xms, XenonXgpuFbInfo *info)
{
    uint32_t viewport;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t base;
    uint32_t alt_base;
    uint32_t alt_pitch;
    uint32_t alt_mode;
    bool enabled;

    if (!info) {
        return false;
    }

    base = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    pitch = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_PITCH);
    alt_base = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_ALT_SURFACE_ADDRESS);
    alt_pitch = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_ALT_PITCH);
    alt_mode = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_ALT_MODE);
    width = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_X_END);
    height = xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_Y_END);
    viewport = xenon_xgpu_load32(xms, XGPU_REG_D1MODE_VIEWPORT_SIZE);
    enabled = (xenon_xgpu_load32(xms, XGPU_REG_D1GRPH_ENABLE) & 1U) != 0;

    if (!base) {
        base = alt_base;
    }
    if (!pitch) {
        pitch = alt_pitch;
    }

    if (!width) {
        width = viewport >> 16;
    }
    if (!height) {
        height = viewport & 0xFFFFU;
    }
    if (!pitch) {
        pitch = width;
    }
    if (pitch < width) {
        pitch = width;
    }

    info->base = base & 0x3FFFFFFFU;
    info->pitch = pitch;
    info->width = width;
    info->height = height;
    info->enabled = enabled && base != 0 && width != 0 && height != 0;
    info->tiled = (alt_mode & 0x00080000U) != 0;
    return true;
}
