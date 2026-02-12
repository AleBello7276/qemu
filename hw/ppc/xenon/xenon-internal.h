/*
 * Xbox 360 Xenon machine shared internals
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_XENON_INTERNAL_H
#define HW_PPC_XENON_INTERNAL_H

#include "hw/boards.h"
#include "hw/ppc/xenon/smc.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "target/ppc/cpu.h"
#include "ui/console.h"

#define XENON_MAX_CPUS              6
#define XENON_SROM_BASE             0x00000000ULL
#define XENON_SROM_SIZE             0x00040000ULL
#define XENON_1BL_SIZE              0x00008000ULL
#define XENON_FUSE_BASE             0x00020000ULL
#define XENON_FUSE_LINES            12
#define XENON_FUSE_LINE_STRIDE      0x200
#define XENON_FUSE_REPLICA_COUNT    64
#define XENON_NAND_BASE             0xC8000000ULL
#define XENON_NAND_MMIO_SIZE        0x04000000ULL /* 64 MiB */
#define XENON_NAND_MIN_SIZE         0x01080000ULL /* 16 MiB raw + spare */
#define XENON_NAND_MAX_SIZE         0x04200000ULL /* 64 MiB raw + spare */
#define XENON_NAND_LOGICAL_PAGE     0x200U
#define XENON_NAND_RAW_PAGE         0x210U
#define XENON_NB_MMIO_BASE          0xE4000000ULL
#define XENON_NB_MMIO_SIZE          0x00040000ULL
#define XENON_SFCX_MMIO_BASE        0xEA00C000ULL
#define XENON_SFCX_MMIO_SIZE        0x00000400ULL
#define XENON_IIC_MMIO_BASE         0x00050000ULL
#define XENON_IIC_MMIO_SIZE         0x00008000ULL
#define XENON_SOC_E1_BASE           0xE1000000ULL
#define XENON_SOC_E1_SIZE           0x00100000ULL
#define XENON_PCI_CFG_BASE          0xD0000000ULL
#define XENON_PCI_CFG_SIZE          0x01000000ULL
#define XENON_XGPU_MMIO_BASE        0xE4010000ULL
#define XENON_XGPU_MMIO_SIZE        0x00020000ULL
#define XENON_XGPU_BAR0_BASE        0xEC800000ULL
#define XENON_RAM_BASE              0x00000000ULL
#define XENON_SECENG_REGION_SIZE    0x0000010000000000ULL /* 1 TiB */
#define XENON_HASH_BASE_LO          0x0000010000000000ULL
#define XENON_SOC_BASE_LO           0x0000020000000000ULL
#define XENON_ENCR_BASE_LO          0x0000030000000000ULL
#define XENON_HASH_BASE_HI          0x8000010000000000ULL
#define XENON_SOC_BASE_HI           0x8000020000000000ULL
#define XENON_ENCR_BASE_HI          0x8000030000000000ULL

#define SPR_XENON_HID6              0x3F9
#define SPR_XENON_PPE_TLB_INDEX_HINT 0x3B2
#define SPR_XENON_PPE_TLB_INDEX     0x3B3
#define SPR_XENON_PPE_TLB_VPN       0x3B4
#define SPR_XENON_PPE_TLB_RPN       0x3B5
#define XENON_LPCR_INIT             0x402ULL
#define XENON_HID6_INIT             0x1803800000000ULL
#define XENON_HRMOR_INIT            0x20000000000ULL
#define XENON_SMC_PWRBTN            0x11U
#define XENON_SMC_EJECT             0x12U
#define XENON_SMC_AVPACK_DEFAULT    0x1FU
#define XENON_PRV_POR_STATUS_ADDR   0x00061000ULL
#define XENON_PRV_PMCTRL_ADDR       0x00061188ULL
#define XENON_PRV_MMIO_SIZE         0x00000200ULL
#define XENON_PRV_POR_STATUS_INIT   0x2000000000000000ULL
#define XENON_PRV_PMCTRL_INIT       0x382C00000000B001ULL
#define XENON_SMC_TRACE_LIMIT       64U

typedef struct XenonMachineState XenonMachineState;
typedef enum XenonConsoleRevision {
    XENON_CONSOLE_XENON = 0,
    XENON_CONSOLE_ZEPHYR = 1,
    XENON_CONSOLE_FALCON = 2,
    XENON_CONSOLE_JASPER = 3,
    XENON_CONSOLE_TRINITY = 4,
    XENON_CONSOLE_CORONA = 5,
    XENON_CONSOLE_CORONA_4GB = 6,
    XENON_CONSOLE_WINCHESTER = 7,
} XenonConsoleRevision;

