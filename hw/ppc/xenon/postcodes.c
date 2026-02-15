/*
 * Xbox 360 Xenon POST code decoding
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ppc/xenon/postcodes.h"

typedef struct XenonPostCodeDesc {
    uint8_t code;
    const char *desc;
} XenonPostCodeDesc;

static const XenonPostCodeDesc xenon_post_descs[] = {
    { 0x10, "1BL started" },
    { 0x11, "1BL FSB_CONFIG_PHY_CONTROL" },
    { 0x12, "1BL FSB_CONFIG_RX_STATE" },
    { 0x13, "1BL FSB_CONFIG_TX_STATE" },
    { 0x14, "1BL FSB_CONFIG_TX_CREDITS" },
    { 0x15, "1BL FETCH_OFFSET" },
    { 0x16, "1BL FETCH_HEADER" },
    { 0x17, "1BL VERIFY_HEADER" },
    { 0x18, "1BL FETCH_CONTENTS" },
    { 0x19, "1BL HMACSHA_COMPUTE" },
    { 0x1A, "1BL RC4_INITIALIZE" },
    { 0x1B, "1BL RC4_DECRYPT" },
    { 0x1C, "1BL SHA_COMPUTE" },
    { 0x1D, "1BL SIG_VERIFY" },
    { 0x1E, "1BL BRANCH_TO_CB" },

    { 0x20, "CB ENTRY" },
    { 0x21, "CB INIT_SECOTP" },
    { 0x22, "CB INIT_SECENG" },
    { 0x23, "CB INIT_SYSRAM" },
    { 0x24, "CB VERIFY_OFFSET_3BL_CC" },
    { 0x25, "CB LOCATE_3BL_CC" },
    { 0x26, "CB FETCH_HEADER_3BL_CC" },
    { 0x27, "CB VERIFY_HEADER_3BL_CC" },
    { 0x28, "CB FETCH_CONTENTS_3BL_CC" },
    { 0x29, "CB HMACSHA_COMPUTE_3BL_CC" },
    { 0x2A, "CB RC4_INITIALIZE_3BL_CC" },
    { 0x2B, "CB RC4_DECRYPT_3BL_CC" },
    { 0x2C, "CB SHA_COMPUTE_3BL_CC" },
    { 0x2D, "CB SIG_VERIFY_3BL_CC" },
    { 0x2E, "CB HWINIT" },
    { 0x2F, "CB RELOCATE" },
    { 0x30, "CB VERIFY_OFFSET_4BL_CD" },
    { 0x31, "CB FETCH_HEADER_4BL_CD" },
    { 0x32, "CB VERIFY_HEADER_4BL_CD" },
    { 0x33, "CB FETCH_CONTENTS_4BL_CD" },
    { 0x34, "CB HMACSHA_COMPUTE_4BL_CD" },
    { 0x35, "CB RC4_INITIALIZE_4BL_CD" },
    { 0x36, "CB RC4_DECRYPT_4BL_CD" },
    { 0x37, "CB SHA_COMPUTE_4BL_CD" },
    { 0x38, "CB SIG_VERIFY_4BL_CD" },
    { 0x39, "CB SHA_VERIFY_4BL_CD" },
    { 0x3A, "CB BRANCH_TO_CD" },
    { 0x3B, "CB PCI_INIT" },

    { 0x40, "CD ENTRY" },
    { 0x41, "CD VERIFY_OFFSET" },
    { 0x42, "CD FETCH_HEADER" },
    { 0x43, "CD VERIFY_HEADER" },
    { 0x44, "CD FETCH_CONTENTS" },
    { 0x45, "CD HMACSHA_COMPUTE" },
    { 0x46, "CD RC4_INITIALIZE" },
    { 0x47, "CD RC4_DECRYPT" },
    { 0x48, "CD SHA_COMPUTE" },
    { 0x49, "CD SHA_VERIFY" },
    { 0x4A, "CD LOAD_6BL_CF" },
    { 0x4B, "CD LZX_EXPAND" },
    { 0x4C, "CD SWEEP_CACHES" },
    { 0x4D, "CD DECODE_FUSES" },
    { 0x4E, "CD FETCH_OFFSET_6BL_CF" },
    { 0x4F, "CD VERIFY_OFFSET_6BL_CF" },
    { 0x50, "CD LOAD_UPDATE_1" },
    { 0x51, "CD LOAD_UPDATE_2" },
    { 0x52, "CD BRANCH_KERNEL_HV" },
    { 0x53, "CD DECRYPT_VERIFY_HV_CERT" },

    { 0x58, "HV INIT_HYPERVISOR" },
    { 0x59, "HV INIT_SOC_MMIO" },
    { 0x5A, "HV INIT_XEX_TRAINING" },
    { 0x5B, "HV INIT_KEYRING" },
    { 0x5C, "HV INIT_KEYS" },
    { 0x5D, "HV INIT_SOC_INT" },
    { 0x5E, "HV INIT_SOC_INT_COMPLETE" },
    { 0x5F, "HV INIT_HYPERVISOR_COMPLETE" },

    { 0x81, "1BL PANIC MACHINE_CHECK" },
    { 0x82, "1BL PANIC DATA_STORAGE" },
    { 0x83, "1BL PANIC DATA_SEGMENT" },
    { 0x84, "1BL PANIC INSTRUCTION_STORAGE" },
    { 0x85, "1BL PANIC INSTRUCTION_SEGMENT" },
    { 0x86, "1BL PANIC EXTERNAL" },
    { 0x87, "1BL PANIC ALIGNMENT" },
    { 0x88, "1BL PANIC PROGRAM" },
    { 0x89, "1BL PANIC FPU_UNAVAILABLE" },
    { 0x8A, "1BL PANIC DECREMENTER" },
    { 0x8B, "1BL PANIC HYPERVISOR_DECREMENTER" },
    { 0x8C, "1BL PANIC SYSTEM_CALL" },
    { 0x8D, "1BL PANIC TRACE" },
    { 0x8E, "1BL PANIC VPU_UNAVAILABLE" },
    { 0x8F, "1BL PANIC MAINTENANCE" },
    { 0x90, "1BL PANIC VMX_ASSIST" },
    { 0x91, "1BL PANIC THERMAL_MANAGEMENT" },
    { 0x92, "1BL PANIC WRONG_CPU_THREAD" },
    { 0x93, "1BL PANIC TOO_MANY_CORES" },
    { 0x94, "1BL PANIC VERIFY_OFFSET" },
    { 0x95, "1BL PANIC VERIFY_HEADER" },
    { 0x96, "1BL PANIC SIG_VERIFY" },
    { 0x97, "1BL PANIC NONHOST_RESUME_STATUS" },
    { 0x98, "1BL PANIC NEXT_STAGE_SIZE" },

    { 0x9B, "CB PANIC VERIFY_SECOTP_1" },
    { 0x9C, "CB PANIC VERIFY_SECOTP_2" },
    { 0x9D, "CB PANIC VERIFY_SECOTP_3" },
    { 0x9E, "CB PANIC VERIFY_SECOTP_4" },
    { 0x9F, "CB PANIC VERIFY_SECOTP_5" },
    { 0xA0, "CB PANIC VERIFY_SECOTP_6" },
    { 0xA1, "CB PANIC VERIFY_SECOTP_7" },
    { 0xA2, "CB PANIC VERIFY_SECOTP_8" },
    { 0xA3, "CB PANIC VERIFY_SECOTP_9" },
    { 0xA4, "CB PANIC VERIFY_SECOTP_10" },
    { 0xA5, "CB PANIC VERIFY_OFFSET_3BL_CC" },
    { 0xA6, "CB PANIC LOCATE_3BL_CC" },
    { 0xA7, "CB PANIC VERIFY_HEADER_3BL_CC" },
    { 0xA8, "CB PANIC SIG_VERIFY_3BL_CC" },
    { 0xA9, "CB PANIC HWINIT" },
    { 0xAA, "CB PANIC VERIFY_OFFSET_4BL_CC" },
    { 0xAB, "CB PANIC VERIFY_HEADER_4BL_CC" },
    { 0xAC, "CB PANIC SIG_VERIFY_4BL_CC" },
    { 0xAD, "CB PANIC SHA_VERIFY_4BL_CC" },
    { 0xAE, "CB PANIC UNEXPECTED_INTERRUPT" },
    { 0xAF, "CB PANIC UNSUPPORTED_RAM_SIZE" },
    { 0xB0, "CB PANIC UNKNOWN_B0" },

    { 0xB1, "CD PANIC VERIFY_OFFSET" },
    { 0xB2, "CD PANIC VERIFY_HEADER" },
    { 0xB3, "CD PANIC SHA_VERIFY" },
    { 0xB4, "CD PANIC LZX_EXPAND" },
    { 0xB5, "CD PANIC VERIFY_OFFSET_6BL" },
    { 0xB6, "CD PANIC DECODE_FUSES" },
    { 0xB7, "CD PANIC UPDATE_MISSING" },
    { 0xB8, "CD PANIC CF_HASH_AUTH" },

    { 0xC1, "CE/CF PANIC LZX_EXPAND_1" },
    { 0xC2, "CE/CF PANIC LZX_EXPAND_2" },
    { 0xC3, "CE/CF PANIC LZX_EXPAND_3" },
    { 0xC4, "CE/CF PANIC LZX_EXPAND_4" },
    { 0xC5, "CE/CF PANIC LZX_EXPAND_5" },
    { 0xC6, "CE/CF PANIC LZX_EXPAND_6" },
    { 0xC7, "CE/CF PANIC LZX_EXPAND_7" },
    { 0xC8, "CE/CF PANIC SHA_VERIFY" },

    { 0xD0, "CB_A ENTRY" },
    { 0xD1, "CB_A READ_FUSES" },
    { 0xD2, "CB_A VERIFY_OFFSET_CB_B" },
    { 0xD3, "CB_A FETCH_HEADER_CB_B" },
    { 0xD4, "CB_A VERIFY_HEADER_CB_B" },
    { 0xD5, "CB_A FETCH_CONTENTS_CB_B" },
    { 0xD6, "CB_A HMACSHA_COMPUTE_CB_B" },
    { 0xD7, "CB_A RC4_INITIALIZE_CB_B" },
    { 0xD8, "CB_A RC4_DECRYPT_CB_B" },
    { 0xD9, "CB_A SHA_COMPUTE_CB_B" },
    { 0xDA, "CB_A SHA_VERIFY_CB_B" },
    { 0xDB, "CB_A BRANCH_CB_B" },

    { 0xF0, "CB_A PANIC VERIFY_OFFSET_CB_B" },
    { 0xF1, "CB_A PANIC VERIFY_HEADER_CB_B" },
    { 0xF2, "CB_A PANIC SHA_VERIFY_CB_B" },
    { 0xF3, "CB_A PANIC ENTRY_SIZE_INVALID_CB_B" },
};

/*
 * Decode the high byte of the POST register into a mnemonic string.
 *
 * Purpose: provide stable, human-readable labels for the secure boot timeline
 * (1BL/CB/CD/HV), matching xenon-emu's general naming.
 */
const char *xenon_decode_post_code(uint8_t post)
{
    for (size_t i = 0; i < ARRAY_SIZE(xenon_post_descs); i++) {
        if (xenon_post_descs[i].code == post) {
            return xenon_post_descs[i].desc;
        }
    }

    return NULL;
}
