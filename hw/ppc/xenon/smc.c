/*
 * Xbox 360 Xenon SMC minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "hw/ppc/xenon/hana.h"
#include "hw/ppc/xenon/smc.h"

#define UART_BYTE_OUT_REG   0x10
#define UART_BYTE_IN_REG    0x14
#define UART_STATUS_REG     0x18
#define UART_CONFIG_REG     0x1c
#define GPIO_REG0           0x20
#define GPIO_REG1           0x24
#define GPIO_REG2           0x28
#define GPIO_REG3           0x30
#define GPIO_REG4           0x34
#define GPIO_REG5           0x38
#define GPIO_REG6           0x40
#define GPIO_REG7           0x44
#define GPIO_REG8           0x48
#define SMI_INT_STATUS_REG  0x50
#define SMI_INT_ACK_REG     0x58
#define SMI_INT_ENABLED_REG 0x5c
#define CLCK_INT_ENABLED_REG 0x64
#define CLCK_INT_STATUS_REG 0x6c
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
#define SMC_DDC_REG_ADDR_HI 0xEE
#define SMC_DDC_REG_ADDR_LO 0xEF
#define SMC_DDC_REG_STATUS  0xF2
#define SMC_DDC_REG_CMD     0xF3
#define SMC_DDC_REG_DATA    0xF4
#define SMC_DDC_STATUS_BUSY 0x10

/*
 * Store a little-endian integer into a byte buffer of size 1/2/4/8.
 *
 * Purpose: SMC registers are modeled as little-endian and accessed at varying
 * widths; centralize packing for consistent behavior.
 */
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

/*
 * Load a little-endian integer from a byte buffer of size 1/2/4/8.
 *
 * Purpose: companion to xenon_smc_copy_le_out() for register reads.
 */
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

/*
 * Initialize a minimal, valid EDID image for the SMC DDC emulation.
 *
 * Purpose: XeLL/libxenon display init expects DDC/EDID to respond with a valid
 * base block; this provides a stable stub.
 */
static void xenon_smc_init_ddc_edid(XenonSmcState *smc)
{
    uint8_t sum = 0;

    memset(smc->ddc_edid, 0, sizeof(smc->ddc_edid));
    smc->ddc_edid[0] = 0x00;
    smc->ddc_edid[1] = 0xFF;
    smc->ddc_edid[2] = 0xFF;
    smc->ddc_edid[3] = 0xFF;
    smc->ddc_edid[4] = 0xFF;
    smc->ddc_edid[5] = 0xFF;
    smc->ddc_edid[6] = 0xFF;
    smc->ddc_edid[7] = 0x00;

    /* Minimal but valid EDID base block. */
    smc->ddc_edid[8] = 0x12;
    smc->ddc_edid[9] = 0x34;
    smc->ddc_edid[10] = 0x56;
    smc->ddc_edid[11] = 0x78;
    smc->ddc_edid[16] = 0x01;
    smc->ddc_edid[17] = 0x1E;
    smc->ddc_edid[18] = 0x01;
    smc->ddc_edid[19] = 0x04;
    smc->ddc_edid[20] = 0x80; /* digital input */
    smc->ddc_edid[21] = 0x34;
    smc->ddc_edid[22] = 0x1D;
    smc->ddc_edid[23] = 0x78;
    smc->ddc_edid[24] = 0x0A;
    smc->ddc_edid[126] = 0x00; /* no extension blocks */

    for (unsigned i = 0; i < 127; i++) {
        sum += smc->ddc_edid[i];
    }
    smc->ddc_edid[127] = (uint8_t)(0 - sum);
}

/*
 * Read a byte from the SMC DDC register file.
 *
 * Purpose: keep DDC access logic local so the FIFO command handler can remain
 * simple.
 */
static uint8_t xenon_smc_ddc_read_reg(XenonSmcState *smc, uint8_t reg)
{
    return smc->ddc_regs[reg];
}

/*
 * Write a byte to the SMC DDC register file and execute simple commands.
 *
 * Purpose: support the DDC "read EDID byte" command pattern used by early
 * bring-up code.
 */
static void xenon_smc_ddc_write_reg(XenonSmcState *smc, uint8_t reg, uint8_t value)
{
    smc->ddc_regs[reg] = value;

    if (reg == SMC_DDC_REG_CMD && value == 0x04) {
        uint16_t off = ((uint16_t)smc->ddc_regs[SMC_DDC_REG_ADDR_HI] << 8) |
                       smc->ddc_regs[SMC_DDC_REG_ADDR_LO];
        uint8_t v = (off < sizeof(smc->ddc_edid)) ? smc->ddc_edid[off] : 0x00;

        smc->ddc_regs[SMC_DDC_REG_STATUS] |= SMC_DDC_STATUS_BUSY;
        smc->ddc_regs[SMC_DDC_REG_DATA] = v;
        smc->ddc_regs[SMC_DDC_REG_STATUS] &= ~SMC_DDC_STATUS_BUSY;
    }
}

