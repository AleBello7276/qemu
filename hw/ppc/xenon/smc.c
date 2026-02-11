/*
 * Xbox 360 Xenon SMC minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/smc.h"

#define UART_BYTE_OUT_REG   0x10
#define UART_BYTE_IN_REG    0x14
#define UART_STATUS_REG     0x18
#define UART_CONFIG_REG     0x1c
#define SMI_INT_STATUS_REG  0x50
#define SMI_INT_ACK_REG     0x58
#define SMI_INT_ENABLED_REG 0x5c
#define CLCK_INT_ENABLED_REG 0x64
#define CLCK_INT_STATUS_REG 0x6c
#define SMI_INT_STATUS_REG_ALIAS 0x20
#define SMI_INT_ACK_REG_ALIAS    0x24
#define SMI_INT_ENABLED_REG_ALIAS 0x28
#define CLCK_INT_ENABLED_REG_ALIAS 0x2c
#define CLCK_INT_STATUS_REG_ALIAS 0x34
#define FIFO_IN_DATA_REG    0x80
#define FIFO_IN_STATUS_REG  0x84
#define FIFO_OUT_DATA_REG   0x90
#define FIFO_OUT_STATUS_REG 0x94

#define FIFO_STATUS_BUSY    0x0
#define FIFO_STATUS_READY   0x4
#define UART_STATUS_DATA_PRES 0x1
#define UART_STATUS_EMPTY     0x2
#define SMI_INT_ENABLED_MASK 0x0c
#define SMI_INT_PENDING       0x10000000U
#define CLCK_INT_ENABLED      0x10000000U
#define CLCK_INT_READY        0x1
#define CLCK_INT_TAKEN        0x3

#define SMC_CMD_PWRON_TYPE  0x01
#define SMC_CMD_QUERY_RTC   0x04
#define SMC_CMD_QUERY_TEMP  0x07
#define SMC_CMD_QUERY_TRAY  0x0a
#define SMC_CMD_QUERY_AVPACK 0x0f
#define SMC_CMD_I2C_RW      0x11
#define SMC_CMD_QUERY_VERSION 0x12
#define SMC_CMD_SET_POWER_LED 0x8c
#define SMC_CMD_SET_FP_LEDS 0x99

#define SMC_TRAY_CLOSED     0x62

static void xenon_smc_copy_le_out(uint8_t *dst, uint64_t data, unsigned size)
{
    switch (size) {
    case 1:
        dst[0] = (uint8_t)data;
        break;
    case 2:
        stw_le_p(dst, (uint16_t)data);
        break;
    case 4:
        stl_le_p(dst, (uint32_t)data);
        break;
    case 8:
        stq_le_p(dst, data);
        break;
    default:
        break;
    }
}

static uint64_t xenon_smc_copy_le_in(const uint8_t *src, unsigned size)
{
    switch (size) {
    case 1:
        return src[0];
    case 2:
        return lduw_le_p(src);
    case 4:
        return ldl_le_p(src);
    case 8:
        return ldq_le_p(src);
    default:
        return 0;
    }
}

static void xenon_smc_process_fifo(XenonSmcState *smc)
{
    uint8_t req[sizeof(smc->fifo)];
    uint8_t cmd = smc->fifo_cmd ? smc->fifo_cmd : smc->fifo[0];

    memcpy(req, smc->fifo, sizeof(req));
    if (cmd == 0 && req[3] != 0) {
        cmd = smc->fifo[3];
    }

    memset(smc->fifo, 0, sizeof(smc->fifo));
    smc->fifo[0] = cmd;
    smc->fifo_cmd = cmd;

    switch (cmd) {
    case SMC_CMD_PWRON_TYPE:
        smc->fifo[1] = smc->power_on_reason;
        break;
    case SMC_CMD_QUERY_RTC:
        smc->fifo[1] = 0x00;
        break;
    case SMC_CMD_QUERY_TEMP:
        /*
         * Coarse defaults used by xenon-emu stubs.
         */
        smc->fifo[1] = 0x24;
        smc->fifo[2] = 0x1B;
        smc->fifo[3] = 0x2F;
        smc->fifo[4] = 0xA4;
        smc->fifo[5] = 0x2C;
        smc->fifo[6] = 0x24;
        smc->fifo[7] = 0x26;
        break;
    case SMC_CMD_QUERY_TRAY:
        smc->fifo[1] = SMC_TRAY_CLOSED;
        break;
    case SMC_CMD_QUERY_AVPACK:
        smc->fifo[1] = smc->avpack_type;
        break;
    case SMC_CMD_I2C_RW: {
        uint8_t sub = req[1];

        smc->fifo[0] = SMC_CMD_I2C_RW;
        switch (sub) {
        case 0x03: /* DDC lock */
        case 0x05: /* DDC unlock */
        case 0x20: /* I2C write */
        case 0x21: /* DDC write */
        case 0x60: /* SMBUS write */
            smc->fifo[1] = 0x00;
            break;
        case 0x11: /* DDC read */
            smc->fifo[1] = 0x00;
            smc->fifo[3] = 0x00;
            smc->fifo[4] = 0x00;
            smc->fifo[5] = 0x00;
            smc->fifo[6] = 0x00;
            break;
        case 0x10: { /* SMBUS/I2C read */
            uint16_t addr = req[6] + (req[3] == 0x8D ? 0x200 : 0x100);

            smc->fifo[1] = 0x00;
            if (addr == 0x102) {
                smc->fifo[3] = 0x53;
                smc->fifo[4] = 0x92;
                smc->fifo[5] = 0x00;
                smc->fifo[6] = 0x00;
            } else {
                smc->fifo[3] = 0x00;
                smc->fifo[4] = 0x00;
                smc->fifo[5] = 0x00;
                smc->fifo[6] = 0x00;
            }
            break;
        }
        default:
            smc->fifo[1] = 0x01;
            break;
        }
        break;
    }
    case SMC_CMD_QUERY_VERSION:
        smc->fifo[1] = 0x41;
        smc->fifo[2] = 0x02;
        smc->fifo[3] = 0x03;
        break;
    case SMC_CMD_SET_POWER_LED:
    case SMC_CMD_SET_FP_LEDS:
        /* Ack-only stubs: enough for bring-up code paths. */
        break;
    default:
        break;
    }

    smc->fifo_out_status = FIFO_STATUS_READY;
    smc->fifo_read_pos = 0;
    if (smc->smi_int_enabled & SMI_INT_ENABLED_MASK) {
        smc->smi_int_pending = SMI_INT_PENDING;
        smc->smi_int_ack = 0;
    }

    if (smc->trace_boot) {
        info_report("xbox360: smc fifo cmd=0x%02x -> rsp[0..3]=%02x %02x %02x %02x",
                    cmd, smc->fifo[0], smc->fifo[1], smc->fifo[2], smc->fifo[3]);
    }
}

