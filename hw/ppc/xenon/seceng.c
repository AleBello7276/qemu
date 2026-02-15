/*
 * QEMU Xbox 360 (Xenon) machine - SecEng window handlers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/machine-priv.h"
#include "hw/ppc/xenon/postcodes.h"
#include "hw/ppc/xenon/iic.h"
#include "hw/ppc/xenon/xgpu.h"
#include "target/ppc/spr_common.h"
#include "target/ppc/mmu-hash64.h"

#define SECENG_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_SECENG, __VA_ARGS__)
#define NAND_INFO(...)  XENON_LOG_INFO(xms, XENON_LOG_MODULE_NAND, __VA_ARGS__)
#define SOC_INFO(...)   XENON_LOG_INFO(xms, XENON_LOG_MODULE_SOC, __VA_ARGS__)
#define XGPU_INFO(...)  XENON_LOG_INFO(xms, XENON_LOG_MODULE_XGPU, __VA_ARGS__)

/*
 * Translate a Xenon "SecEng region" effective address to the underlying
 * physical address used for the access.
 *
 * Purpose: implement Xenon SecEng region decode (physical vs hashed/encrypted
 * vs SoC) so early boot accesses land on the correct underlying PA.
 *
 * This implements the same basic region decode xenon-emu models (physical vs
 * hashed/encrypted vs SoC), where some regions mask to 30 bits.
 */
hwaddr xenon_seceng_translate(uint64_t in_ea)
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

/*
 * Decide whether we can satisfy a SecEng window access via a direct memcpy to
 * the backing RAM buffer (fast path), without going through the full address
 * space dispatch.
 *
 * Purpose: avoid slow address_space dispatch for plain RAM while still routing
 * MMIO subranges through their device models.
 *
 * This intentionally excludes MMIO subranges that live inside the "RAM"
 * physical window on Xenon (eg IIC/PRV), so behavior stays device-accurate.
 */
bool xenon_seceng_ram_fastpath_ok(const XenonMachineState *xms,
                                  hwaddr pa, unsigned size)
{
    if (!xms->ram_ptr || size < 1 || size > 8) {
        return false;
    }
    if (pa < 0x00100000ULL || pa + size > xms->ram_size) {
        return false;
    }
    if (pa >= XENON_IIC_MMIO_BASE &&
        pa < (XENON_IIC_MMIO_BASE + XENON_IIC_MMIO_SIZE)) {
        return false;
    }
    if (pa >= XENON_PRV_POR_STATUS_ADDR &&
        pa < (XENON_PRV_POR_STATUS_ADDR + XENON_PRV_MMIO_SIZE)) {
        return false;
    }

    return true;
}

/*
 * Best-effort translation using Xenon-specific "soft TLB" SPRs.
 *
 * Purpose: during very early boot (before normal hashed page tables are fully
 * established), firmware sometimes uses a software-managed mapping mechanism.
 * We use this helper for debugging/trace-only translation (eg dumping CD
 * buffers) when QEMU's normal debug page translation may not see the mapping.
 */
bool xenon_soft_tlb_translate_ea(CPUPPCState *env, uint64_t ea, hwaddr *pa_out)
{
    uint64_t pte0 = env->spr[SPR_XENON_PPE_TLB_VPN];
    uint64_t pte1 = env->spr[SPR_XENON_PPE_TLB_RPN];
    uint64_t hid6 = env->spr[SPR_XENON_HID6];
    uint64_t page_mask;
    uint64_t vpn;
    uint64_t rpn;
    unsigned page_shift = 12;

    if (!(pte0 & HPTE64_V_VALID)) {
        return false;
    }

    if (pte0 & HPTE64_V_LARGE) {
        uint8_t lb = (hid6 >> 16) & 0xF;
        uint8_t lp = (pte1 >> 12) & 0x1;
        uint8_t idx = lp ? (lb & 0x3) : ((lb >> 2) & 0x3);

        switch (idx) {
        case 0:
            page_shift = 24;
            break;
        case 1:
            page_shift = 20;
            break;
        case 2:
            page_shift = 16;
            break;
        default:
            page_shift = 12;
            break;
        }
    }

    page_mask = (1ULL << page_shift) - 1ULL;
    vpn = ((pte0 & HPTE64_V_AVPN) << 16) & ~page_mask;
    if ((ea & ~page_mask) != vpn) {
        return false;
    }

    rpn = (pte1 & HPTE64_R_RPN) & ~page_mask;
    *pa_out = (hwaddr)(rpn | (ea & page_mask));
    return true;
}

/*
 * MemoryRegion .read() handler for the Xenon SecEng alias windows.
 *
 * Purpose: provide the guest-visible behavior of reads through the security
 * engine address regions, including:
 * - region decode (EA->PA) used by Xenon
 * - special handling for certain emulated blocks (PCI config, PRV, SoC E1,
 *   XGPU MMIO, IIC)
 * - trace-boot targeted logging for bring-up
 */
