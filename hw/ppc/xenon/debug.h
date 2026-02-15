/*
 * Xbox 360 Xenon debug utilities
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_DEBUG_H
#define HW_PPC_XENON_DEBUG_H

#include "qemu/osdep.h"
#include "target/ppc/cpu.h"

typedef struct XenonMachineState XenonMachineState;

#define XENON_PC_WATCHPOINT_MAX 16

typedef enum XenonLogLevel {
    XENON_LOG_LEVEL_OFF = 0,
    XENON_LOG_LEVEL_ERROR,
    XENON_LOG_LEVEL_WARN,
    XENON_LOG_LEVEL_INFO,
    XENON_LOG_LEVEL_DEBUG,
    XENON_LOG_LEVEL_TRACE,
} XenonLogLevel;

#define XENON_LOG_MODULE_NONE       0U
#define XENON_LOG_MODULE_POST       (1U << 0)
#define XENON_LOG_MODULE_PC         (1U << 1)
#define XENON_LOG_MODULE_NAND       (1U << 2)
#define XENON_LOG_MODULE_SOC        (1U << 3)
#define XENON_LOG_MODULE_SECENG     (1U << 4)
#define XENON_LOG_MODULE_SMC        (1U << 5)
#define XENON_LOG_MODULE_SATA       (1U << 6)
#define XENON_LOG_MODULE_XGPU       (1U << 7)
#define XENON_LOG_MODULE_PATCH      (1U << 8)
#define XENON_LOG_MODULE_BOOT       (1U << 9)
#define XENON_LOG_MODULE_MACHINE    (1U << 10)
#define XENON_LOG_MODULE_IIC        (1U << 11)
#define XENON_LOG_MODULE_TRACE      (1U << 12)
#define XENON_LOG_MODULE_ALL        ((1U << 13) - 1)

typedef struct XenonPcWatchpoint {
    uint64_t ea;
    char *label;
    bool triggered;
    bool pending_dump;
} XenonPcWatchpoint;

char *xenon_log_modules_to_string(uint32_t mask);
uint32_t xenon_log_modules_from_string(const char *value, bool *ok);
bool xenon_log_level_from_string(const char *value, XenonLogLevel *level);
const char *xenon_log_level_name(XenonLogLevel level);
void xenon_log_set_level(XenonMachineState *xms, XenonLogLevel level);
void xenon_log_set_modules(XenonMachineState *xms, uint32_t mask);
bool xenon_log_enabled(const XenonMachineState *xms, XenonLogLevel level,
                       uint32_t module);
void xenon_log(const XenonMachineState *xms, XenonLogLevel level,
               uint32_t module, const char *fmt, ...) G_GNUC_PRINTF(4, 5);
void xenon_log_dump_disasm(XenonMachineState *xms, CPUPPCState *env,
                           uint64_t start_pc, unsigned count,
                           const char *context, XenonLogLevel level,
                           uint32_t module);

#define XENON_LOG_INTERNAL(xms, level, module, fmt, ...)               \
    do {                                                               \
        if (xenon_log_enabled((xms), (level), (module))) {             \
            xenon_log((xms), (level), (module), fmt, ##__VA_ARGS__);    \
        }                                                              \
    } while (0)

#define XENON_LOG_ERROR(xms, module, fmt, ...)   \
    XENON_LOG_INTERNAL(xms, XENON_LOG_LEVEL_ERROR, module, fmt, ##__VA_ARGS__)
#define XENON_LOG_WARN(xms, module, fmt, ...)    \
    XENON_LOG_INTERNAL(xms, XENON_LOG_LEVEL_WARN, module, fmt, ##__VA_ARGS__)
#define XENON_LOG_INFO(xms, module, fmt, ...)    \
    XENON_LOG_INTERNAL(xms, XENON_LOG_LEVEL_INFO, module, fmt, ##__VA_ARGS__)
#define XENON_LOG_DEBUG(xms, module, fmt, ...)   \
    XENON_LOG_INTERNAL(xms, XENON_LOG_LEVEL_DEBUG, module, fmt, ##__VA_ARGS__)
#define XENON_LOG_TRACE(xms, module, fmt, ...)   \
    XENON_LOG_INTERNAL(xms, XENON_LOG_LEVEL_TRACE, module, fmt, ##__VA_ARGS__)

#endif /* HW_PPC_XENON_DEBUG_H */
