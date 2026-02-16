/*
 * QEMU Xbox 360 (Xenon) machine - MMIO handlers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/ppc/xenon/debug.h"
#include "hw/ppc/xenon/xenon-internal.h"
#include "hw/ppc/xenon/xgpu.h"

#define NAND_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_NAND, __VA_ARGS__)
#define SOC_INFO(...)  XENON_LOG_INFO(xms, XENON_LOG_MODULE_SOC, __VA_ARGS__)
#define SMC_INFO(...)  XENON_LOG_INFO(xms, XENON_LOG_MODULE_SMC, __VA_ARGS__)
#define SATA_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_SATA, __VA_ARGS__)
#define XGPU_INFO(...) XENON_LOG_INFO(xms, XENON_LOG_MODULE_XGPU, __VA_ARGS__)

/*
 * NAND MMIO read handler.
 *
 * Purpose: expose the pre-built "logical NAND view" to the guest as a simple
 * window during early bring-up, with bounded reads and optional trace.
 */
static uint64_t xenon_nand_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t v = 0;

    if (offset >= XENON_NAND_MMIO_SIZE) {
        memset(buf, 0xFF, size);
    } else {
        hwaddr avail = XENON_NAND_MMIO_SIZE - offset;
        if (avail < size) {
            memset(buf, 0xFF, size);
            memcpy(buf, xms->nand_mmio_data + offset, avail);
        } else {
            memcpy(buf, xms->nand_mmio_data + offset, size);
        }
    }

    switch (size) {
    case 1: v = buf[0]; break;
    case 2: v = lduw_be_p(buf); break;
    case 4: v = ldl_be_p(buf); break;
    case 8: v = ldq_be_p(buf); break;
    default: break;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_NAND) && xms->nand_trace_reads < 32) {
        NAND_INFO("nand-mmio-read off=0x%08" PRIx64
                  " size=%u val=0x%016" PRIx64,
                  (uint64_t)offset, size, v);
        xms->nand_trace_reads++;
    }
    return v;
}

/*
 * NAND MMIO write handler.
 *
 * Purpose: capture guest writes to the NAND window and keep trace-boot
 * visibility. Some firmware uses this region as scratch/status.
 */
static void xenon_nand_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8];

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_NAND) && xms->nand_trace_writes < 16) {
        NAND_INFO("nand-mmio-write off=0x%08" PRIx64
                  " size=%u val=0x%016" PRIx64,
                  (uint64_t)offset, size, data);
        xms->nand_trace_writes++;
    }

    if (offset >= XENON_NAND_MMIO_SIZE) {
        return;
    }

    switch (size) {
    case 1: buf[0] = data; break;
    case 2: stw_be_p(buf, data); break;
    case 4: stl_be_p(buf, data); break;
    case 8: stq_be_p(buf, data); break;
    default:
        return;
    }

    {
        hwaddr avail = XENON_NAND_MMIO_SIZE - offset;
        if (avail < size) {
            memcpy(xms->nand_mmio_data + offset, buf, avail);
        } else {
            memcpy(xms->nand_mmio_data + offset, buf, size);
        }
    }
}

const MemoryRegionOps xenon_nand_ops = {
    .read = xenon_nand_read,
    .write = xenon_nand_write,
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

/*
 * Northbridge/SoC MMIO read handler (training/status scratch area).
 *
 * Purpose: provide a minimal NB register window sufficient for secure boot
 * progress, including modeled status bits used by memory training code.
 */
static uint64_t xenon_nb_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t v = 0;

    if (offset >= XENON_NB_MMIO_SIZE) {
        return 0;
    }

    {
        hwaddr avail = XENON_NB_MMIO_SIZE - offset;
        if (avail < size) {
            memcpy(buf, xms->nb_mmio_data + offset, avail);
        } else {
            memcpy(buf, xms->nb_mmio_data + offset, size);
        }
    }

    if (size == 4 && offset == 0x15ec && xms->nb_training_done) {
        uint32_t sts = ldl_be_p(buf) | 0x00040000U;
        stl_be_p(buf, sts);
    }
    if (size == 4 && offset == 0x15e8) {
        uint32_t ctl = ldl_be_p(buf) & ~0x20000000U;
        stl_be_p(buf, ctl);
    }
    switch (size) {
    case 1: v = buf[0]; break;
    case 2: v = lduw_be_p(buf); break;
    case 4: v = ldl_be_p(buf); break;
    case 8: v = ldq_be_p(buf); break;
    default: break;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SOC) &&
        (offset == 0x15e0 || offset == 0x15e4 ||
         offset == 0x15e8 || offset == 0x15ec) &&
        xms->soc_trace_reads < 256) {
        SOC_INFO("nb-mmio-read off=0x%05" PRIx64
                 " size=%u val=0x%016" PRIx64,
                 (uint64_t)offset, size, v);
        xms->soc_trace_reads++;
    }
    return v;
}

