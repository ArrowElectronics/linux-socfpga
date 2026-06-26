// SPDX-License-Identifier: GPL-2.0
/*
 * altera_vfr_drm.c - DRM driver for the Altera VVP Video Frame Reader
 *
 * The VFR IP core reads pixel data from a Linux CMA-allocated framebuffer
 * in system memory via an AXI DMA read master and streams it to a fixed
 * 1080p60 HDMI output pipeline.
 *
 * Hardware programming sequence (from intel_vvp_vfr.c):
 *
 *   First enable  — vfr_program_bufset():
 *     1. Write RUN = STOP
 *     2. Write buffer set 0 descriptor (BASE_ADDR, dimensions, format)
 *     3. Write NUM_BUFFER_SETS, BUFFER_MODE, STARTING_BUFSET
 *     4. Write COMMIT = 1
 *     5. Write RUN = FREE_RUNNING
 *
 *   Page flip  — vfr_update_base_addr():
 *     1. Write BUFSET[0].BASE_ADDR = new CMA physical address
 *     2. Write COMMIT = 1  (latches at next frame boundary)
 *
 * No interrupt is used. The driver uses a no_vblank CRTC with immediate
 * event delivery in atomic_flush, which is correct for the single
 * framebuffer / fbdev emulation use case.
 *
 * Copyright (C) 2024 - DRM driver for Altera VFR
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "altera_vfr_drm.h"
#include "altera_vfr_regs.h"

/* -----------------------------------------------------------------------
 * Register accessors
 * altera_vfr_regs.h provides byte offsets; all registers are 32-bit words.
 * ----------------------------------------------------------------------- */
static inline void vfr_write(struct altera_vfr *vfr, u32 reg, u32 val)
{
	iowrite32(val, vfr->regs + reg);
}

static inline u32 vfr_read(struct altera_vfr *vfr, u32 reg)
{
	return ioread32(vfr->regs + reg);
}

/* -----------------------------------------------------------------------
 * Hardware control helpers  (mirror intel_vvp_vfr.c API)
 * ----------------------------------------------------------------------- */

/* intel_vvp_vfr_set_run_mode(instance, STOP) */
static void vfr_stop(struct altera_vfr *vfr)
{
	vfr_write(vfr, VFR_REG_RUN, VFR_RUN_STOP);
}

/* intel_vvp_vfr_set_run_mode(instance, FREE_RUNNING) */
static void vfr_start(struct altera_vfr *vfr)
{
	vfr_write(vfr, VFR_REG_RUN, VFR_RUN_FREE_RUNNING);
}

/* intel_vvp_vfr_commit_writes(instance) */
static void vfr_commit(struct altera_vfr *vfr)
{
	vfr_write(vfr, VFR_REG_COMMIT, 1);
}

/* -----------------------------------------------------------------------
 * Full buffer-set 0 initialisation  — mirrors the embedded driver sequence.
 *
 * stride: inter-line offset in bytes.
 *   XRGB8888 → VFR_FIXED_STRIDE (width * 4)
 *   NV12     → VFR_FIXED_WIDTH  (luma stride; chroma follows contiguously)
 *   YUV420   → VFR_FIXED_WIDTH
 * ----------------------------------------------------------------------- */
static void vfr_program_bufset(struct altera_vfr *vfr, dma_addr_t paddr,
			       u32 stride)
{
	vfr_stop(vfr);

	/* Buffer set 0 descriptor */
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_BASE_ADDR),      (u32)paddr);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_NUM_BUFFERS),    1);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_INTER_BUF_OFFS), 0);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_INTER_LINE_OFFS),stride);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_WIDTH),
		  (stride == VFR_FIXED_STRIDE) ? VFR_FIXED_VFR_WIDTH : VFR_FIXED_WIDTH);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_HEIGHT),         VFR_FIXED_HEIGHT);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_INTERLACE),      VFR_PROGRESSIVE);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_COLORSPACE),     VFR_COLORSPACE_RGB);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_SUBSAMPLING),    VFR_SUBSAMPLING_444);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_COSITING),       VFR_COSITING_NONE);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_BPS),            VFR_FIXED_BPS);
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_FIELD_COUNT),    0); /* continuous */

	/* Global buffer control */
	vfr_write(vfr, VFR_REG_NUM_BUFFER_SETS, 1);
	vfr_write(vfr, VFR_REG_BUFFER_MODE,     VFR_BUF_MODE_SINGLE_SET);
	vfr_write(vfr, VFR_REG_STARTING_BUFSET, 0);

	vfr_commit(vfr);
	vfr_start(vfr);

	dev_dbg(vfr->dev, "VFR started: paddr=0x%08llx stride=%u\n",
		(unsigned long long)paddr, stride);
}

