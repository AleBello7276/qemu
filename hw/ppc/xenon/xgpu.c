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
#define XGPU_REG_MH_STATUS              0x0A07
#define XGPU_REG_COHER_SIZE_HOST        0x0A2F
#define XGPU_REG_COHER_BASE_HOST        0x0A30
#define XGPU_REG_COHER_STATUS_HOST      0x0A31
#define XGPU_REG_RB_EDRAM_TIMING        0x0F00
#define XGPU_REG_RB_EDRAM_INFO          0x0F02
#define XGPU_REG_D1CRTC_CONTROL         0x180A
#define XGPU_REG_DC_LUT_AUTOFILL        0x1928
#define XGPU_REG_D1MODE_V_COUNTER       0x194C
#define XGPU_REG_D1MODE_VBLANK_STATUS   0x194D
#define XGPU_REG_D1MODE_INT_MASK        0x1950
#define XGPU_REG_D1MODE_VBLANK_VLINE_STATUS 0x1951
#define XGPU_REG_D1MODE_VIEWPORT_SIZE   0x1961

#define XGPU_REG_SPLL_CNTL              0x0084
#define XGPU_REG_RPLL_CNTL              0x0091
#define XGPU_REG_FPLL_CNTL              0x0099
#define XGPU_REG_MPLL_CNTL              0x00A1
#define XGPU_REG_MDLL_CNTL1             0x00A3

static inline uint32_t xenon_xgpu_load32(XenonMachineState *xms, uint32_t reg)
{
    return ldl_be_p(xms->xgpu_mmio_data + ((hwaddr)reg << 2));
}

static inline void xenon_xgpu_store32(XenonMachineState *xms, uint32_t reg, uint32_t v)
{
    stl_be_p(xms->xgpu_mmio_data + ((hwaddr)reg << 2), v);
}

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
    xenon_xgpu_store32(xms, XGPU_REG_MH_STATUS, 0x02000000U);
    xenon_xgpu_store32(xms, XGPU_REG_CP_ME_STATUS, 0x00000000U);
    xenon_xgpu_store32(xms, XGPU_REG_COHER_STATUS_HOST, 0x80000000U);
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
        v = 0x000F0000U;
        xenon_xgpu_store32(xms, reg, v);
        break;
    case XGPU_REG_MH_STATUS:
        v |= 0x02000000U;
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
        if (v == 0x00000001U) {
            v = 0x02000000U;
            xenon_xgpu_store32(xms, reg, v);
        }
        break;
    case XGPU_REG_D1MODE_V_COUNTER:
        v = 720U;
        break;
    case XGPU_REG_D1MODE_VBLANK_STATUS: {
        int64_t us = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
        v = ((us % 16667) < 500) ? 0x0000FFFFU : 0x00000000U;
        break;
    }
    case XGPU_REG_D1MODE_VBLANK_VLINE_STATUS:
        if (!v) {
            v = 0x00000001U;
            xenon_xgpu_store32(xms, reg, v);
        }
        break;
    default:
        break;
    }

    return v;
}

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
        v |= 0x02000000U;
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
    case XGPU_REG_D1CRTC_CONTROL:
    case XGPU_REG_DC_LUT_AUTOFILL:
    case XGPU_REG_D1MODE_V_COUNTER:
    case XGPU_REG_D1MODE_VBLANK_STATUS:
    case XGPU_REG_D1MODE_INT_MASK:
    case XGPU_REG_D1MODE_VBLANK_VLINE_STATUS:
    case XGPU_REG_D1MODE_VIEWPORT_SIZE:
    case XGPU_REG_RB_EDRAM_INFO:
    case XGPU_REG_COHER_SIZE_HOST:
    case XGPU_REG_COHER_BASE_HOST:
    case XGPU_REG_RBBM_DEBUG:
        break;
    default:
        break;
    }
}
