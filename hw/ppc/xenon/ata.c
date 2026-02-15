/*
 * Xbox 360 Xenon ATA/ATAPI (ODD/HDD) minimal model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/ppc/xenon/ata.h"
#include "hw/ppc/xenon/xenon-internal.h"

#include <fcntl.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define XENON_SATA_ODD_BASE         0x000
#define XENON_SATA_HDD_BASE         0x100

#define XENON_ATA_REG_DATA          0x00
#define XENON_ATA_REG_ERROR         0x01
#define XENON_ATA_REG_FEATURES      0x01
#define XENON_ATA_REG_SECTOR_COUNT  0x02
#define XENON_ATA_REG_LBA_LOW       0x03
#define XENON_ATA_REG_LBA_MID       0x04
#define XENON_ATA_REG_LBA_HIGH      0x05
#define XENON_ATA_REG_DEVICE        0x06
#define XENON_ATA_REG_STATUS        0x07
#define XENON_ATA_REG_COMMAND       0x07
#define XENON_ATA_REG_ALT_STATUS    0x0A
#define XENON_ATA_REG_DEV_CONTROL   0x0A

#define XENON_ATA_REG_SSTATUS       0x10
#define XENON_ATA_REG_SERROR        0x14
#define XENON_ATA_REG_SCONTROL      0x18
#define XENON_ATA_REG_SACTIVE       0x1C

/* libxenon maps the secondary block at +0x20 (control + BMDMA) */
#define XENON_ATA_REG2_CMD          0x20
#define XENON_ATA_REG2_STATUS       0x22
#define XENON_ATA_REG2_TABLE        0x24

#define XENON_ATA_CMD_PACKET                0xA0
#define XENON_ATA_CMD_IDENTIFY_PACKET       0xA1
#define XENON_ATA_CMD_IDENTIFY_DEVICE       0xEC
#define XENON_ATA_CMD_SET_FEATURES          0xEF
#define XENON_ATA_CMD_READ_DMA_EXT          0x25

#define XENON_ATA_ST_ERR            0x01
#define XENON_ATA_ST_DRQ            0x08
#define XENON_ATA_ST_DF             0x10
#define XENON_ATA_ST_DRDY           0x40
#define XENON_ATA_ST_BSY            0x80

#define XENON_ATA_ERR_ABRT          0x04

#define XENON_ATA_DMA_ACTIVE        0x01
#define XENON_ATA_DMA_ERR           0x02
#define XENON_ATA_DMA_INTR          0x04
#define XENON_ATA_DMA_WR            0x08

#define XENON_ATA_SECTOR_SIZE       512U
#define XENON_ATAPI_SECTOR_SIZE     2048U

#define XENON_SCSI_TEST_UNIT_READY  0x00
#define XENON_SCSI_REQUEST_SENSE    0x03
#define XENON_SCSI_INQUIRY          0x12
#define XENON_SCSI_READ_CAPACITY    0x25
#define XENON_SCSI_READ10           0x28
#define XENON_SCSI_READ_TOC         0x43
#define XENON_SCSI_MODE_SELECT10    0x55
#define XENON_SCSI_MODE_SENSE10     0x5A

#define XENON_ATA_TRACE_LIMIT       128U

typedef struct XenonAtaPort {
    bool atapi;
    bool media_present;
    int media_fd;
    uint64_t media_size;

    uint8_t error;
    uint8_t features;
    uint8_t sector_count;
    uint8_t prev_sector_count;
    uint8_t lba_low;
    uint8_t lba_mid;
    uint8_t lba_high;
    uint8_t prev_lba_low;
    uint8_t prev_lba_mid;
    uint8_t prev_lba_high;
    uint8_t device;
    uint8_t status;
    uint8_t command;
    uint8_t device_control;

    uint8_t dma_command;
    uint8_t dma_status;
    uint32_t dma_table_raw;

    uint32_t sstatus;
    uint32_t serror;
    uint32_t scontrol;
    uint32_t sactive;

    GByteArray *data_in;
    GByteArray *data_out;
    uint32_t data_out_pos;
    uint32_t expected_data_in;

    uint8_t packet[16];
    uint32_t packet_filled;
    bool waiting_for_packet;

    uint8_t identify[512];
} XenonAtaPort;

struct XenonAtaState {
    XenonAtaPort odd;
    XenonAtaPort hdd;
    uint8_t inquiry[36];
    uint32_t trace_reads;
    uint32_t trace_writes;
};

/*
 * Load a big-endian integer from a byte buffer (1/2/4/8).
 *
 * Purpose: ATA/SATA BAR accesses are modeled as big-endian in this machine,
 * while internal state is stored as bytes.
 */
static uint64_t xenon_ata_pack_be(const uint8_t *buf, unsigned size)
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

/*
 * Store a big-endian integer into a byte buffer (1/2/4/8).
 *
 * Purpose: helper for composing MMIO readback values from register state.
 */
static void xenon_ata_unpack_be(uint8_t *buf, unsigned size, uint64_t data)
{
    memset(buf, 0, size);
    switch (size) {
    case 1:
        buf[0] = (uint8_t)data;
        break;
    case 2:
        stw_be_p(buf, (uint16_t)data);
        break;
    case 4:
        stl_be_p(buf, (uint32_t)data);
        break;
    case 8:
        stq_be_p(buf, data);
        break;
    default:
        break;
    }
}

/*
 * Compute the "idle" status value for a port.
 *
 * Purpose: centralize default status bits for ATA vs ATAPI devices.
 */
static uint8_t xenon_ata_idle_status(const XenonAtaPort *p)
{
    return p->atapi ? (XENON_ATA_ST_DRDY | XENON_ATA_ST_DF) : XENON_ATA_ST_DRDY;
}

/*
 * Clear in-flight data buffers and packet state for a port.
 *
 * Purpose: reset the port's FIFO-like state machine between commands.
 */