/* -----------------------------------------------------------------------
 * Page-flip address update  — mirrors:
 *   intel_vvp_vfr_set_bufset_base_addr(instance, 0, paddr)
 *   intel_vvp_vfr_commit_writes(instance)
 *
 * The VFR keeps running; the new address is latched at the next frame
 * boundary without interrupting DMA output.
 * ----------------------------------------------------------------------- */
static void vfr_update_base_addr(struct altera_vfr *vfr, dma_addr_t paddr)
{
	vfr_write(vfr, VFR_BUFSET_REG(0, VFR_BUFSET_BASE_ADDR), (u32)paddr);
	vfr_commit(vfr);
}

/* -----------------------------------------------------------------------
 * Primary plane
 *
 * Supported formats:
 *   XRGB8888 — compositor default (Labwc/fbcon)
 *   NV12     — Y + interleaved UV, native avdec_h264 output
 *   YUV420   — Y + planar U + V, also common decoder output
 * ----------------------------------------------------------------------- */
static const u32 altera_vfr_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_NV12,
	DRM_FORMAT_YUV420,
};

static void altera_vfr_plane_atomic_update(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct altera_vfr *vfr = plane_to_vfr(plane);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_gem_dma_object *gem;
	dma_addr_t paddr;
	u32 stride, fb_size;

	if (!fb)
		return;

	gem   = drm_fb_dma_get_gem_obj(fb, 0);
	paddr = gem->dma_addr + fb->offsets[0];

	/*
	 * Compute stride and total buffer size for cache flush.
	 * NV12/YUV420: luma stride = width (1 byte/pixel),
	 *              total = width * height * 3/2.
	 * XRGB8888:   stride = width * 4, total = stride * height.
	 */
	if (fb->format->format == DRM_FORMAT_NV12 ||
	    fb->format->format == DRM_FORMAT_YUV420) {
		stride  = VFR_FIXED_WIDTH;
		fb_size = VFR_FIXED_WIDTH * VFR_FIXED_HEIGHT * 3 / 2;
	} else {
		stride  = VFR_FIXED_STRIDE;
		fb_size = VFR_FIXED_STRIDE * VFR_FIXED_HEIGHT;
	}

	/*
	 * Flush CPU cache to DDR so the VFR DMA master sees current
	 * framebuffer content. Required because the CMA buffer is
	 * allocated cached and the VFR AXI master bypasses the cache.
	 */
	dma_sync_single_for_device(vfr->dev, paddr, fb_size, DMA_TO_DEVICE);

	if (!vfr->running) {
		/* First enable: program full descriptor and start DMA */
		vfr_program_bufset(vfr, paddr, stride);
		vfr->running = true;
	} else {
		/* Page flip: address-only update, hardware keeps running */
		vfr_update_base_addr(vfr, paddr);
	}
}

static const struct drm_plane_helper_funcs altera_vfr_plane_helper_funcs = {
	.atomic_update = altera_vfr_plane_atomic_update,
};

static const struct drm_plane_funcs altera_vfr_plane_funcs = {
	.update_plane           = drm_atomic_helper_update_plane,
	.disable_plane          = drm_atomic_helper_disable_plane,
	.destroy                = drm_plane_cleanup,
	.reset                  = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
};

/* -----------------------------------------------------------------------
 * CRTC
 * ----------------------------------------------------------------------- */

/*
 * Custom reset allocates the CRTC state with no_vblank = true so that
 * the DRM core completes commits without waiting for a hardware vblank.
 * Flip events are delivered immediately in atomic_flush.
 */
static void altera_vfr_crtc_reset(struct drm_crtc *crtc)
{
	struct drm_crtc_state *state;

	if (crtc->state) {
		__drm_atomic_helper_crtc_destroy_state(crtc->state);
		kfree(crtc->state);
		crtc->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, state);
	state->no_vblank = true;
}

static void altera_vfr_crtc_atomic_enable(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
}

static void altera_vfr_crtc_atomic_disable(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
	struct altera_vfr *vfr = crtc_to_vfr(crtc);

	vfr_stop(vfr);
	vfr->running = false;
}

static void altera_vfr_crtc_atomic_flush(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	unsigned long flags;

	if (!new_crtc_state->event)
		return;

	/*
	 * Send the flip event immediately. With no_vblank=true and
	 * fake_vblank in the commit tail there is no real vblank to
	 * wait for, so deliver directly under the event lock.
	 */
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, new_crtc_state->event);
	new_crtc_state->event = NULL;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs altera_vfr_crtc_helper_funcs = {
	.atomic_enable  = altera_vfr_crtc_atomic_enable,
	.atomic_disable = altera_vfr_crtc_atomic_disable,
	.atomic_flush   = altera_vfr_crtc_atomic_flush,
};