typedef struct XenonSecEngWindow {
    MemoryRegion mr;
    hwaddr base;
    char *name;
    XenonMachineState *owner;
} XenonSecEngWindow;

struct XenonMachineState {
    MachineState parent_obj;

    char *nand_path;
    char *fuses_path;
    char *onebl_path;
    char *config_path;
    char *smc_uart;
    XenonConsoleRevision console_revision;
    uint8_t smc_power_on_reason;
    uint8_t smc_avpack_type;
    bool user_set_nand;
    bool user_set_fuses;
    bool user_set_onebl;
    bool user_set_trace_boot;
    bool user_set_pretty_post;
    bool user_set_rgh2_patches;
    bool user_set_boot_mode;
    bool user_set_smc_uart;
    bool user_set_smc_avpack;
    bool user_set_console_revision;

    MemoryRegion srom;
    MemoryRegion nand;
    MemoryRegion smc;
    MemoryRegion nb_mmio;
    MemoryRegion sfcx_mmio;
    MemoryRegion pci_cfg_flat;
    MemoryRegion xgpu_bar0;
    XenonSecEngWindow seceng_windows[6];
    XenonSmcState smc_state;

    uint8_t *srom_data;
    uint8_t *nand_raw_data;
    size_t nand_raw_size;
    uint8_t *nand_mmio_data;
    uint8_t *nb_mmio_data;
    uint8_t *soc_e1_data;
    uint8_t prv_mmio_data[XENON_PRV_MMIO_SIZE];
    uint8_t *iic_mmio_data;
    uint8_t *pci_cfg_data;
    uint8_t *xgpu_mmio_data;
    uint8_t sfcx_page_buf[0x210];
    uint32_t xgpu_me_ucode[0x900];
    uint32_t xgpu_pfp_ucode[0x120];
    uint32_t xgpu_me_waddr;
    uint32_t xgpu_me_raddr;
    uint32_t xgpu_pfp_addr;
    PowerPCCPU *cpus[XENON_MAX_CPUS];
    PowerPCCPU *boot_cpu;
    uint8_t cpu_count;
    uint8_t cpu_online_mask;
    bool iic_migr2_flip;
    QemuConsole *dbg_con;
    uint8_t *dbg_fb_shadow;
    size_t dbg_fb_shadow_size;
    uint32_t dbg_fb_base;
    uint32_t dbg_fb_pitch;
    uint32_t dbg_fb_width;
    uint32_t dbg_fb_height;
    bool dbg_fb_enabled;
    QEMUTimer *pc_log_timer;
    uint64_t last_logged_pc;
    uint64_t pc_log_count;
    int64_t last_pc_log_ms;
    bool have_last_post_code;
    uint64_t last_post_code;
    uint64_t last_exception_pc;
    uint64_t last_exc_handler_pc;
    uint32_t exc_handler_same_pc_count;
    uint32_t nand_trace_reads;
    uint32_t nand_trace_writes;
    uint32_t soc_trace_reads;
    uint32_t soc_trace_writes;
    uint32_t secotp_trace_reads;
    uint32_t secotp_trace_writes;
    uint32_t xgpu_trace_reads;
    uint32_t xgpu_trace_writes;
    uint32_t sfcx_trace_reads;
    uint32_t sfcx_trace_writes;
    uint32_t smc_trace_reads;
    uint32_t smc_trace_writes;
    uint32_t smc_last_in_status;
    uint32_t smc_last_out_status;
    uint32_t smc_last_uart_status;
    uint32_t hwinit_fetch_logs;
    bool hwinit_bytecode_dumped;
    bool smc_last_status_valid;
    bool nb_training_done;
    bool low_mmio_aliases_enabled;
    bool trace_boot;
    bool pretty_post;
    bool rgh2_patches;
    bool rgh2_patches_applied;
};

extern const MemoryRegionOps xenon_nand_ops;
extern const MemoryRegionOps xenon_nb_mmio_ops;
extern const MemoryRegionOps xenon_smc_ops;
extern const MemoryRegionOps xenon_sfcx_ops;
extern const MemoryRegionOps xenon_pci_cfg_ops;
extern const MemoryRegionOps xenon_xgpu_bar0_ops;
const char *xenon_console_revision_name(XenonConsoleRevision rev);

#endif /* HW_PPC_XENON_INTERNAL_H */