static void xenon_ata_port_clear_buffers(XenonAtaPort *p)
{
    if (p->data_in) {
        g_byte_array_set_size(p->data_in, 0);
    }
    if (p->data_out) {
        g_byte_array_set_size(p->data_out, 0);
    }
    p->data_out_pos = 0;
    p->expected_data_in = 0;
    p->packet_filled = 0;
    p->waiting_for_packet = false;
}

/*
 * Apply an ATA soft reset to the port.
 *
 * Purpose: reset task file registers and status to the "device ready" baseline
 * expected by libxenon/XeLL probes.
 */
static void xenon_ata_port_soft_reset(XenonAtaPort *p)
{
    p->error = p->atapi ? XENON_ATA_ERR_ABRT : 0;
    p->features = 0;
    p->sector_count = 1;
    p->prev_sector_count = 0;
    p->lba_low = 1;
    p->prev_lba_low = 0;
    p->prev_lba_mid = 0;
    p->prev_lba_high = 0;
    p->device = 0xE0;
    p->command = 0;
    p->dma_command = 0;
    p->dma_status = 0;
    p->dma_table_raw = 0;

    if (p->atapi) {
        p->lba_mid = 0x14;
        p->lba_high = 0xEB;
    } else {
        p->lba_mid = 0;
        p->lba_high = 0;
    }

    p->status = xenon_ata_idle_status(p);
    xenon_ata_port_clear_buffers(p);
}

/*
 * Open an ODD/HDD backing image and return its size.
 *
 * Purpose: keep the ATA model decoupled from QEMU block layer; this bring-up
 * model uses simple host file descriptors.
 */
static int xenon_ata_open_image(const char *path, uint64_t *size_out)
{
    int fd;
    off_t sz;

    *size_out = 0;
    if (!path || !path[0]) {
        return -1;
    }

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        error_report("xbox360: sata: failed to open image '%s': %s",
                     path, strerror(errno));
        return -1;
    }

    sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) {
        error_report("xbox360: sata: failed to size image '%s': %s",
                     path, strerror(errno));
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    *size_out = (uint64_t)sz;
    return fd;
}

/*
 * Set a 16-bit word in the ATA IDENTIFY data block (little-endian words).
 *
 * Purpose: helper for building HDD/ODD identify response payloads.
 */
static void xenon_ata_id_set_word(uint8_t *id, unsigned word, uint16_t v)
{
    stw_le_p(id + (word * 2U), v);
}

/*
 * Set an ATA IDENTIFY string field with ATA word byte swapping.
 *
 * Purpose: IDENTIFY strings are stored as big-endian pairs within 16-bit words
 * (ATA legacy quirk); this writes a padded, swapped field.
 */
static void xenon_ata_id_set_string(uint8_t *id, unsigned word,
                                    unsigned words, const char *s)
{
    g_autofree char *tmp = g_malloc0(words * 2U);
    size_t n;

    memset(tmp, ' ', words * 2U);
    n = MIN(strlen(s), (size_t)(words * 2U));
    memcpy(tmp, s, n);

    for (unsigned i = 0; i < words; i++) {
        id[(word + i) * 2U + 0] = (uint8_t)tmp[i * 2U + 1U];
        id[(word + i) * 2U + 1] = (uint8_t)tmp[i * 2U + 0U];
    }
}

/*
 * Construct an ATA IDENTIFY DEVICE block for the HDD port.
 *
 * Purpose: provide plausible capacity/features so the guest can probe a disk
 * even when no backing image is configured.
 */
static void xenon_ata_build_hdd_identify(XenonAtaPort *p)
{
    uint32_t sectors28;
    uint64_t sectors48;

    memset(p->identify, 0, sizeof(p->identify));

    sectors48 = p->media_size ? (p->media_size / XENON_ATA_SECTOR_SIZE)
                              : (40ULL * 1024ULL * 1024ULL / XENON_ATA_SECTOR_SIZE);
    sectors28 = (uint32_t)MIN(sectors48, 0x0FFFFFFFULL);

    xenon_ata_id_set_word(p->identify, 0, 0x0040);
    xenon_ata_id_set_word(p->identify, 1, 0x3FFF);
    xenon_ata_id_set_word(p->identify, 3, 0x0010);
    xenon_ata_id_set_word(p->identify, 6, 0x003F);
    xenon_ata_id_set_word(p->identify, 47, 0x8001);
    xenon_ata_id_set_word(p->identify, 49, (1U << 9));
    xenon_ata_id_set_word(p->identify, 53, 0x0007);
    xenon_ata_id_set_word(p->identify, 63, 0x0007);
    xenon_ata_id_set_word(p->identify, 80, 0x007E);
    xenon_ata_id_set_word(p->identify, 83, (1U << 10));

    xenon_ata_id_set_word(p->identify, 60, sectors28 & 0xFFFFU);
    xenon_ata_id_set_word(p->identify, 61, (sectors28 >> 16) & 0xFFFFU);

    xenon_ata_id_set_word(p->identify, 100, (uint16_t)(sectors48 & 0xFFFFU));
    xenon_ata_id_set_word(p->identify, 101, (uint16_t)((sectors48 >> 16) & 0xFFFFU));
    xenon_ata_id_set_word(p->identify, 102, (uint16_t)((sectors48 >> 32) & 0xFFFFU));
    xenon_ata_id_set_word(p->identify, 103, (uint16_t)((sectors48 >> 48) & 0xFFFFU));

    xenon_ata_id_set_string(p->identify, 10, 10, "QEMU360HDD0001");
    xenon_ata_id_set_string(p->identify, 23, 4, "0.1");
    xenon_ata_id_set_string(p->identify, 27, 20, "QEMU360 VIRTUAL HDD");
}

/*
 * Construct an ATA IDENTIFY PACKET DEVICE block for the ODD (ATAPI) port.
 *
 * Purpose: return a stable DG-16D5S-like identify payload for Xbox 360 code
 * paths that expect a DVD drive.
 */