static const struct drm_crtc_funcs altera_vfr_crtc_funcs = {
	.reset                  = altera_vfr_crtc_reset,
	.destroy                = drm_crtc_cleanup,
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,
};

/*
 * Custom commit tail — fully non-blocking.
 * Events are delivered immediately in atomic_flush; nothing to wait for.
 */
static void altera_vfr_commit_tail(struct drm_atomic_state *state)
{
	drm_atomic_helper_commit_modeset_disables(state->dev, state);
	drm_atomic_helper_commit_planes(state->dev, state, 0);
	drm_atomic_helper_commit_modeset_enables(state->dev, state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(state->dev, state);
}

/* -----------------------------------------------------------------------
 * Encoder — pass-through
 * ----------------------------------------------------------------------- */
static const struct drm_encoder_funcs altera_vfr_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* -----------------------------------------------------------------------
 * Connector — fixed 1080p60, always connected
 * ----------------------------------------------------------------------- */
static const struct drm_display_mode altera_vfr_1080p_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		 148500,
		 1920, 2008, 2052, 2200, 0,
		 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
};

static int altera_vfr_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &altera_vfr_1080p_mode);
	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_mode_status
altera_vfr_connector_mode_valid(struct drm_connector *connector,
                                const struct drm_display_mode *mode)
{
	if (!drm_mode_equal(mode, &altera_vfr_1080p_mode))
		return MODE_ONE_SIZE;

	return MODE_OK;
}

static const struct drm_connector_helper_funcs altera_vfr_connector_helper_funcs = {
	.get_modes  = altera_vfr_connector_get_modes,
	.mode_valid = altera_vfr_connector_mode_valid,
};