void xenon_smc_reset(XenonSmcState *smc, uint8_t power_on_reason,
                     uint8_t avpack_type, const char *uart_backend, bool trace_boot)
{
    memset(smc, 0, sizeof(*smc));
    smc->power_on_reason = power_on_reason;
    smc->avpack_type = avpack_type;
    smc->trace_boot = trace_boot;
    smc->uart_stdio = false;
    if (!uart_backend || !g_ascii_strcasecmp(uart_backend, "null")) {
        smc->uart_stdio = false;
    } else if (!g_ascii_strcasecmp(uart_backend, "stdio") ||
               !g_ascii_strcasecmp(uart_backend, "print")) {
        smc->uart_stdio = true;
    } else if (!g_ascii_strcasecmp(uart_backend, "socket") ||
               !g_ascii_strcasecmp(uart_backend, "vcom")) {
        warn_report("xbox360: smc-uart='%s' not implemented, falling back to null",
                    uart_backend);
        smc->uart_stdio = false;
    } else {
        warn_report("xbox360: unknown smc-uart='%s', falling back to null",
                    uart_backend);
        smc->uart_stdio = false;
    }
    smc->fifo_in_status = FIFO_STATUS_READY;
    smc->fifo_out_status = FIFO_STATUS_READY;
    smc->uart_status = UART_STATUS_EMPTY;
    smc->uart_status_flip = false;
    smc->smi_int_pending = 0;
    smc->smi_int_ack = 0;
    smc->smi_int_enabled = 0;
    smc->clock_int_enabled = 0;
    smc->clock_int_status = CLCK_INT_READY;
}