static void xenon_ata_build_odd_identify(XenonAtaPort *p)
{
    memset(p->identify, 0, sizeof(p->identify));

    xenon_ata_id_set_word(p->identify, 0, 0x85C0);
    xenon_ata_id_set_word(p->identify, 47, 0x8001);
    xenon_ata_id_set_word(p->identify, 49, (1U << 9));
    xenon_ata_id_set_word(p->identify, 63, 0x0007);
    xenon_ata_id_set_word(p->identify, 80, 0x003F);

    xenon_ata_id_set_string(p->identify, 10, 10, "DG16D5S0000001");
    xenon_ata_id_set_string(p->identify, 23, 4, "1532");
    xenon_ata_id_set_string(p->identify, 27, 20, "PLDS DG-16D5S");
}

/*
 * Build a default SCSI INQUIRY response used for ATAPI PACKET commands.
 *
 * Purpose: Xenon ODD probing uses ATAPI/SCSI commands; return a vendor/product
 * string matching common 360 drives.
 */
static void xenon_ata_build_inquiry(XenonAtaState *ata)
{
    static const uint8_t inquiry[36] = {
        0x05, 0x80, 0x00, 0x32, 0x5B, 0x00, 0x00, 0x00,
        'P', 'L', 'D', 'S', ' ', ' ', ' ', ' ',
        'D', 'G', '-', '1', '6', 'D', '5', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        '1', '5', '3', '2',
    };

    memcpy(ata->inquiry, inquiry, sizeof(inquiry));
}

/*
 * Queue a data-out buffer for the guest to read (DRQ asserted).
 *
 * Purpose: model the ATA data register FIFO for IDENTIFY and PACKET responses.
 */
static void xenon_ata_queue_output(XenonAtaPort *p, const uint8_t *data, size_t len)
{
    g_byte_array_set_size(p->data_out, 0);
    g_byte_array_append(p->data_out, data, len);
    p->data_out_pos = 0;
    p->status = xenon_ata_idle_status(p) | XENON_ATA_ST_DRQ;
}

/*
 * Read from the backing media image into a buffer with bounds handling.
 *
 * Purpose: keep command implementations simple by centralizing short-read and
 * "no media" behavior.
 */
static void xenon_ata_read_media(XenonAtaPort *p, uint64_t off,
                                 uint8_t *dst, size_t len)
{
    memset(dst, 0, len);

    if (p->media_fd < 0 || off >= p->media_size) {
        return;
    }

    if (off + len > p->media_size) {
        len = (size_t)(p->media_size - off);
    }

    while (len > 0) {
        ssize_t r = pread(p->media_fd, dst, len, (off_t)off);

        if (r <= 0) {
            break;
        }
        dst += r;
        off += r;
        len -= (size_t)r;
    }
}

/*
 * Prepare the output buffer for an ATA READ DMA EXT command.
 *
 * Purpose: compute 48-bit LBA + sector count, then stage the requested sectors
 * into data_out for the DMA engine to transfer.
 */
static void xenon_ata_prepare_read_dma_ext(XenonAtaPort *p)
{
    uint64_t lba;
    uint32_t count;
    size_t bytes;

    lba = ((uint64_t)p->prev_lba_high << 40) |
          ((uint64_t)p->prev_lba_mid << 32) |
          ((uint64_t)p->prev_lba_low << 24) |
          ((uint64_t)p->lba_high << 16) |
          ((uint64_t)p->lba_mid << 8) |
          ((uint64_t)p->lba_low);

    count = ((uint32_t)p->prev_sector_count << 8) | p->sector_count;
    if (count == 0) {
        count = 65536U;
    }

    bytes = (size_t)count * XENON_ATA_SECTOR_SIZE;
    g_byte_array_set_size(p->data_out, bytes);
    p->data_out_pos = 0;
    xenon_ata_read_media(p, lba * XENON_ATA_SECTOR_SIZE,
                         p->data_out->data, bytes);
    p->status = xenon_ata_idle_status(p);
}

/*
 * Prepare an ATAPI READ CAPACITY (10) response.
 *
 * Purpose: report the last logical block address and sector size to software
 * probing the ODD.
 */
static void xenon_ata_prepare_read_capacity(XenonAtaPort *p)
{
    uint8_t data[8] = { 0 };
    uint32_t sectors = p->media_size ?
        (uint32_t)(p->media_size / XENON_ATAPI_SECTOR_SIZE) : 0;
    uint32_t last_lba = sectors ? (sectors - 1U) : 0;

    stl_be_p(data + 0, last_lba);
    stl_be_p(data + 4, XENON_ATAPI_SECTOR_SIZE);
    xenon_ata_queue_output(p, data, sizeof(data));
}

/*
 * Prepare an ATAPI READ TOC response.
 *
 * Purpose: provide a minimal TOC that satisfies early ODD probing code paths.
 */
static void xenon_ata_prepare_read_toc(XenonAtaPort *p)
{
    uint8_t data[20] = { 0 };
    uint32_t sectors = p->media_size ?
        (uint32_t)(p->media_size / XENON_ATAPI_SECTOR_SIZE) : 0;

    stw_be_p(data + 0, 0x0012);
    data[2] = 1;
    data[3] = 1;

    data[5] = 1;
    stl_be_p(data + 8, 0);

    data[13] = 0xAA;
    stl_be_p(data + 16, sectors);
    xenon_ata_queue_output(p, data, sizeof(data));
}

/*
 * Prepare an ATAPI MODE SENSE(10) response.
 *
 * Purpose: return a minimal mode page payload bounded by the allocation length.
 */
static void xenon_ata_prepare_mode_sense10(XenonAtaPort *p, uint16_t alloc_len)
{
    uint8_t data[64] = { 0 };
    uint16_t payload = 8;
    uint16_t out_len;

    stw_be_p(data + 0, payload);
    out_len = MIN(alloc_len, payload + 2U);
    xenon_ata_queue_output(p, data, out_len);
}