static const struct drm_connector_funcs altera_vfr_connector_funcs = {
	.reset                  = drm_atomic_helper_connector_reset,
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

/* -----------------------------------------------------------------------
 * DRM driver descriptor
 * ----------------------------------------------------------------------- */
DEFINE_DRM_GEM_DMA_FOPS(altera_vfr_fops);

static const struct drm_driver altera_vfr_drm_driver = {
	.driver_features        = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops                   = &altera_vfr_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.fbdev_probe            = drm_fbdev_dma_driver_fbdev_probe,
	.name                   = "altera-vfr",
	.desc                   = "Altera VVP Video Frame Reader DRM driver",
	.major                  = 1,
	.minor                  = 0,
};

/* -----------------------------------------------------------------------
 * Mode config
 * ----------------------------------------------------------------------- */
static const struct drm_mode_config_funcs altera_vfr_mode_config_funcs = {
	.fb_create     = drm_gem_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs altera_vfr_mode_config_helper_funcs = {
	.atomic_commit_tail = altera_vfr_commit_tail,
};

/* -----------------------------------------------------------------------
 * KMS object initialisation
 * ----------------------------------------------------------------------- */
static int altera_vfr_kms_init(struct altera_vfr *vfr)
{
	struct drm_device *drm = &vfr->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width  = VFR_FIXED_WIDTH - 1;
	drm->mode_config.max_width  = VFR_FIXED_WIDTH;
	drm->mode_config.min_height = VFR_FIXED_HEIGHT - 1;
	drm->mode_config.max_height = VFR_FIXED_HEIGHT;
	drm->mode_config.funcs      = &altera_vfr_mode_config_funcs;
	drm->mode_config.helper_private = &altera_vfr_mode_config_helper_funcs;

	/* Primary plane */
	ret = drm_universal_plane_init(drm, &vfr->primary, 0,
				       &altera_vfr_plane_funcs,
				       altera_vfr_formats,
				       ARRAY_SIZE(altera_vfr_formats),
				       NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err(vfr->dev, "plane init failed: %d\n", ret);
		return ret;
	}
	drm_plane_helper_add(&vfr->primary, &altera_vfr_plane_helper_funcs);

	/* CRTC */
	ret = drm_crtc_init_with_planes(drm, &vfr->crtc, &vfr->primary, NULL,
					&altera_vfr_crtc_funcs, NULL);
	if (ret) {
		dev_err(vfr->dev, "CRTC init failed: %d\n", ret);
		return ret;
	}
	drm_crtc_helper_add(&vfr->crtc, &altera_vfr_crtc_helper_funcs);

	/* Encoder */
	ret = drm_encoder_init(drm, &vfr->encoder, &altera_vfr_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret) {
		dev_err(vfr->dev, "encoder init failed: %d\n", ret);
		return ret;
	}
	vfr->encoder.possible_crtcs = drm_crtc_mask(&vfr->crtc);

	/* Connector */
	ret = drm_connector_init(drm, &vfr->connector,
				 &altera_vfr_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		dev_err(vfr->dev, "connector init failed: %d\n", ret);
		return ret;
	}
	drm_connector_helper_add(&vfr->connector,
				 &altera_vfr_connector_helper_funcs);
	vfr->connector.status = connector_status_connected;
	drm_connector_attach_encoder(&vfr->connector, &vfr->encoder);

	return 0;
}

/* -----------------------------------------------------------------------
 * CVO initialisation
 * ----------------------------------------------------------------------- */
static void altera_vfr_cvo_init(struct altera_vfr *vfr)
{
	iowrite32(VFR_FIXED_WIDTH,  vfr->cvo_regs + CVO_REG_IMG_INFO_WIDTH);
	iowrite32(VFR_FIXED_HEIGHT, vfr->cvo_regs + CVO_REG_IMG_INFO_HEIGHT);
	iowrite32(0,                vfr->cvo_regs + CVO_REG_IMG_INFO_INTERLACE);
	iowrite32(0,                vfr->cvo_regs + CVO_REG_IMG_INFO_COLORSPACE);
	iowrite32(0,                vfr->cvo_regs + CVO_REG_IMG_INFO_SUBSAMPLING);
	iowrite32(VFR_FIXED_BPS,    vfr->cvo_regs + CVO_REG_IMG_INFO_BPS);
	iowrite32(CVO_FALLBACK_FORCE_VID, vfr->cvo_regs + CVO_REG_FALLBACK);

	dev_info(vfr->dev, "CVO programmed: %dx%d RGB 8bpc Lite mode\n",
		 VFR_FIXED_WIDTH, VFR_FIXED_HEIGHT);
}

/* -----------------------------------------------------------------------
 * Platform driver probe / remove
 * ----------------------------------------------------------------------- */
static int altera_vfr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct altera_vfr *vfr;
	struct resource *res;
	int ret;

	vfr = devm_drm_dev_alloc(dev, &altera_vfr_drm_driver,
				 struct altera_vfr, drm);
	if (IS_ERR(vfr))
		return PTR_ERR(vfr);

	vfr->dev     = dev;
	vfr->running = false;

	/* Map VFR hardware register block */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	vfr->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(vfr->regs))
		return PTR_ERR(vfr->regs);

	dev_info(dev, "VFR registers mapped at %pa\n", &res->start);

	/* Sanity check VID_PID */
	{
		u32 vid_pid = ioread32(vfr->regs + VFR_REG_VID_PID);

		if (vid_pid != VFR_VID_PID_EXPECTED) {
			dev_err(dev, "unexpected VID_PID 0x%08X (expected 0x%08X)\n",
				vid_pid, VFR_VID_PID_EXPECTED);
			return -ENODEV;
		}
		dev_info(dev, "VFR VID_PID confirmed: 0x%08X\n", vid_pid);
	}

	/* Map CVO register block */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(dev, "CVO register resource not found\n");
		return -ENODEV;
	}
	vfr->cvo_regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(vfr->cvo_regs))
		return PTR_ERR(vfr->cvo_regs);

	dev_info(dev, "CVO registers mapped at %pa\n", &res->start);

	/* 32-bit DMA mask */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	vfr_stop(vfr);
	altera_vfr_cvo_init(vfr);

	ret = altera_vfr_kms_init(vfr);
	if (ret)
		return ret;

	drm_mode_config_reset(&vfr->drm);

	ret = drm_dev_register(&vfr->drm, 0);
	if (ret) {
		dev_err(dev, "drm_dev_register failed: %d\n", ret);
		return ret;
	}

	drm_client_setup_with_fourcc(&vfr->drm, DRM_FORMAT_XRGB8888);

	platform_set_drvdata(pdev, vfr);
	dev_info(dev, "Altera VFR DRM ready — 1920x1080 fixed pipeline\n");
	return 0;
}

static void altera_vfr_remove(struct platform_device *pdev)
{
	struct altera_vfr *vfr = platform_get_drvdata(pdev);

	drm_dev_unregister(&vfr->drm);
	drm_atomic_helper_shutdown(&vfr->drm);
	vfr_stop(vfr);
}

/* -----------------------------------------------------------------------
 * OF match + module glue
 * ----------------------------------------------------------------------- */
static const struct of_device_id altera_vfr_of_match[] = {
	{ .compatible = "altr,vfr-drm-1.0" },
	{ .compatible = "altr,vip-frame-reader-2.0" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, altera_vfr_of_match);

static struct platform_driver altera_vfr_platform_driver = {
	.probe  = altera_vfr_probe,
	.remove = altera_vfr_remove,
	.driver = {
		.name           = "altera-vfr-drm",
		.of_match_table = altera_vfr_of_match,
	},
};

module_platform_driver(altera_vfr_platform_driver);

MODULE_AUTHOR("DRM Driver for Intel/Altera VFR");
MODULE_DESCRIPTION("Altera VVP Video Frame Reader DRM driver — fixed 1080p");
MODULE_LICENSE("GPL v2");
