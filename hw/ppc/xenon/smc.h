/*
 * Xbox 360 Xenon SMC minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_SMC_H
#define HW_PPC_XENON_SMC_H

#include "qemu/osdep.h"
#include "hw/ppc/xenon/hana.h"
#include "hw/ppc/xenon/debug.h"

#define XENON_SMC_BASE 0xEA001000ULL
#define XENON_SMC_SIZE 0x100ULL

typedef struct XenonSmcState {
    uint32_t gpio_regs[9];
    uint8_t fifo[16];
    uint8_t fifo_pos;
    uint8_t fifo_read_pos;
    uint8_t fifo_cmd;
    uint32_t fifo_in_status;
    uint32_t fifo_out_status;
    uint32_t uart_status;
    uint32_t uart_config;
    uint32_t smi_int_pending;
    uint32_t smi_int_ack;
    uint32_t smi_int_enabled;
    uint32_t clock_int_enabled;
    uint32_t clock_int_status;
    uint32_t hana_regs[XENON_HANA_REG_COUNT];
    uint8_t ddc_regs[256];
    uint8_t ddc_edid[128];
    uint8_t power_on_reason;
    uint8_t avpack_type;
    uint8_t console_revision;
    XenonLogLevel log_level;
    uint32_t log_module_mask;
    bool uart_stdio;
    bool uart_status_flip;
} XenonSmcState;

void xenon_smc_reset(XenonSmcState *smc, uint8_t power_on_reason, uint8_t avpack_type,
                     uint8_t console_revision, const char *uart_backend,
                     XenonLogLevel log_level, uint32_t log_module_mask);
uint64_t xenon_smc_read(XenonSmcState *smc, uint64_t offset, unsigned size);
void xenon_smc_write(XenonSmcState *smc, uint64_t offset, uint64_t data, unsigned size);

#endif