uint64_t xenon_smc_read(XenonSmcState *smc, uint64_t offset, unsigned size)
{
    uint8_t tmp[8] = { 0 };
    uint32_t reg = offset & 0xff;

    switch (reg) {
    case UART_BYTE_OUT_REG:
        /* RX not modeled yet */
        xenon_smc_copy_le_out(tmp, 0, size);
        break;
    case UART_STATUS_REG:
        /*
         * Hardware exposes UART status bits on mirrored byte lanes for wider
         * accesses. Keep that behavior so both low-bit and high-halfword poll
         * masks used by CB/CD observe TX-empty consistently.
         */
        {
            uint32_t lane = smc->uart_status & 0xFFU;

            /*
             * CD has two distinct status poll patterns: one waits for the
             * TX-empty bit, the other waits for transient bits to quiesce.
             * Alternate EMPTY/0 between consecutive reads to model that
             * transition without hardcoding PC-specific behavior.
             */
            if (lane == UART_STATUS_EMPTY) {
                if (smc->uart_status_flip) {
                    lane = 0;
                }
                smc->uart_status_flip = !smc->uart_status_flip;
            }
        if (size == 4) {
            xenon_smc_copy_le_out(tmp, lane * 0x01010101U, size);
        } else if (size == 2) {
            xenon_smc_copy_le_out(tmp, lane * 0x0101U, size);
        } else {
            xenon_smc_copy_le_out(tmp, lane, size);
        }
        }
        break;
    case UART_CONFIG_REG:
        xenon_smc_copy_le_out(tmp, smc->uart_config, size);
        break;
    case SMI_INT_STATUS_REG:
    case SMI_INT_STATUS_REG_ALIAS:
        xenon_smc_copy_le_out(tmp, smc->smi_int_pending, size);
        break;
    case SMI_INT_ACK_REG:
    case SMI_INT_ACK_REG_ALIAS:
        xenon_smc_copy_le_out(tmp, smc->smi_int_ack, size);
        break;
    case SMI_INT_ENABLED_REG:
    case SMI_INT_ENABLED_REG_ALIAS:
        xenon_smc_copy_le_out(tmp, smc->smi_int_enabled, size);
        break;
    case CLCK_INT_ENABLED_REG:
    case CLCK_INT_ENABLED_REG_ALIAS:
        xenon_smc_copy_le_out(tmp, smc->clock_int_enabled, size);
        break;
    case CLCK_INT_STATUS_REG:
    case CLCK_INT_STATUS_REG_ALIAS:
        if (smc->clock_int_enabled == CLCK_INT_ENABLED &&
            smc->clock_int_status == CLCK_INT_READY) {
            smc->clock_int_status = CLCK_INT_TAKEN;
        }
        xenon_smc_copy_le_out(tmp, smc->clock_int_status, size);
        break;
    case FIFO_IN_STATUS_REG:
        xenon_smc_copy_le_out(tmp, smc->fifo_in_status, size);
        break;
    case FIFO_OUT_STATUS_REG:
        xenon_smc_copy_le_out(tmp, smc->fifo_out_status, size);
        break;
    case FIFO_OUT_DATA_REG:
        if (smc->fifo_read_pos + size > sizeof(smc->fifo)) {
            size = sizeof(smc->fifo) - smc->fifo_read_pos;
        }
        memcpy(tmp, &smc->fifo[smc->fifo_read_pos], size);
        smc->fifo_read_pos += size;
        break;
    default:
        xenon_smc_copy_le_out(tmp, 0, size);
        break;
    }

    return xenon_smc_copy_le_in(tmp, size);
}

