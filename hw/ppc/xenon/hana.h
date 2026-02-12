/*
 * Xbox 360 Xenon HANA/ANA state helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_HANA_H
#define HW_PPC_XENON_HANA_H

#include "qemu/osdep.h"

#define XENON_HANA_REG_COUNT 256U

void xenon_hana_reset(uint32_t regs[XENON_HANA_REG_COUNT],
                      uint8_t console_revision);
uint32_t xenon_hana_read(const uint32_t regs[XENON_HANA_REG_COUNT],
                         uint8_t addr);
void xenon_hana_write(uint32_t regs[XENON_HANA_REG_COUNT], uint8_t addr,
                      uint32_t value);

#endif /* HW_PPC_XENON_HANA_H */
