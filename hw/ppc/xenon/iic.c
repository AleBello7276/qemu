/*
 * Xbox 360 Xenon machine IIC helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/ppc/ppc.h"
#include "hw/ppc/xenon/iic.h"

#define XENON_IIC_THREAD_BLOCK_SIZE   0x1000U
#define XENON_IIC_THREAD_BLOCKS_END   (XENON_MAX_CPUS * XENON_IIC_THREAD_BLOCK_SIZE)

static uint64_t xenon_iic_load_be(const uint8_t *buf, unsigned size)
{
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

static void xenon_iic_store_be(uint8_t *buf, uint64_t v, unsigned size)
{
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
}

static uint64_t xenon_iic_read_raw(XenonMachineState *xms, hwaddr off, unsigned size)
{
    uint8_t tmp[8] = { 0 };
    hwaddr avail;

    if (!xms->iic_mmio_data || off >= XENON_IIC_MMIO_SIZE) {
        return 0;
    }

    avail = XENON_IIC_MMIO_SIZE - off;
    if (avail < size) {
        memcpy(tmp, xms->iic_mmio_data + off, avail);
    } else {
        memcpy(tmp, xms->iic_mmio_data + off, size);
    }

    return xenon_iic_load_be(tmp, size);
}

static void xenon_iic_write_raw(XenonMachineState *xms, hwaddr off, uint64_t data,
                                unsigned size)
{
    uint8_t tmp[8] = { 0 };
    hwaddr avail;

    if (!xms->iic_mmio_data || off >= XENON_IIC_MMIO_SIZE) {
        return;
    }

    xenon_iic_store_be(tmp, data, size);
    avail = XENON_IIC_MMIO_SIZE - off;
    if (avail < size) {
        memcpy(xms->iic_mmio_data + off, tmp, avail);
    } else {
        memcpy(xms->iic_mmio_data + off, tmp, size);
    }
}

static uint8_t xenon_iic_thread_logical_id(XenonMachineState *xms, unsigned thread_id)
{
    hwaddr off = ((hwaddr)thread_id * XENON_IIC_THREAD_BLOCK_SIZE);
    uint8_t logical_id = (uint8_t)(xenon_iic_read_raw(xms, off, 8) & 0x3f);

    if (!logical_id && thread_id < 8) {
        logical_id = (uint8_t)(1u << thread_id);
    }
    return logical_id;
}

static void xenon_iic_wake_thread_one(XenonMachineState *xms, unsigned thread_id,
                                      const char *source)
{
    PowerPCCPU *cpu;
    CPUState *cs;

    if (thread_id >= xms->cpu_count) {
        return;
    }
    cpu = xms->cpus[thread_id];
    if (!cpu) {
        return;
    }
    cs = CPU(cpu);

    /* Keep the boot thread uninterrupted unless it is explicitly halted. */
    if (thread_id == 0 && !cs->halted) {
        return;
    }

    if (cs->halted) {
        cs->halted = false;
    }

    ppc_set_irq(cpu, PPC_INTERRUPT_RESET, 1);
    ppc_set_irq(cpu, PPC_INTERRUPT_RESET, 0);
    qemu_cpu_kick(cs);
    xms->cpu_online_mask |= (uint8_t)(1u << thread_id);

    if (xms->trace_boot) {
        info_report("xbox360: iic wake thread=%u source=%s online-mask=0x%02x",
                    thread_id, source, xms->cpu_online_mask);
    }
}

static void xenon_iic_wake_thread(XenonMachineState *xms, unsigned thread_id,
                                  const char *source)
{
    xenon_iic_wake_thread_one(xms, thread_id, source);

    /*
     * Xenon IPIs are core-oriented during CD bring-up: when thread0 of a
     * core is woken, thread1 is quickly enabled via CTRL and takes a reset.
     * Mirror that behavior so software observing "CPUs online" sees both
     * hardware threads per awakened core.
     */
    if ((thread_id % 2) == 0 && (thread_id + 1) < xms->cpu_count) {
        xenon_iic_wake_thread_one(xms, thread_id + 1, "core-sibling");
    }
}

static void xenon_iic_generate_interrupt(XenonMachineState *xms,
                                         uint8_t interrupt_type,
                                         uint8_t cpus_mask)
{
    /*
     * Xenon software uses IPI writes to wake sleeping hardware threads.
     * Route targeted masks to halted vCPUs and deliver reset, mirroring
     * the xenon-emu bring-up flow where sleeping PPUs come online through
     * system reset handling.
     */
    for (unsigned i = 0; i < xms->cpu_count && i < XENON_MAX_CPUS; i++) {
        uint8_t logical_id = xenon_iic_thread_logical_id(xms, i);

        if (logical_id && (cpus_mask & logical_id)) {
            xenon_iic_wake_thread(xms, i, "ipi");
        }
    }

    /*
     * Boot thread1 is typically enabled during the same wakeup phase.
     * If IPI1 is emitted and thread1 is still halted, bring it online.
     */
    if (interrupt_type == 0x78 && xms->cpu_count > 1) {
        xenon_iic_wake_thread_one(xms, 1, "ipi-boot-sibling");
    }

    if (xms->trace_boot) {
        info_report("xbox360: iic ipi vector=0x%02x mask=0x%02x",
                    interrupt_type, cpus_mask);
    }
}

void xenon_iic_reset(XenonMachineState *xms)
{
    if (!xms->iic_mmio_data) {
        return;
    }

    memset(xms->iic_mmio_data, 0, XENON_IIC_MMIO_SIZE);
    for (unsigned i = 0; i < XENON_MAX_CPUS; i++) {
        hwaddr off = ((hwaddr)i * XENON_IIC_THREAD_BLOCK_SIZE);

        xenon_iic_write_raw(xms, off + 0x0000, (uint64_t)(1u << i), 8);
    }
    xms->iic_migr2_flip = false;
}

uint64_t xenon_iic_read(XenonMachineState *xms, hwaddr off, unsigned size)
{
    uint64_t v;

    if (size < 1 || size > 8) {
        return 0;
    }

    v = xenon_iic_read_raw(xms, off, size);

    if (off == 0x6020 && size <= 8) {
        if (xms->iic_migr2_flip) {
            v |= 0x200ULL;
            xms->iic_migr2_flip = false;
        } else {
            v &= ~0x200ULL;
            xms->iic_migr2_flip = true;
        }
        xenon_iic_write_raw(xms, off, v, size);
    }

    return v;
}

void xenon_iic_write(XenonMachineState *xms, hwaddr off, uint64_t data, unsigned size)
{
    if (size < 1 || size > 8) {
        return;
    }

    xenon_iic_write_raw(xms, off, data, size);

    if (off < XENON_IIC_THREAD_BLOCKS_END) {
        unsigned thread_id = off / XENON_IIC_THREAD_BLOCK_SIZE;
        unsigned block_off = off % XENON_IIC_THREAD_BLOCK_SIZE;

        switch (block_off) {
        case 0x0010: {
            uint8_t interrupt_type = (uint8_t)(data & 0xff);
            uint8_t cpus_mask = (uint8_t)((data >> 16) & 0xff);

            xenon_iic_generate_interrupt(xms, interrupt_type, cpus_mask);
            break;
        }
        case 0x00F0:
            if (data) {
                xenon_iic_wake_thread(xms, thread_id, "thread-reset");
            }
            break;
        default:
            break;
        }
    }
}