/*
 * Northbridge/SoC MMIO write handler.
 *
 * Purpose: store guest-programmed NB state and recognize key writes that
 * indicate training completion.
 */
static void xenon_nb_mmio_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8] = { 0 };

    if (offset >= XENON_NB_MMIO_SIZE) {
        return;
    }

    switch (size) {
    case 1: buf[0] = data; break;
    case 2: stw_be_p(buf, data); break;
    case 4: stl_be_p(buf, data); break;
    case 8: stq_be_p(buf, data); break;
    default:
        return;
    }

    {
        hwaddr avail = XENON_NB_MMIO_SIZE - offset;
        if (avail < size) {
            memcpy(xms->nb_mmio_data + offset, buf, avail);
        } else {
            memcpy(xms->nb_mmio_data + offset, buf, size);
        }
    }

    if (size == 4 && offset == 0x15e8) {
        uint32_t ctl = ldl_be_p(xms->nb_mmio_data + 0x15e8);
        uint32_t sts = ldl_be_p(xms->nb_mmio_data + 0x15ec);

        if (ctl & 0x01000000U) {
            xms->nb_training_done = true;
            sts |= 0x00040000U;
            ctl &= ~0x20000000U;
        }
        stl_be_p(xms->nb_mmio_data + 0x15e8, ctl);
        stl_be_p(xms->nb_mmio_data + 0x15ec, sts);
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SOC) &&
        (offset == 0x15e0 || offset == 0x15e4 ||
         offset == 0x15e8 || offset == 0x15ec) &&
        xms->soc_trace_writes < 256) {
        SOC_INFO("nb-mmio-write off=0x%05" PRIx64
                 " size=%u val=0x%016" PRIx64,
                 (uint64_t)offset, size, data);
        xms->soc_trace_writes++;
    }
}

const MemoryRegionOps xenon_nb_mmio_ops = {
    .read = xenon_nb_mmio_read,
    .write = xenon_nb_mmio_write,
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

/*
 * SMC MMIO read handler.
 *
 * Purpose: route reads from the SMC register window through the SMC state
 * machine and optionally log interesting status transitions during bring-up.
 */
static uint64_t xenon_smc_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint64_t v = xenon_smc_read(&xms->smc_state, offset, size);
    bool should_log = false;

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SMC) && offset <= 0x94 &&
        xms->smc_trace_reads < XENON_SMC_TRACE_LIMIT) {
        if (offset == 0x18) {
            uint32_t cur = (uint32_t)v;
            if (!xms->smc_last_status_valid || cur != xms->smc_last_uart_status) {
                xms->smc_last_uart_status = cur;
                should_log = true;
            }
        } else if (offset == 0x84) {
            uint32_t cur = (uint32_t)v;
            if (!xms->smc_last_status_valid || cur != xms->smc_last_in_status) {
                xms->smc_last_in_status = cur;
                should_log = true;
            }
        } else if (offset == 0x94) {
            uint32_t cur = (uint32_t)v;
            if (!xms->smc_last_status_valid || cur != xms->smc_last_out_status) {
                xms->smc_last_out_status = cur;
                should_log = true;
            }
        } else {
            should_log = true;
        }

        if (should_log) {
            SMC_INFO("smc-read off=0x%03" PRIx64
                     " size=%u val=0x%016" PRIx64,
                     (uint64_t)offset, size, v);
            xms->smc_trace_reads++;
        }
        xms->smc_last_status_valid = true;
    }
    return v;
}

/*
 * SMC MMIO write handler.
 *
 * Purpose: deliver guest writes into the SMC model and keep trace-boot logs
 * readable by rate-limiting.
 */
static void xenon_smc_mmio_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;

    if (offset == 0x14) {
        xenon_smc_write(&xms->smc_state, offset, data, size);
        return;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SMC) && offset <= 0x94 &&
        xms->smc_trace_writes < XENON_SMC_TRACE_LIMIT) {
        SMC_INFO("smc-write off=0x%03" PRIx64
                 " size=%u val=0x%016" PRIx64,
                 (uint64_t)offset, size, data);
        xms->smc_trace_writes++;
    }
    xenon_smc_write(&xms->smc_state, offset, data, size);
}