/*
 * Prepare an ATAPI REQUEST SENSE response.
 *
 * Purpose: provide a generic "no additional sense" style payload for stubs.
 */
static void xenon_ata_prepare_request_sense(XenonAtaPort *p, uint8_t alloc_len)
{
    uint8_t data[24] = { 0 };
    uint8_t out_len = MIN((uint8_t)sizeof(data), alloc_len ? alloc_len : (uint8_t)sizeof(data));

    data[0] = 0x70;
    data[7] = 0x0A;
    xenon_ata_queue_output(p, data, out_len);
}

/*
 * Prepare an ATAPI READ(10) transfer into the port output buffer.
 *
 * Purpose: stage 2048-byte sectors from the ODD image for PACKET READ10.
 */
static void xenon_ata_prepare_read10(XenonAtaPort *p, const uint8_t *packet)
{
    uint32_t lba = ldl_be_p(packet + 2);
    uint32_t blocks = lduw_be_p(packet + 7);
    size_t bytes;

    if (blocks == 0) {
        blocks = 0x10000;
    }

    bytes = (size_t)blocks * XENON_ATAPI_SECTOR_SIZE;
    g_byte_array_set_size(p->data_out, bytes);
    p->data_out_pos = 0;
    xenon_ata_read_media(p, (uint64_t)lba * XENON_ATAPI_SECTOR_SIZE,
                         p->data_out->data, bytes);
    p->status = xenon_ata_idle_status(p) | XENON_ATA_ST_DRQ;
}

/*
 * Handle a completed ATAPI PACKET command (SCSI opcode dispatch).
 *
 * Purpose: implement the subset of SCSI commands used by Xenon ODD probing and
 * disc reads (inquiry, read capacity, read toc, read10, etc.).
 */
static void xenon_ata_handle_scsi_packet(XenonAtaState *ata, XenonAtaPort *p)
{
    uint8_t opcode = p->packet[0];

    switch (opcode) {
    case XENON_SCSI_TEST_UNIT_READY:
        p->status = xenon_ata_idle_status(p);
        p->error = 0;
        break;
    case XENON_SCSI_REQUEST_SENSE:
        xenon_ata_prepare_request_sense(p, p->packet[4]);
        break;
    case XENON_SCSI_INQUIRY:
    {
        uint8_t alloc = p->packet[4] ? p->packet[4] : sizeof(ata->inquiry);
        xenon_ata_queue_output(p, ata->inquiry, MIN((size_t)alloc, sizeof(ata->inquiry)));
        break;
    }
    case XENON_SCSI_READ_CAPACITY:
        xenon_ata_prepare_read_capacity(p);
        break;
    case XENON_SCSI_READ10:
        xenon_ata_prepare_read10(p, p->packet);
        break;
    case XENON_SCSI_READ_TOC:
        xenon_ata_prepare_read_toc(p);
        break;
    case XENON_SCSI_MODE_SELECT10:
        p->expected_data_in = lduw_be_p(p->packet + 7);
        if (p->expected_data_in) {
            p->status = xenon_ata_idle_status(p) | XENON_ATA_ST_DRQ;
        } else {
            p->status = xenon_ata_idle_status(p);
        }
        break;
    case XENON_SCSI_MODE_SENSE10:
        xenon_ata_prepare_mode_sense10(p, lduw_be_p(p->packet + 7));
        break;
    case 0xE7:
    {
        uint8_t zeros[0x30] = { 0 };
        xenon_ata_queue_output(p, zeros, sizeof(zeros));
        break;
    }
    case 0xFF:
    {
        uint8_t zeros[0x10] = { 0 };
        xenon_ata_queue_output(p, zeros, sizeof(zeros));
        break;
    }
    default:
        p->error = XENON_ATA_ERR_ABRT;
        p->status = xenon_ata_idle_status(p) | XENON_ATA_ST_ERR;
        break;
    }
}

/*
 * Handle guest writes to the ATA data register.
 *
 * Purpose: accept ATAPI PACKET bytes and command payloads (eg MODE SELECT),
 * advancing the port state machine when the expected input is complete.
 */
static void xenon_ata_write_data(XenonAtaState *ata, XenonAtaPort *p,
                                 const uint8_t *bytes, unsigned size)
{
    if (p->waiting_for_packet) {
        unsigned copy = MIN(size, (unsigned)(12U - p->packet_filled));

        memcpy(p->packet + p->packet_filled, bytes, copy);
        p->packet_filled += copy;
        if (p->packet_filled >= 12U) {
            p->waiting_for_packet = false;
            xenon_ata_handle_scsi_packet(ata, p);
        }
        return;
    }

    if (p->expected_data_in > 0) {
        unsigned copy = MIN(size, p->expected_data_in);

        g_byte_array_append(p->data_in, bytes, copy);
        p->expected_data_in -= copy;
        if (p->expected_data_in == 0) {
            p->status = xenon_ata_idle_status(p);
            g_byte_array_set_size(p->data_in, 0);
        }
    }
}

/*
 * Handle guest reads from the ATA data register.
 *
 * Purpose: drain the queued output buffer and clear DRQ when all bytes have
 * been consumed.
 */
static void xenon_ata_read_data(XenonAtaPort *p, uint8_t *bytes, unsigned size)
{
    memset(bytes, 0, size);

    if (p->data_out_pos < p->data_out->len) {
        size_t remaining = p->data_out->len - p->data_out_pos;
        size_t copy = MIN((size_t)size, remaining);

        memcpy(bytes, p->data_out->data + p->data_out_pos, copy);
        p->data_out_pos += copy;
        if (p->data_out_pos >= p->data_out->len) {
            p->status &= ~XENON_ATA_ST_DRQ;
            p->data_out_pos = 0;
            g_byte_array_set_size(p->data_out, 0);
        }
    }
}

/*
 * Dispatch an ATA command written to the command register.
 *
 * Purpose: implement the small subset of ATA/ATAPI commands Xenon bring-up
 * code uses (IDENTIFY, PACKET, READ DMA EXT, SET FEATURES).
 */
