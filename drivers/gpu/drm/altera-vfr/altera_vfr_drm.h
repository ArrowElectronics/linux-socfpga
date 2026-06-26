/* SPDX-License-Identifier: GPL-2.0 */
/*
 * altera_vfr_drm.h — Private header for Intel/Altera VFR DRM driver
 *
 * Fixed 1080p pipeline. No interrupt is used — the CRTC is configured
 * with no_vblank=true and flip events are delivered immediately in
 * atomic_flush via drm_crtc_send_vblank_event().
 */

#ifndef _ALTERA_VFR_DRM_H_
#define _ALTERA_VFR_DRM_H_

#include <linux/types.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

/**
 * struct altera_vfr - Driver private state
 *
 * @drm:          Base DRM device — must be the first member
 * @dev:          Linux device handle
 * @regs:         MMIO base of the VFR register block (from DT "reg")
 * @running:      True once vfr_program_bufset() has been called and the
 *                hardware is actively performing DMA readout
 * @primary:      DRM primary plane (DMA-backed)
 * @crtc:         DRM CRTC (no_vblank=true)
 * @encoder:      DRM encoder (TMDS/HDMI)
 * @connector:    DRM HDMI-A connector (always connected, fixed mode)
 */
struct altera_vfr {
	struct drm_device    drm;        /* must be first */

	struct device       *dev;
	void __iomem        *regs;       /* VFR MMIO registers             */
	void __iomem        *cvo_regs;   /* CVO MMIO registers             */
	bool                 running;

	struct drm_plane     primary;
	struct drm_crtc      crtc;
	struct drm_encoder   encoder;
	struct drm_connector connector;
};

/* Container-of helpers */
#define to_altera_vfr(d)   container_of(d, struct altera_vfr, drm)
#define crtc_to_vfr(c)     container_of(c, struct altera_vfr, crtc)
#define plane_to_vfr(p)    container_of(p, struct altera_vfr, primary)
#define encoder_to_vfr(e)  container_of(e, struct altera_vfr, encoder)

#endif /* _ALTERA_VFR_DRM_H_ */