const MemoryRegionOps xenon_smc_ops = {
    .read = xenon_smc_mmio_read,
    .write = xenon_smc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

#define XENON_SFCX_REG_CONFIG       0x00
#define XENON_SFCX_REG_STATUS       0x04
#define XENON_SFCX_REG_COMMAND      0x08
#define XENON_SFCX_REG_ADDRESS      0x0C
#define XENON_SFCX_REG_DATA         0x10
#define XENON_SFCX_REG_LOGICAL      0x14
#define XENON_SFCX_REG_PHYSICAL     0x18
#define XENON_SFCX_REG_DATAPHYS     0x1C
#define XENON_SFCX_REG_SPAREPHYS    0x20
#define XENON_SFCX_REG_PHISON       0xFC

#define XENON_SFCX_CMD_PAGE_BUF_TO_REG 0x00
#define XENON_SFCX_CMD_REG_TO_PAGE_BUF 0x01
#define XENON_SFCX_CMD_LOG_PAGE_TO_BUF 0x02
#define XENON_SFCX_CMD_PHY_PAGE_TO_BUF 0x03
#define XENON_SFCX_CMD_DMA_LOG_TO_RAM  0x06
#define XENON_SFCX_CMD_DMA_PHY_TO_RAM  0x07
#define XENON_SFCX_CMD_NO_CMD          0xFF

#define XENON_SFCX_CFG_DMA_LEN_MASK    0x000003C0U
#define XENON_SFCX_SPARE_SIZE          (XENON_NAND_RAW_PAGE - XENON_NAND_LOGICAL_PAGE)

static inline uint32_t xenon_sfcx_pci_get32(XenonMachineState *xms, hwaddr off);
static inline void xenon_sfcx_pci_put32(XenonMachineState *xms, hwaddr off, uint32_t v);

/*
 * Perform an SFCX DMA transfer from NAND into guest RAM.
 *
 * Purpose: emulate the DMA mode used by CB/CD to fetch pages through the flash
 * controller (logical vs physical page selection via the `physical` flag).
 */
static void xenon_sfcx_dma_from_nand(XenonMachineState *xms, bool physical)
{
    uint32_t addr = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_ADDRESS);
    uint32_t config = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_CONFIG);
    uint32_t data_pa = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_DATAPHYS);
    uint32_t spare_pa = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_SPAREPHYS);
    uint32_t pages = ((config & XENON_SFCX_CFG_DMA_LEN_MASK) >> 6) + 1U;

    for (uint32_t page = 0; page < pages; page++) {
        uint8_t raw_page[XENON_NAND_RAW_PAGE];
        size_t copy = 0;
        hwaddr raw_off;

        /*
         * Match xenon-emu: address register is logical-byte based and then
         * expanded into raw 0x210-byte pages (data + spare).
         */
        raw_off = ((hwaddr)(addr / XENON_NAND_LOGICAL_PAGE) * XENON_NAND_RAW_PAGE) +
                  (addr % XENON_NAND_LOGICAL_PAGE);
        memset(raw_page, 0xFF, sizeof(raw_page));
        if (raw_off < xms->nand_raw_size) {
            copy = MIN((size_t)XENON_NAND_RAW_PAGE, xms->nand_raw_size - raw_off);
            memcpy(raw_page, xms->nand_raw_data + raw_off, copy);
        }

        cpu_physical_memory_write((hwaddr)data_pa, raw_page, XENON_NAND_LOGICAL_PAGE);
        if (physical) {
            cpu_physical_memory_write((hwaddr)spare_pa,
                                      raw_page + XENON_NAND_LOGICAL_PAGE,
                                      XENON_SFCX_SPARE_SIZE);
            spare_pa += XENON_SFCX_SPARE_SIZE;
        }

        addr += XENON_NAND_LOGICAL_PAGE;
        data_pa += XENON_NAND_LOGICAL_PAGE;
    }

    xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_ADDRESS, addr);
}

/*
 * Read a 32-bit little-endian value from the SFCX PCI config shadow.
 *
 * Purpose: SFCX registers are modeled as PCI config fields (xenon-emu style);
 * this helper centralizes byte->u32 conversion.
 */
static inline uint32_t xenon_sfcx_pci_get32(XenonMachineState *xms, hwaddr off)
{
    return ldl_le_p(xms->pci_cfg_data + 0x8000 + off);
}