void xenon_smc_write(XenonSmcState *smc, uint64_t offset, uint64_t data, unsigned size)
{
    uint8_t tmp[8] = { 0 };
    uint32_t reg = offset & 0xff;

    xenon_smc_copy_le_out(tmp, data, size);

    switch (reg) {
    case UART_BYTE_IN_REG: {
        uint8_t ch = 0;

        /*
         * Xenon software can place the UART byte on different lanes for
         * 16/32-bit stores depending on bridge/endian path. Accept either.
         */
        for (unsigned i = 0; i < size; i++) {
            if (tmp[i] != 0) {
                ch = tmp[i];
                break;
            }
        }

        if (smc->uart_stdio && ch) {
            fputc(ch, stderr);
            fflush(stderr);
        }
        smc->uart_status_flip = false;
        break;
    }
    case UART_CONFIG_REG:
        smc->uart_config = (uint32_t)data;
        break;
    case SMI_INT_STATUS_REG:
    case SMI_INT_STATUS_REG_ALIAS:
        smc->smi_int_pending = (uint32_t)data;
        break;
    case SMI_INT_ACK_REG:
    case SMI_INT_ACK_REG_ALIAS:
        smc->smi_int_ack = (uint32_t)data;
        if (smc->smi_int_ack) {
            smc->smi_int_pending = 0;
        }
        break;
    case SMI_INT_ENABLED_REG:
    case SMI_INT_ENABLED_REG_ALIAS:
        smc->smi_int_enabled = (uint32_t)data;
        break;
    case CLCK_INT_ENABLED_REG:
    case CLCK_INT_ENABLED_REG_ALIAS:
        smc->clock_int_enabled = (uint32_t)data;
        break;
    case CLCK_INT_STATUS_REG:
    case CLCK_INT_STATUS_REG_ALIAS:
        smc->clock_int_status = (uint32_t)data;
        break;
    case FIFO_IN_DATA_REG:
        if (smc->fifo_pos + size > sizeof(smc->fifo)) {
            size = sizeof(smc->fifo) - smc->fifo_pos;
        }
        memcpy(&smc->fifo[smc->fifo_pos], tmp, size);
        smc->fifo_pos += size;
        break;
    case FIFO_IN_STATUS_REG: {
        uint32_t v = (uint32_t)data;
        smc->fifo_in_status = v;
        if (v == FIFO_STATUS_READY) {
            memset(smc->fifo, 0, sizeof(smc->fifo));
            smc->fifo_pos = 0;
            smc->fifo_cmd = 0;
        } else if (v == FIFO_STATUS_BUSY) {
            /*
             * Match xenon-emu FIFO handshake:
             * - command commit transitions OUT to busy first
             * - IN returns to ready while SMC processes
             * - OUT becomes ready when response is available
             */
            smc->fifo_cmd = smc->fifo[0] ? smc->fifo[0] : smc->fifo[3];
            smc->fifo_out_status = FIFO_STATUS_BUSY;
            xenon_smc_process_fifo(smc);
            smc->fifo_in_status = FIFO_STATUS_READY;
        }
        break;
    }
    case FIFO_OUT_STATUS_REG:
        smc->fifo_out_status = (uint32_t)data;
        if (smc->fifo_out_status == FIFO_STATUS_READY) {
            smc->fifo_read_pos = 0;
        }
        break;
    default:
        break;
    }
}