static void xenon_ata_handle_command(XenonAtaPort *p, uint8_t cmd)
{
    p->command = cmd;
    p->status &= ~(XENON_ATA_ST_ERR | XENON_ATA_ST_BSY);

    switch (cmd) {
    case XENON_ATA_CMD_IDENTIFY_DEVICE:
        if (p->atapi) {
            p->error = XENON_ATA_ERR_ABRT;
            p->sector_count = 1;
            p->lba_low = 1;
            p->lba_mid = 0x14;
            p->lba_high = 0xEB;
            p->status = XENON_ATA_ST_ERR | xenon_ata_idle_status(p);
        } else {
            /*
             * Match xenon-emu/libxenon behavior: if no HDD image is attached,
             * IDENTIFY still returns a 512-byte payload (zeros) rather than an
             * immediate ERR status.
             */
            static const uint8_t zero_identify[512];

            p->error = 0;
            xenon_ata_queue_output(p, p->media_fd >= 0 ? p->identify : zero_identify,
                                   sizeof(p->identify));
        }
        break;
    case XENON_ATA_CMD_IDENTIFY_PACKET:
        if (p->atapi) {
            p->error = 0;
            xenon_ata_queue_output(p, p->identify, sizeof(p->identify));
        } else {
            p->error = XENON_ATA_ERR_ABRT;
            p->status = XENON_ATA_ST_ERR | xenon_ata_idle_status(p);
        }
        break;
    case XENON_ATA_CMD_PACKET:
        if (p->atapi) {
            p->error = 0;
            p->waiting_for_packet = true;
            p->packet_filled = 0;
            p->status = xenon_ata_idle_status(p) | XENON_ATA_ST_DRQ;
        } else {
            p->error = XENON_ATA_ERR_ABRT;
            p->status = XENON_ATA_ST_ERR | xenon_ata_idle_status(p);
        }
        break;
    case XENON_ATA_CMD_SET_FEATURES:
        p->error = 0;
        p->status = xenon_ata_idle_status(p);
        break;
    case XENON_ATA_CMD_READ_DMA_EXT:
        if (p->atapi || (!p->atapi && p->media_fd < 0)) {
            p->error = XENON_ATA_ERR_ABRT;
            p->status = XENON_ATA_ST_ERR | xenon_ata_idle_status(p);
        } else {
            p->error = 0;
            xenon_ata_prepare_read_dma_ext(p);
        }
        break;
    default:
        p->error = XENON_ATA_ERR_ABRT;
        p->status = XENON_ATA_ST_ERR | xenon_ata_idle_status(p);
        break;
    }
}

/*
 * Execute a BMDMA transfer described by the PRD table.
 *
 * Purpose: model libxenon-style SATA DMA reads by copying from the staged
 * data_out buffer into guest RAM and updating DMA status bits.
 */
static void xenon_ata_dma_exec(XenonAtaPort *p)
{
    uint32_t table = p->dma_table_raw & 0x7FFFFFFFU;
    bool read_operation = (p->dma_command & XENON_ATA_DMA_WR) != 0;
    uint32_t table_off = 0;

    p->dma_status = XENON_ATA_DMA_ACTIVE;

    for (unsigned i = 0; i < 256; i++) {
        uint8_t prd[8];
        uint32_t phys;
        uint32_t size_flags;
        uint32_t size;
        bool last;

        cpu_physical_memory_read((hwaddr)table + table_off, prd, sizeof(prd));
        phys = ldl_le_p(prd + 0);
        size_flags = ldl_le_p(prd + 4);
        size = size_flags & 0xFFFFU;
        last = (size_flags & 0x80000000U) != 0;
        if (size == 0) {
            size = 65536U;
        }

        if (read_operation) {
            size_t remain = 0;
            size_t copy = 0;

            if (p->data_out_pos < p->data_out->len) {
                remain = p->data_out->len - p->data_out_pos;
                copy = MIN((size_t)size, remain);
                cpu_physical_memory_write((hwaddr)phys,
                                          p->data_out->data + p->data_out_pos,
                                          copy);
                p->data_out_pos += copy;
            }
            if (copy < size) {
                g_autofree uint8_t *zeros = g_malloc0(size - copy);
                cpu_physical_memory_write((hwaddr)phys + copy, zeros, size - copy);
            }
        } else {
            g_byte_array_set_size(p->data_in, p->data_in->len + size);
            cpu_physical_memory_read((hwaddr)phys,
                                     p->data_in->data + (p->data_in->len - size),
                                     size);
        }

        table_off += 8;
        if (last) {
            break;
        }
    }

    p->dma_command &= ~XENON_ATA_DMA_ACTIVE;
    p->dma_status = XENON_ATA_DMA_INTR;
    p->status = xenon_ata_idle_status(p);
    p->data_out_pos = 0;
    g_byte_array_set_size(p->data_out, 0);
}

/*
 * Select the ODD vs HDD port based on the BAR offset and return the per-port
 * register offset.
 *
 * Purpose: model the Xenon SATA BAR layout where the HDD block is mapped at a
 * fixed +0x100 offset from the ODD block.
 */
static XenonAtaPort *xenon_ata_port_from_offset(XenonAtaState *ata, hwaddr offset,
                                                hwaddr *port_off)
{
    if ((offset & 0x100U) != 0) {
        *port_off = offset - XENON_SATA_HDD_BASE;
        return &ata->hdd;
    }

    *port_off = offset - XENON_SATA_ODD_BASE;
    return &ata->odd;
}

/*
 * Read a single 8-bit task-file/BMDMA/SATA register from a port.
 *
 * Purpose: central register dispatch used by the MMIO read handler.
 */