/*
 * Write a 32-bit little-endian value to the SFCX PCI config shadow.
 *
 * Purpose: centralize byte ordering and offset calculation for SFCX register
 * updates stored in the PCI config shadow.
 */
static inline void xenon_sfcx_pci_put32(XenonMachineState *xms, hwaddr off, uint32_t v)
{
    stl_le_p(xms->pci_cfg_data + 0x8000 + off, v);
}

/*
 * Load the internal SFCX page buffer from NAND.
 *
 * Purpose: implement the "page to buffer" commands (logical or physical),
 * backing later DATA register accesses.
 */
static void xenon_sfcx_load_pagebuf(XenonMachineState *xms, uint32_t addr, bool physical)
{
    memset(xms->sfcx_page_buf, 0xFF, sizeof(xms->sfcx_page_buf));

    if (physical) {
        hwaddr raw_off = ((hwaddr)(addr / XENON_NAND_LOGICAL_PAGE) * XENON_NAND_RAW_PAGE) +
                         (addr % XENON_NAND_LOGICAL_PAGE);
        if (raw_off < xms->nand_raw_size) {
            size_t avail = xms->nand_raw_size - raw_off;
            size_t copy = MIN((size_t)XENON_NAND_RAW_PAGE, avail);
            memcpy(xms->sfcx_page_buf, xms->nand_raw_data + raw_off, copy);
        }
    } else {
        hwaddr logical_off = addr;
        if (logical_off < XENON_NAND_MMIO_SIZE) {
            size_t avail = XENON_NAND_MMIO_SIZE - logical_off;
            size_t copy = MIN((size_t)XENON_NAND_LOGICAL_PAGE, avail);
            memcpy(xms->sfcx_page_buf, xms->nand_mmio_data + logical_off, copy);
        }
    }
}

/*
 * SFCX MMIO read handler.
 *
 * Purpose: expose key SFCX registers (config/status/cmd/addr/data) through the
 * low MMIO alias window used during CD/XeLL stages.
 */
static uint64_t xenon_sfcx_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint32_t reg = (uint32_t)offset & 0xFF;
    uint32_t val32 = 0;
    uint8_t buf[8] = { 0 };
    uint64_t v = 0;

    if (!xms->low_mmio_aliases_enabled) {
        return 0;
    }

    switch (reg) {
    case XENON_SFCX_REG_CONFIG:
    case XENON_SFCX_REG_STATUS:
    case XENON_SFCX_REG_COMMAND:
    case XENON_SFCX_REG_ADDRESS:
    case XENON_SFCX_REG_DATA:
    case XENON_SFCX_REG_LOGICAL:
    case XENON_SFCX_REG_PHYSICAL:
    case XENON_SFCX_REG_DATAPHYS:
    case XENON_SFCX_REG_SPAREPHYS:
        val32 = xenon_sfcx_pci_get32(xms, reg);
        break;
    case XENON_SFCX_REG_PHISON:
        val32 = 0;
        break;
    default:
        val32 = 0;
        break;
    }

    stl_le_p(buf, val32);
    switch (size) {
    case 1:
        v = buf[0];
        break;
    case 2:
        v = lduw_le_p(buf);
        break;
    case 4:
        v = ldl_le_p(buf);
        break;
    case 8:
        v = ldq_le_p(buf);
        break;
    default:
        break;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SATA) && reg <= XENON_SFCX_REG_PHISON &&
        xms->sfcx_trace_reads < 128) {
        SATA_INFO("sfcx-read off=0x%03" PRIx32
                  " size=%u val=0x%016" PRIx64,
                  reg, size, v);
        xms->sfcx_trace_reads++;
    }

    return v;
}

/*
 * SFCX MMIO write handler.
 *
 * Purpose: accept flash controller commands and update register state, page
 * buffer, and DMA behavior as needed by early boot loaders.
 */
