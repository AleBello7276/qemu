/*
 * QEMU Xbox 360 (Xenon) machine - MMIO handlers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/xenon-internal.h"

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

    if (xms->trace_boot && xms->nand_trace_reads < 32) {
        info_report("xbox360: nand-mmio-read off=0x%08" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    (uint64_t)offset, size, v);
        xms->nand_trace_reads++;
    }
    return v;
}

static void xenon_nand_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint8_t buf[8];

    if (xms->trace_boot && xms->nand_trace_writes < 16) {
        info_report("xbox360: nand-mmio-write off=0x%08" PRIx64
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

    if (xms->trace_boot &&
        (offset == 0x15e0 || offset == 0x15e4 ||
         offset == 0x15e8 || offset == 0x15ec) &&
        xms->soc_trace_reads < 256) {
        info_report("xbox360: nb-mmio-read off=0x%05" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    (uint64_t)offset, size, v);
        xms->soc_trace_reads++;
    }
    return v;
}

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

    if (xms->trace_boot &&
        (offset == 0x15e0 || offset == 0x15e4 ||
         offset == 0x15e8 || offset == 0x15ec) &&
        xms->soc_trace_writes < 256) {
        info_report("xbox360: nb-mmio-write off=0x%05" PRIx64
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

static uint64_t xenon_smc_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    uint64_t v = xenon_smc_read(&xms->smc_state, offset, size);
    bool should_log = false;

    if (xms->trace_boot && offset <= 0x94 &&
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
            info_report("xbox360: smc-read off=0x%03" PRIx64
                        " size=%u val=0x%016" PRIx64,
                        (uint64_t)offset, size, v);
            xms->smc_trace_reads++;
        }
        xms->smc_last_status_valid = true;
    }
    return v;
}

static void xenon_smc_mmio_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;

    if (offset == 0x14) {
        xenon_smc_write(&xms->smc_state, offset, data, size);
        return;
    }

    if (xms->trace_boot && offset <= 0x94 &&
        xms->smc_trace_writes < XENON_SMC_TRACE_LIMIT) {
        info_report("xbox360: smc-write off=0x%03" PRIx64
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