static uint8_t xenon_ata_read8(XenonAtaPort *p, hwaddr reg)
{
    switch (reg) {
    case XENON_ATA_REG_ERROR:
        return p->error;
    case XENON_ATA_REG_SECTOR_COUNT:
        return p->sector_count;
    case XENON_ATA_REG_LBA_LOW:
        return p->lba_low;
    case XENON_ATA_REG_LBA_MID:
        return p->lba_mid;
    case XENON_ATA_REG_LBA_HIGH:
        return p->lba_high;
    case XENON_ATA_REG_DEVICE:
        return p->device;
    case XENON_ATA_REG_STATUS:
    case XENON_ATA_REG_ALT_STATUS:
        return p->status;
    case XENON_ATA_REG_SSTATUS:
        return p->sstatus & 0xFF;
    case XENON_ATA_REG_SSTATUS + 1:
        return (p->sstatus >> 8) & 0xFF;
    case XENON_ATA_REG_SSTATUS + 2:
        return (p->sstatus >> 16) & 0xFF;
    case XENON_ATA_REG_SSTATUS + 3:
        return (p->sstatus >> 24) & 0xFF;
    case XENON_ATA_REG_SERROR:
        return p->serror & 0xFF;
    case XENON_ATA_REG_SERROR + 1:
        return (p->serror >> 8) & 0xFF;
    case XENON_ATA_REG_SERROR + 2:
        return (p->serror >> 16) & 0xFF;
    case XENON_ATA_REG_SERROR + 3:
        return (p->serror >> 24) & 0xFF;
    case XENON_ATA_REG_SCONTROL:
        return p->scontrol & 0xFF;
    case XENON_ATA_REG_SCONTROL + 1:
        return (p->scontrol >> 8) & 0xFF;
    case XENON_ATA_REG_SCONTROL + 2:
        return (p->scontrol >> 16) & 0xFF;
    case XENON_ATA_REG_SCONTROL + 3:
        return (p->scontrol >> 24) & 0xFF;
    case XENON_ATA_REG_SACTIVE:
        return p->sactive & 0xFF;
    case XENON_ATA_REG_SACTIVE + 1:
        return (p->sactive >> 8) & 0xFF;
    case XENON_ATA_REG_SACTIVE + 2:
        return (p->sactive >> 16) & 0xFF;
    case XENON_ATA_REG_SACTIVE + 3:
        return (p->sactive >> 24) & 0xFF;
    case XENON_ATA_REG2_CMD:
        return p->dma_command;
    case XENON_ATA_REG2_STATUS:
        return p->dma_status;
    case XENON_ATA_REG2_TABLE:
        return p->dma_table_raw & 0xFF;
    case XENON_ATA_REG2_TABLE + 1:
        return (p->dma_table_raw >> 8) & 0xFF;
    case XENON_ATA_REG2_TABLE + 2:
        return (p->dma_table_raw >> 16) & 0xFF;
    case XENON_ATA_REG2_TABLE + 3:
        return (p->dma_table_raw >> 24) & 0xFF;
    default:
        return 0;
    }
}

/*
 * Write a single 8-bit task-file/BMDMA/SATA register to a port.
 *
 * Purpose: central register dispatch used by the MMIO write handler, including
 * command submission and soft-reset semantics.
 */
static void xenon_ata_write8(XenonAtaPort *p, hwaddr reg, uint8_t val)
{
    switch (reg) {
    case XENON_ATA_REG_FEATURES:
        p->features = val;
        break;
    case XENON_ATA_REG_SECTOR_COUNT:
        p->prev_sector_count = p->sector_count;
        p->sector_count = val;
        break;
    case XENON_ATA_REG_LBA_LOW:
        if (!p->atapi && p->media_fd < 0) {
            break;
        }
        p->prev_lba_low = p->lba_low;
        p->lba_low = val;
        break;
    case XENON_ATA_REG_LBA_MID:
        p->prev_lba_mid = p->lba_mid;
        p->lba_mid = val;
        break;
    case XENON_ATA_REG_LBA_HIGH:
        p->prev_lba_high = p->lba_high;
        p->lba_high = val;
        break;
    case XENON_ATA_REG_DEVICE:
        p->device = val;
        break;
    case XENON_ATA_REG_COMMAND:
        xenon_ata_handle_command(p, val);
        break;
    case XENON_ATA_REG_DEV_CONTROL:
    {
        bool old_srst = (p->device_control & 0x04U) != 0;
        bool new_srst;

        p->device_control = val;
        new_srst = (val & 0x04U) != 0;
        if (old_srst && !new_srst) {
            xenon_ata_port_soft_reset(p);
        }
        break;
    }
    case XENON_ATA_REG_SSTATUS:
    case XENON_ATA_REG_SSTATUS + 1:
    case XENON_ATA_REG_SSTATUS + 2:
    case XENON_ATA_REG_SSTATUS + 3:
    {
        unsigned shift = (unsigned)(reg - XENON_ATA_REG_SSTATUS) * 8U;
        p->sstatus = (p->sstatus & ~(0xFFU << shift)) | ((uint32_t)val << shift);
        break;
    }
    case XENON_ATA_REG_SERROR:
    case XENON_ATA_REG_SERROR + 1:
    case XENON_ATA_REG_SERROR + 2:
    case XENON_ATA_REG_SERROR + 3:
    {
        unsigned shift = (unsigned)(reg - XENON_ATA_REG_SERROR) * 8U;
        p->serror = (p->serror & ~(0xFFU << shift)) | ((uint32_t)val << shift);
        break;
    }
    case XENON_ATA_REG_SCONTROL:
    case XENON_ATA_REG_SCONTROL + 1:
    case XENON_ATA_REG_SCONTROL + 2:
    case XENON_ATA_REG_SCONTROL + 3:
    {
        unsigned shift = (unsigned)(reg - XENON_ATA_REG_SCONTROL) * 8U;
        p->scontrol = (p->scontrol & ~(0xFFU << shift)) | ((uint32_t)val << shift);
        break;
    }
    case XENON_ATA_REG_SACTIVE:
    case XENON_ATA_REG_SACTIVE + 1:
    case XENON_ATA_REG_SACTIVE + 2:
    case XENON_ATA_REG_SACTIVE + 3:
    {
        unsigned shift = (unsigned)(reg - XENON_ATA_REG_SACTIVE) * 8U;
        p->sactive = (p->sactive & ~(0xFFU << shift)) | ((uint32_t)val << shift);
        break;
    }
    case XENON_ATA_REG2_CMD:
    {
        bool old_srst = (p->device_control & 0x04U) != 0;
        bool new_srst;

        p->device_control = val;
        p->dma_command = val;
        new_srst = (val & 0x04U) != 0;
        if (old_srst && !new_srst) {
            xenon_ata_port_soft_reset(p);
        }
        if (val & XENON_ATA_DMA_ACTIVE) {
            xenon_ata_dma_exec(p);
        }
        break;
    }
    case XENON_ATA_REG2_STATUS:
        p->dma_status = val;
        break;
    case XENON_ATA_REG2_TABLE:
        p->dma_table_raw = (p->dma_table_raw & 0xFFFFFF00U) | val;
        break;
    case XENON_ATA_REG2_TABLE + 1:
        p->dma_table_raw = (p->dma_table_raw & 0xFFFF00FFU) | ((uint32_t)val << 8);
        break;
    case XENON_ATA_REG2_TABLE + 2:
        p->dma_table_raw = (p->dma_table_raw & 0xFF00FFFFU) | ((uint32_t)val << 16);
        break;
    case XENON_ATA_REG2_TABLE + 3:
        p->dma_table_raw = (p->dma_table_raw & 0x00FFFFFFU) | ((uint32_t)val << 24);
        break;
    default:
        break;
    }
}