static void xenon_sfcx_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint32_t reg = (uint32_t)offset & 0xFF;
    uint8_t buf[8] = { 0 };
    uint32_t val32;

    if (!xms->low_mmio_aliases_enabled) {
        return;
    }

    switch (size) {
    case 1:
        buf[0] = data;
        break;
    case 2:
        stw_le_p(buf, data);
        break;
    case 4:
        stl_le_p(buf, data);
        break;
    case 8:
        stq_le_p(buf, data);
        break;
    default:
        return;
    }
    val32 = ldl_le_p(buf);

    switch (reg) {
    case XENON_SFCX_REG_CONFIG:
    {
        const uint32_t strap_mask =
            (0x3U << 4) | (0x3U << 17) | (0x3U << 19) | (0xFU << 21);
        uint32_t base = (xms->console_revision == XENON_CONSOLE_XENON) ?
                        0x01198030U : 0x00043000U;
        uint32_t cur = (val32 & ~strap_mask) | (base & strap_mask);
        xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_CONFIG, cur);
        break;
    }
    case XENON_SFCX_REG_STATUS:
        /* Write-then-read clears to WP/BY asserted. */
        xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
        break;
    case XENON_SFCX_REG_COMMAND: {
        uint32_t addr = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_ADDRESS);
        uint32_t data_reg = xenon_sfcx_pci_get32(xms, XENON_SFCX_REG_DATA);

        switch (val32 & 0xFFU) {
        case XENON_SFCX_CMD_PAGE_BUF_TO_REG:
            if (addr + 4 <= sizeof(xms->sfcx_page_buf)) {
                data_reg = ldl_be_p(xms->sfcx_page_buf + addr);
            } else {
                data_reg = 0xFFFFFFFFU;
            }
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_DATA, data_reg);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_ADDRESS, addr + 4);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            break;
        case XENON_SFCX_CMD_REG_TO_PAGE_BUF:
            if (addr + 4 <= sizeof(xms->sfcx_page_buf)) {
                stl_be_p(xms->sfcx_page_buf + addr, data_reg);
            }
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_ADDRESS, addr + 4);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            break;
        case XENON_SFCX_CMD_LOG_PAGE_TO_BUF:
            xenon_sfcx_load_pagebuf(xms, addr, false);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
            break;
        case XENON_SFCX_CMD_PHY_PAGE_TO_BUF:
            xenon_sfcx_load_pagebuf(xms, addr, true);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
            break;
        case XENON_SFCX_CMD_DMA_LOG_TO_RAM:
            xenon_sfcx_dma_from_nand(xms, false);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
            break;
        case XENON_SFCX_CMD_DMA_PHY_TO_RAM:
            xenon_sfcx_dma_from_nand(xms, true);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, XENON_SFCX_CMD_NO_CMD);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
            break;
        default:
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_COMMAND, val32);
            xenon_sfcx_pci_put32(xms, XENON_SFCX_REG_STATUS, 0x00000600U);
            break;
        }
        break;
    }
    case XENON_SFCX_REG_ADDRESS:
    case XENON_SFCX_REG_DATA:
    case XENON_SFCX_REG_LOGICAL:
    case XENON_SFCX_REG_PHYSICAL:
    case XENON_SFCX_REG_DATAPHYS:
    case XENON_SFCX_REG_SPAREPHYS:
        xenon_sfcx_pci_put32(xms, reg, val32);
        break;
    default:
        break;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_SATA) && reg <= XENON_SFCX_REG_PHISON &&
        xms->sfcx_trace_writes < 128) {
        SATA_INFO("sfcx-write off=0x%03" PRIx32
                  " size=%u val=0x%016" PRIx64,
                  reg, size, data);
        xms->sfcx_trace_writes++;
    }
}

const MemoryRegionOps xenon_sfcx_ops = {
    .read = xenon_sfcx_read,
    .write = xenon_sfcx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
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

/*
 * PCI config MMIO read handler for Xenon low-MMIO alias window.
 *
 * Purpose: expose the PCI config shadow as a MMIO range during early stages
 * (used for XGPU/SFCX config interactions).
 */
static uint64_t xenon_pci_cfg_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t v = 0;

    if (!xms->low_mmio_aliases_enabled) {
        return 0;
    }

    if (offset >= XENON_PCI_CFG_SIZE) {
        memset(buf, 0xFF, size);
    } else {
        hwaddr avail = XENON_PCI_CFG_SIZE - offset;
        if (avail < size) {
            memset(buf, 0xFF, size);
            memcpy(buf, xms->pci_cfg_data + offset, avail);
        } else {
            memcpy(buf, xms->pci_cfg_data + offset, size);
        }
    }

    switch (size) {
    case 1:
        v = buf[0];
        break;
    case 2:
        v = lduw_be_p(buf);
        break;
    case 4:
        v = ldl_be_p(buf);
        break;
    case 8:
        v = ldq_be_p(buf);
        break;
    default:
        break;
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_XGPU) &&
        offset >= 0x10000 && offset < 0x10100 &&
        xms->xgpu_trace_reads < 32) {
        XGPU_INFO("xgpu-cfg-read off=0x%05" PRIx64
                  " size=%u val=0x%016" PRIx64,
                  (uint64_t)offset, size, v);
        xms->xgpu_trace_reads++;
    }

    return v;
}