/*
 * Process a completed FIFO request into a FIFO response.
 *
 * Purpose: implement the subset of SMC commands that secure boot / XeLL use
 * (power-on reason, AV pack, I2C/DDC, version, etc.).
 */
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
            smc->fifo[1] = 0x00;
            break;
        case 0x21: /* DDC write */
            xenon_smc_ddc_write_reg(smc, req[6], req[7]);
            smc->fifo[1] = 0x00;
            break;
        case 0x60: { /* SMBUS write */
            uint32_t v = req[8] |
                         ((uint32_t)req[9] << 8) |
                         ((uint32_t)req[10] << 16) |
                         ((uint32_t)req[11] << 24);

            smc->fifo[1] = 0x00;
            if (req[3] == 0xF0) {
                xenon_hana_write(smc->hana_regs, req[6], v);
            }
            break;
        }
        case 0x11: /* DDC read */
            smc->fifo[1] = 0x00;
            smc->fifo[3] = xenon_smc_ddc_read_reg(smc, req[6]);
            smc->fifo[4] = 0x00;
            smc->fifo[5] = 0x00;
            smc->fifo[6] = 0x00;
            break;
        case 0x10: { /* SMBUS/I2C read */
            smc->fifo[1] = 0x00;
            if (req[5] == 0xF0) {
                uint32_t v = xenon_hana_read(smc->hana_regs, req[6]);
                uint8_t idx = (uint8_t)((v >> 16) & 0xFF);
                uint8_t hi =
                    (uint8_t)((xenon_hana_read(smc->hana_regs, idx) >> 24) & 0xFF);

                smc->fifo[4] = (uint8_t)(v & 0xFF);
                smc->fifo[5] = (uint8_t)((v >> 8) & 0xFF);
                smc->fifo[6] = idx;
                /*
                 * xenon-emu compatibility quirk: top byte is sourced through
                 * a second table lookup indexed by byte2 of the first read.
                 */
                smc->fifo[7] = hi;
            } else {
                uint16_t addr = req[6] + (req[3] == 0x8D ? 0x200 : 0x100);

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

/*
 * Map a GPIO register offset to a compact index in the SMC state.
 *
 * Purpose: simplify GPIO register handling in read/write paths.
 */
static int xenon_smc_gpio_index(uint32_t reg)
{
    switch (reg) {
    case GPIO_REG0:
        return 0;
    case GPIO_REG1:
        return 1;
    case GPIO_REG2:
        return 2;
    case GPIO_REG3:
        return 3;
    case GPIO_REG4:
        return 4;
    case GPIO_REG5:
        return 5;
    case GPIO_REG6:
        return 6;
    case GPIO_REG7:
        return 7;
    case GPIO_REG8:
        return 8;
    default:
        return -1;
    }
}

/*
 * Reset/initialize the SMC state to power-on defaults.
 *
 * Purpose: seed the minimal SMC model with the configured power reason, AV pack
 * type, console revision, UART backend selection, and baseline peripheral state.
 */
void xenon_smc_reset(XenonSmcState *smc, uint8_t power_on_reason,
                     uint8_t avpack_type, uint8_t console_revision,
                     const char *uart_backend, bool trace_boot)
{
    memset(smc, 0, sizeof(*smc));
    smc->power_on_reason = power_on_reason;
    smc->avpack_type = avpack_type;
    smc->console_revision = console_revision;
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
    xenon_smc_init_ddc_edid(smc);
    xenon_hana_reset(smc->hana_regs, console_revision);
}

/*
 * Guest-visible SMC register read helper.
 *
 * Purpose: implement the SMC MMIO read semantics used by CB/CD and XeLL, backed
 * by the XenonSmcState fields and FIFO/DDC helpers.
 */
uint64_t xenon_smc_read(XenonSmcState *smc, uint64_t offset, unsigned size)
{
    uint8_t tmp[8] = { 0 };
    uint32_t reg = offset & 0xff;
    int gpio_idx = xenon_smc_gpio_index(reg);

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
        xenon_smc_copy_le_out(tmp, smc->smi_int_pending, size);
        break;
    case SMI_INT_ACK_REG:
        xenon_smc_copy_le_out(tmp, smc->smi_int_ack, size);
        break;
    case SMI_INT_ENABLED_REG:
        xenon_smc_copy_le_out(tmp, smc->smi_int_enabled, size);
        break;
    case CLCK_INT_ENABLED_REG:
        xenon_smc_copy_le_out(tmp, smc->clock_int_enabled, size);
        break;
    case CLCK_INT_STATUS_REG:
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
        if (gpio_idx >= 0) {
            xenon_smc_copy_le_out(tmp, smc->gpio_regs[gpio_idx], size);
        } else {
            xenon_smc_copy_le_out(tmp, 0, size);
        }
        break;
    }

    return xenon_smc_copy_le_in(tmp, size);
}

/*
 * Guest-visible SMC register write helper.
 *
 * Purpose: implement MMIO writes into the SMC model, including UART output and
 * FIFO command submission that drives SMC responses.
 */
void xenon_smc_write(XenonSmcState *smc, uint64_t offset, uint64_t data, unsigned size)
{
    uint8_t tmp[8] = { 0 };
    uint32_t reg = offset & 0xff;
    int gpio_idx = xenon_smc_gpio_index(reg);

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
        smc->smi_int_pending = (uint32_t)data;
        break;
    case SMI_INT_ACK_REG:
        smc->smi_int_ack = (uint32_t)data;
        if (smc->smi_int_ack) {
            smc->smi_int_pending = 0;
        }
        break;
    case SMI_INT_ENABLED_REG:
        smc->smi_int_enabled = (uint32_t)data;
        break;
    case CLCK_INT_ENABLED_REG:
        smc->clock_int_enabled = (uint32_t)data;
        break;
    case CLCK_INT_STATUS_REG:
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
        if (gpio_idx >= 0) {
            smc->gpio_regs[gpio_idx] = (uint32_t)data;
            break;
        }
        break;
    }
}