/*
 * SATA/ATA MMIO read handler for the Xenon SATA BAR.
 *
 * Purpose: provide the guest-visible register interface for both ODD and HDD
 * ports, including special handling for the DATA register FIFO.
 */
static uint64_t xenon_sata_read(void *opaque, hwaddr offset, unsigned size)
{
    XenonMachineState *xms = opaque;
    XenonAtaState *ata = xms->ata;
    XenonAtaPort *p;
    hwaddr reg;
    uint8_t tmp[8] = { 0 };

    if (!ata || offset >= XENON_SATA_MMIO_SIZE || size == 0 || size > sizeof(tmp)) {
        return 0;
    }

    p = xenon_ata_port_from_offset(ata, offset, &reg);

    if (reg == XENON_ATA_REG_DATA) {
        xenon_ata_read_data(p, tmp, size);
    } else {
        for (unsigned i = 0; i < size; i++) {
            tmp[i] = xenon_ata_read8(p, reg + i);
        }
    }

    if (xms->trace_boot && ata->trace_reads < XENON_ATA_TRACE_LIMIT) {
        info_report("xbox360: sata-read off=0x%03" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    (uint64_t)offset, size, xenon_ata_pack_be(tmp, size));
        ata->trace_reads++;
    }

    return xenon_ata_pack_be(tmp, size);
}

/*
 * SATA/ATA MMIO write handler for the Xenon SATA BAR.
 *
 * Purpose: accept guest register writes, route DATA writes into the port state
 * machine, and trigger commands/DMA as needed.
 */
static void xenon_sata_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    XenonMachineState *xms = opaque;
    XenonAtaState *ata = xms->ata;
    XenonAtaPort *p;
    hwaddr reg;
    uint8_t tmp[8];

    if (!ata || offset >= XENON_SATA_MMIO_SIZE || size == 0 || size > sizeof(tmp)) {
        return;
    }

    p = xenon_ata_port_from_offset(ata, offset, &reg);
    xenon_ata_unpack_be(tmp, size, data);

    if (xms->trace_boot && ata->trace_writes < XENON_ATA_TRACE_LIMIT) {
        info_report("xbox360: sata-write off=0x%03" PRIx64
                    " size=%u val=0x%016" PRIx64,
                    (uint64_t)offset, size, data);
        ata->trace_writes++;
    }

    if (reg == XENON_ATA_REG_DATA) {
        xenon_ata_write_data(ata, p, tmp, size);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        xenon_ata_write8(p, reg + i, tmp[i]);
    }
}