/*
 * PCI config MMIO write handler for Xenon low-MMIO alias window.
 *
 * Purpose: accept guest configuration writes and preserve strap-derived fields
 * that behave read-only on real hardware.
 */
static void xenon_pci_cfg_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8];

    if (!xms->low_mmio_aliases_enabled) {
        return;
    }

    if (offset >= XENON_PCI_CFG_SIZE) {
        return;
    }

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

    {
        hwaddr avail = XENON_PCI_CFG_SIZE - offset;
        if (avail < size) {
            memcpy(xms->pci_cfg_data + offset, buf, avail);
        } else {
            memcpy(xms->pci_cfg_data + offset, buf, size);
        }
    }

    if (offset == 0x8000 && size == 4) {
        const uint32_t strap_mask =
            (0x3U << 4) | (0x3U << 17) | (0x3U << 19) | (0xFU << 21);
        uint32_t cur = ldl_le_p(xms->pci_cfg_data + 0x8000);
        uint32_t base = (xms->console_revision == XENON_CONSOLE_XENON) ?
                        0x01198030U : 0x00043000U;
        cur = (cur & ~strap_mask) | (base & strap_mask);
        stl_le_p(xms->pci_cfg_data + 0x8000, cur);
    }

    if (offset == 0x8004 && size == 2) {
        stl_le_p(xms->pci_cfg_data + 0x8004, 0x00000600U);
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_XGPU) &&
        offset >= 0x10000 && offset < 0x10100 &&
        xms->xgpu_trace_writes < 32) {
        XGPU_INFO("xgpu-cfg-write off=0x%05" PRIx64
                  " size=%u val=0x%016" PRIx64,
                  (uint64_t)offset, size, data);
        xms->xgpu_trace_writes++;
    }
}

const MemoryRegionOps xenon_pci_cfg_ops = {
    .read = xenon_pci_cfg_read,
    .write = xenon_pci_cfg_write,
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

/*
 * XGPU BAR0 MMIO read handler.
 *
 * Purpose: route aligned 32-bit accesses through the XGPU register model while
 * providing a byte-backed shadow for other widths.
 */
static uint64_t xenon_xgpu_bar0_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t v = 0;

    if (!xms->low_mmio_aliases_enabled) {
        return 0;
    }

    if (offset >= XENON_XGPU_MMIO_SIZE) {
        return 0;
    }

    if (size == 4 && (offset & 3) == 0) {
        return xenon_xgpu_mmio_read32(xms, offset);
    }

    {
        hwaddr avail = XENON_XGPU_MMIO_SIZE - offset;
        if (avail < size) {
            memcpy(buf, xms->xgpu_mmio_data + offset, avail);
        } else {
            memcpy(buf, xms->xgpu_mmio_data + offset, size);
        }
    }

    switch (size) {
    case 1:
        v = buf[0];
        break;
    case 2:
        v = lduw_be_p(buf);
        break;
    case 4:
        v = ldl_be_p(buf);
        break;
    case 8:
        v = ldq_be_p(buf);
        break;
    default:
        break;
    }

    return v;
}

/*
 * XGPU BAR0 MMIO write handler.
 *
 * Purpose: route aligned 32-bit writes into the XGPU model; store other writes
 * into the shadow buffer for deterministic readback during bring-up.
 */
static void xenon_xgpu_bar0_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8];

    if (!xms->low_mmio_aliases_enabled) {
        return;
    }

    if (offset >= XENON_XGPU_MMIO_SIZE) {
        return;
    }

    if (size == 4 && (offset & 3) == 0) {
        xenon_xgpu_mmio_write32(xms, offset, (uint32_t)data);
        return;
    }

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

    {
        hwaddr avail = XENON_XGPU_MMIO_SIZE - offset;
        if (avail < size) {
            memcpy(xms->xgpu_mmio_data + offset, buf, avail);
        } else {
            memcpy(xms->xgpu_mmio_data + offset, buf, size);
        }
    }
}

const MemoryRegionOps xenon_xgpu_bar0_ops = {
    .read = xenon_xgpu_bar0_read,
    .write = xenon_xgpu_bar0_write,
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