static uint64_t xenon_seceng_read(void *opaque, hwaddr offset, unsigned size)
{
    static unsigned cd_buf_read_logs;
    XenonSecEngWindow *w = opaque;
    uint64_t ea = w->base + offset;
    hwaddr pa = xenon_seceng_translate(ea);
    uint8_t buf[8] = { 0 };
    XenonMachineState *xms = w->owner;

    if (xenon_seceng_ram_fastpath_ok(xms, pa, size)) {
        memcpy(buf, xms->ram_ptr + pa, size);
    } else if (pa >= XENON_PCI_CFG_BASE &&
        pa < (XENON_PCI_CFG_BASE + XENON_PCI_CFG_SIZE) &&
        size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_PCI_CFG_BASE;

        if (off + size <= XENON_PCI_CFG_SIZE) {
            memcpy(buf, xms->pci_cfg_data + off, size);
        } else {
            memset(buf, 0xFF, size);
        }
    } else if (pa >= XENON_PRV_POR_STATUS_ADDR &&
               pa < (XENON_PRV_POR_STATUS_ADDR + XENON_PRV_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_PRV_POR_STATUS_ADDR;

        if (off + size <= XENON_PRV_MMIO_SIZE) {
            memcpy(buf, xms->prv_mmio_data + off, size);
        } else {
            memset(buf, 0, size);
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
        NAND_INFO("nand-read EA=0x%016" PRIx64 " PA=0x%08" PRIx64
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
        SOC_INFO("soc-read  EA=0x%016" PRIx64 " PA=0x%08" PRIx64
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
    if (xms && xms->trace_boot &&
        pa >= 0x00280000 && pa < 0x00280200 &&
        cd_buf_read_logs < 64) {
        uint64_t v = 0;
        uint64_t nip = xms->boot_cpu ? (uint64_t)xms->boot_cpu->env.nip : 0;

        switch (size) {
        case 1: v = buf[0]; break;
        case 2: v = lduw_be_p(buf); break;
        case 4: v = ldl_be_p(buf); break;
        case 8: v = ldq_be_p(buf); break;
        default: break;
        }
        info_report("xbox360: cd-buf-read EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64
                    " NIP=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, v, nip);
        cd_buf_read_logs++;
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

/*
 * MemoryRegion .write() handler for the Xenon SecEng alias windows.
 *
 * Purpose: provide the guest-visible behavior of writes through the security
 * engine address regions, including a few Xenon-specific semantics:
 * - some PCI config fields behave as strap-derived read-only bits
 * - IIC/MMIO writes must go through the device model
 * - writes that touch the POST register (0x61010) trigger patch hooks and
 *   trace logic (used heavily during secure boot bring-up)
 */
static void xenon_seceng_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    static unsigned cd_buf_write_logs;
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
            if (off == 0x8000 && size == 4) {
                /*
                 * SFCX flash geometry/type fields are strap-derived and
                 * effectively read-only. Preserve them across guest writes.
                 */
                const uint32_t strap_mask =
                    (0x3U << 4) | (0x3U << 17) | (0x3U << 19) | (0xFU << 21);
                uint32_t cur = ldl_le_p(xms->pci_cfg_data + 0x8000);
                uint32_t base = (xms->console_revision == XENON_CONSOLE_XENON) ?
                                0x01198030U : 0x00043000U;
                cur = (cur & ~strap_mask) | (base & strap_mask);
                stl_le_p(xms->pci_cfg_data + 0x8000, cur);
            }
            if (off == 0x8004 && size == 2) {
                /*
                 * SFCX status write semantics: write then readback returns
                 * WP/BY pins asserted.
                 */
                stl_le_p(xms->pci_cfg_data + 0x8004, 0x00000600U);
            }
        }
    } else if (pa >= XENON_PRV_POR_STATUS_ADDR &&
               pa < (XENON_PRV_POR_STATUS_ADDR + XENON_PRV_MMIO_SIZE) &&
               size >= 1 && size <= 8) {
        hwaddr off = pa - XENON_PRV_POR_STATUS_ADDR;

        if (off + size <= XENON_PRV_MMIO_SIZE) {
            memcpy(xms->prv_mmio_data + off, buf, size);
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                                buf, size);
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
    if (xms && xms->trace_boot &&
        pa >= 0x00280000 && pa < 0x00280200 &&
        cd_buf_write_logs < 128) {
        uint64_t nip = xms->boot_cpu ? (uint64_t)xms->boot_cpu->env.nip : 0;

        info_report("xbox360: cd-buf-write EA=0x%016" PRIx64
                    " PA=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64
                    " NIP=0x%016" PRIx64,
                    ea, (uint64_t)pa, size, data, nip);
        cd_buf_write_logs++;
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

        xenon_patches_on_post_write(xms, (uint8_t)post, post_raw, env);
        xenon_trace_on_post_observed(xms, post_raw, (uint8_t)post, desc, ea, env);
    }
}

/*
 * SecEng window MemoryRegionOps.
 *
 * Purpose: bind the read/write handlers above, enforce big-endian device
 * accesses, and allow unaligned accesses (the guest uses 1/2/4/8 byte ops).
 */
const MemoryRegionOps xenon_seceng_ops = {
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