const MemoryRegionOps xenon_sata_ops = {
    .read = xenon_sata_read,
    .write = xenon_sata_write,
    .endianness = DEVICE_BIG_ENDIAN,
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

/*
 * Allocate and initialize the Xenon ATA state (ODD + HDD ports).
 *
 * Purpose: set up port state machines, open backing images, seed IDENTIFY and
 * INQUIRY payloads, and reset ports to an initial ready state.
 */
void xenon_ata_state_init(XenonMachineState *xms)
{
    XenonAtaState *ata;

    if (xms->ata) {
        xenon_ata_state_destroy(xms);
    }

    ata = g_new0(XenonAtaState, 1);
    xms->ata = ata;

    ata->odd.atapi = true;
    ata->odd.data_in = g_byte_array_new();
    ata->odd.data_out = g_byte_array_new();
    ata->odd.media_fd = xenon_ata_open_image(xms->odd_image_path, &ata->odd.media_size);
    ata->odd.media_present = ata->odd.media_fd >= 0;
    ata->odd.sstatus = 0x00000113U;
    ata->odd.serror = 0x001F0201U;
    ata->odd.scontrol = 0x00000300U;
    ata->odd.sactive = 0x00000040U;
    xenon_ata_build_odd_identify(&ata->odd);
    xenon_ata_port_soft_reset(&ata->odd);

    ata->hdd.atapi = false;
    ata->hdd.data_in = g_byte_array_new();
    ata->hdd.data_out = g_byte_array_new();
    ata->hdd.media_fd = xenon_ata_open_image(xms->hdd_image_path, &ata->hdd.media_size);
    ata->hdd.media_present = ata->hdd.media_fd >= 0;
    ata->hdd.sstatus = ata->hdd.media_present ? 0x00000113U : 0x00000000U;
    ata->hdd.serror = 0x001D0003U;
    ata->hdd.scontrol = 0x00000300U;
    ata->hdd.sactive = 0x00000040U;
    xenon_ata_build_hdd_identify(&ata->hdd);
    xenon_ata_port_soft_reset(&ata->hdd);

    xenon_ata_build_inquiry(ata);
}

/*
 * Destroy the ATA state and close backing images.
 *
 * Purpose: clean up resources allocated in xenon_ata_state_init() when the
 * machine is finalized or reconfigured.
 */
void xenon_ata_state_destroy(XenonMachineState *xms)
{
    XenonAtaState *ata = xms->ata;

    if (!ata) {
        return;
    }

    if (ata->odd.media_fd >= 0) {
        close(ata->odd.media_fd);
    }
    if (ata->hdd.media_fd >= 0) {
        close(ata->hdd.media_fd);
    }

    if (ata->odd.data_in) {
        g_byte_array_unref(ata->odd.data_in);
    }
    if (ata->odd.data_out) {
        g_byte_array_unref(ata->odd.data_out);
    }
    if (ata->hdd.data_in) {
        g_byte_array_unref(ata->hdd.data_in);
    }
    if (ata->hdd.data_out) {
        g_byte_array_unref(ata->hdd.data_out);
    }

    g_free(ata);
    xms->ata = NULL;
}

/*
 * Seed PCI config fields for the ODD SATA function.
 *
 * Purpose: provide Xenon-like PCI enumeration information and expose the
 * port's media-present dependent link/config fields.
 */
static void xenon_ata_init_odd_pci_config(XenonMachineState *xms)
{
    XenonAtaState *ata = xms->ata;
    uint32_t cfg = ata->odd.media_present ? 0x00000113U : 0x00000000U;

    stl_le_p(xms->pci_cfg_data + 0x108000, 0x58021414U);
    stl_le_p(xms->pci_cfg_data + 0x108004, 0x02300006U);
    stl_le_p(xms->pci_cfg_data + 0x108008, 0x01060000U);
    stl_le_p(xms->pci_cfg_data + 0x108010, 0xEA001200U);
    stl_le_p(xms->pci_cfg_data + 0x108014, 0xEA001220U);
    stl_le_p(xms->pci_cfg_data + 0x108034, 0x00000058U);
    stl_le_p(xms->pci_cfg_data + 0x10803C, 0x00000100U);

    stl_le_p(xms->pci_cfg_data + 0x108058, 0x80020001U);
    stl_le_p(xms->pci_cfg_data + 0x108060, 0x00112400U);
    stl_le_p(xms->pci_cfg_data + 0x108070, 0x7F7F7F7FU);
    stl_le_p(xms->pci_cfg_data + 0x108074, 0x7F7F7F7FU);
    stl_le_p(xms->pci_cfg_data + 0x108080, 0xC07231BEU);
    stl_le_p(xms->pci_cfg_data + 0x108098, 0x100C04CCU);
    stl_le_p(xms->pci_cfg_data + 0x10809C, 0x004108C0U);

    stl_le_p(xms->pci_cfg_data + 0x1080C0, cfg);
    stl_le_p(xms->pci_cfg_data + 0x1080C4, 0x001F0201U);
    stl_le_p(xms->pci_cfg_data + 0x1080C8, 0x00000300U);
    stl_le_p(xms->pci_cfg_data + 0x1080CC, 0x00000040U);
}

/*
 * Seed PCI config fields for the HDD SATA function.
 *
 * Purpose: provide Xenon-like PCI enumeration information and expose the
 * port's media-present dependent link/config fields.
 */
static void xenon_ata_init_hdd_pci_config(XenonMachineState *xms)
{
    XenonAtaState *ata = xms->ata;
    uint32_t cfg = ata->hdd.media_present ? 0x00000113U : 0x00000000U;

    stl_le_p(xms->pci_cfg_data + 0x110000, 0x58031414U);
    stl_le_p(xms->pci_cfg_data + 0x110004, 0x02300006U);
    stl_le_p(xms->pci_cfg_data + 0x110008, 0x01060000U);
    stl_le_p(xms->pci_cfg_data + 0x110010, 0xEA001300U);
    stl_le_p(xms->pci_cfg_data + 0x110014, 0xEA001320U);
    stl_le_p(xms->pci_cfg_data + 0x110034, 0x00000058U);
    stl_le_p(xms->pci_cfg_data + 0x11003C, 0x00000100U);

    stl_le_p(xms->pci_cfg_data + 0x110058, 0x80020001U);
    stl_le_p(xms->pci_cfg_data + 0x110060, 0x00112400U);
    stl_le_p(xms->pci_cfg_data + 0x110070, 0x7F7F7F7FU);
    stl_le_p(xms->pci_cfg_data + 0x110074, 0x7F7F7F7FU);
    stl_le_p(xms->pci_cfg_data + 0x110080, 0xC07231BEU);
    stl_le_p(xms->pci_cfg_data + 0x110090, 0x00000040U);
    stl_le_p(xms->pci_cfg_data + 0x110098, 0x100C04CCU);
    stl_le_p(xms->pci_cfg_data + 0x11009C, 0x004108C0U);

    stl_le_p(xms->pci_cfg_data + 0x1100C0, cfg);
    stl_le_p(xms->pci_cfg_data + 0x1100C4, 0x001D0003U);
    stl_le_p(xms->pci_cfg_data + 0x1100C8, 0x00000300U);
    stl_le_p(xms->pci_cfg_data + 0x1100CC, 0x00000040U);
}

/*
 * Initialize the SATA-related PCI config shadow for both ODD and HDD functions.
 *
 * Purpose: called during machine init so guest PCI enumeration sees SATA
 * functions with plausible config space.
 */
void xenon_ata_init_pci_config(XenonMachineState *xms)
{
    if (!xms->ata || !xms->pci_cfg_data) {
        return;
    }

    xenon_ata_init_odd_pci_config(xms);
    xenon_ata_init_hdd_pci_config(xms);
}
