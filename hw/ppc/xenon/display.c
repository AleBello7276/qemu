/*
 * Xbox 360 Xenon debug display helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/ppc/xenon/xenon-internal.h"
#include "hw/ppc/xenon/xgpu.h"
#include "hw/ppc/xenon/debug.h"
#include "system/address-spaces.h"
#include "ui/pixel_ops.h"

/*
 * Display invalidation callback for the debug console surface.
 *
 * Purpose: force a full refresh when the console requests it (used by the
 * simple "debug framebuffer" path).
 */
static void xenon_dbg_invalidate_display(void *opaque)
{
    XenonMachineState *xms = opaque;

    if (xms->dbg_con) {
        dpy_gfx_update_full(xms->dbg_con);
    }
}

/*
 * LibXenon/XeLL console uses the native Xenos 32x32 tiled BGRA surface.
 * Reuse the same addressing to present guest text output directly.
 *
 * Purpose: compute the guest's tiled framebuffer index so we can blit scanout
 * pixels correctly without implementing a full GPU tiling unit.
 */
static inline uint32_t xenon_dbg_fb_tiled_index(uint32_t x, uint32_t y,
                                                uint32_t tiled_width)
{
    return (((y >> 5) * 32 * tiled_width + ((x >> 5) << 10) +
            (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) +
            (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2));
}

/*
 * Validate XGPU framebuffer parameters and derive readout geometry.
 *
 * Purpose: keep the debug scanout path bounded/safe and compute the expected
 * byte size for tiled vs linear surfaces.
 */
static bool xenon_dbg_fb_prepare(const XenonXgpuFbInfo *fb,
                                 uint32_t *tiled_width,
                                 size_t *fb_bytes)
{
    uint32_t pitch;
    uint32_t tw;
    uint32_t th;
    size_t bytes;

    if (!fb->enabled || fb->width == 0 || fb->height == 0) {
        return false;
    }
    if (fb->width > 1920U || fb->height > 1200U) {
        return false;
    }

    pitch = fb->pitch ? fb->pitch : fb->width;
    if (pitch < fb->width) {
        pitch = fb->width;
    }

    if (fb->tiled) {
        tw = (pitch + 31U) & ~31U;
        th = (fb->height + 31U) & ~31U;
    } else {
        tw = pitch;
        th = fb->height;
    }
    if (tw == 0 || th == 0) {
        return false;
    }
    bytes = (size_t)tw * (size_t)th * sizeof(uint32_t);
    if (bytes == 0 || bytes > (size_t)(32 * MiB)) {
        return false;
    }

    *tiled_width = tw;
    *fb_bytes = bytes;
    return true;
}

/*
 * Periodic update callback for the Xenon debug display.
 *
 * Purpose: mirror the guest XGPU scanout surface into a QEMU DisplaySurface so
 * Xenon homebrew/XeLL text output can be viewed during bring-up.
 */
void xenon_dbg_update_display(void *opaque)
{
    XenonMachineState *xms = opaque;
    XenonXgpuFbInfo fb = { 0 };
    DisplaySurface *surface;
    uint32_t tiled_width;
    size_t fb_bytes;
    uint8_t *dst;
    uint32_t bg;
    int bpp;
    hwaddr fb_pa;

    if (!xms->dbg_con) {
        return;
    }

    surface = qemu_console_surface(xms->dbg_con);
    if (!surface || surface_bits_per_pixel(surface) == 0) {
        return;
    }

    xenon_xgpu_get_fb_info(xms, &fb);
    if (!xenon_dbg_fb_prepare(&fb, &tiled_width, &fb_bytes)) {
        bg = rgb_to_pixel32(0x00, 0x00, 0x00);
        bpp = (surface_bits_per_pixel(surface) + 7) >> 3;
        for (int y = 0; y < surface_height(surface); y++) {
            dst = surface_data(surface) + y * surface_stride(surface);
            for (int x = 0; x < surface_width(surface); x++) {
                switch (bpp) {
                case 4:
                    ((uint32_t *)dst)[x] = bg;
                    break;
                case 2:
                    ((uint16_t *)dst)[x] = rgb_to_pixel16(0, 0, 0);
                    break;
                case 1:
                    dst[x] = rgb_to_pixel8(0, 0, 0);
                    break;
                default:
                    break;
                }
            }
        }

        if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_XGPU) && xms->dbg_fb_enabled) {
            info_report("xbox360: xgpu scanout disabled");
        }
        xms->dbg_fb_enabled = false;
        dpy_gfx_update_full(xms->dbg_con);
        return;
    }

    if (!xms->dbg_fb_enabled ||
        xms->dbg_fb_width != fb.width ||
        xms->dbg_fb_height != fb.height) {
        qemu_console_resize(xms->dbg_con, fb.width, fb.height);
        surface = qemu_console_surface(xms->dbg_con);
        if (!surface || surface_bits_per_pixel(surface) == 0) {
            return;
        }
    }

    if (xms->dbg_fb_shadow_size < fb_bytes) {
        xms->dbg_fb_shadow = g_realloc(xms->dbg_fb_shadow, fb_bytes);
        xms->dbg_fb_shadow_size = fb_bytes;
    }

    fb_pa = fb.base & 0x3FFFFFFFU;
    address_space_read(&address_space_memory, fb_pa, MEMTXATTRS_UNSPECIFIED,
                       xms->dbg_fb_shadow, fb_bytes);

    bpp = (surface_bits_per_pixel(surface) + 7) >> 3;
    for (uint32_t y = 0; y < fb.height; y++) {
        dst = surface_data(surface) + (int)y * surface_stride(surface);
        for (uint32_t x = 0; x < fb.width; x++) {
            uint32_t pixel_index = fb.tiled ?
                xenon_dbg_fb_tiled_index(x, y, tiled_width) :
                (y * fb.pitch + x);
            uint32_t guest_pixel = ldl_be_p(xms->dbg_fb_shadow +
                                            ((size_t)pixel_index * sizeof(uint32_t)));
            uint8_t b = (guest_pixel >> 24) & 0xFF;
            uint8_t g = (guest_pixel >> 16) & 0xFF;
            uint8_t r = (guest_pixel >> 8) & 0xFF;
            uint32_t color = rgb_to_pixel32(r, g, b);

            switch (bpp) {
            case 4:
                ((uint32_t *)dst)[x] = color;
                break;
            case 2:
                ((uint16_t *)dst)[x] = rgb_to_pixel16(r, g, b);
                break;
            case 1:
                dst[x] = rgb_to_pixel8(r, g, b);
                break;
            default:
                break;
            }
        }
    }

    if (xenon_log_enabled(xms, XENON_LOG_LEVEL_INFO, XENON_LOG_MODULE_XGPU) &&
        (!xms->dbg_fb_enabled ||
         xms->dbg_fb_base != fb.base ||
         xms->dbg_fb_pitch != fb.pitch ||
         xms->dbg_fb_width != fb.width ||
         xms->dbg_fb_height != fb.height)) {
        info_report("xbox360: xgpu scanout base=0x%08" PRIx32
                    " pitch=%u size=%ux%u layout=%s",
                    fb.base, fb.pitch, fb.width, fb.height,
                    fb.tiled ? "tiled" : "linear");
    }

    xms->dbg_fb_enabled = true;
    xms->dbg_fb_base = fb.base;
    xms->dbg_fb_pitch = fb.pitch;
    xms->dbg_fb_width = fb.width;
    xms->dbg_fb_height = fb.height;
    dpy_gfx_update_full(xms->dbg_con);
}

const GraphicHwOps xenon_dbg_display_ops = {
    .invalidate = xenon_dbg_invalidate_display,
    .gfx_update = xenon_dbg_update_display,
};
