# Intel/Altera VVP Video Frame Reader — Linux DRM Driver

## Overview

Linux DRM/KMS driver for the **Intel/Altera Video and Vision Processing
(VVP) Suite Video Frame Reader (VFR)** IP core.

The VFR reads pixel data from a Linux CMA-allocated framebuffer in system
memory via an AXI DMA read master and delivers a continuous Avalon-ST
video stream to a downstream fixed **1920×1080p60** HDMI output pipeline.
All video timing and clocking is configured at Qsys / Platform Designer
build time; this driver requires no runtime clock or timing programming.

```
┌─────────────────────────────────────────────────┐
│  User-space  (Wayland / X11 / fbdev / modetest) │
└────────────────────┬────────────────────────────┘
                     │  DRM/KMS ioctls
┌────────────────────▼────────────────────────────┐
│  DRM Core                                        │
│    CRTC ──► Primary Plane (CMA GEM, XRGB8888)   │
│    └──────► Encoder ──► Connector (HDMI-A)       │
└────────────────────┬────────────────────────────┘
                     │  atomic_update
                     │  vfr_program_bufset()   [first enable]
                     │  vfr_update_base_addr() [page flip]
┌────────────────────▼────────────────────────────┐
│  Intel/Altera VFR IP Core  (MMIO via DT "reg")  │
│    Buffer Set 0 descriptor                       │
│    AXI DMA read master                           │
│    Avalon-ST video output                        │
└────────────────────┬────────────────────────────┘
                     │  Avalon-ST / parallel RGB
┌────────────────────▼────────────────────────────┐
│  HDMI Transmitter  (e.g. ADV7513, IT6263)        │
└────────────────────┬────────────────────────────┘
                     │  HDMI
                  Display
```

---

## Source Files

| File | Description |
|------|-------------|
| `altera_vfr_drm.c` | Main driver: probe, KMS objects, hardware control, IRQ handler |
| `altera_vfr_drm.h` | Private struct and container-of helpers |
| `altera_vfr_regs.h` | Register byte offsets derived from `intel_vvp_core_regs.h` and `intel_vvp_vfr_regs.h` |
| `Makefile` | Kbuild rules |
| `Kconfig` | Kernel configuration entry |
| `altr,vfr-drm.yaml` | Device tree binding schema |

---

## Register Map

Register byte offsets are derived from the two Altera header files:

### Address layout  (`intel_vvp_core_regs.h`)

| Word range | Area | Base constant |
|-----------|------|---------------|
| 0–1 | Common header (VID_PID, VERSION) | — |
| 2–63 | Compile-time parameters (read-only) | `INTEL_VVP_CORE_COMPILE_TIME_BASE_REG = 2` |
| 64–71 | IRQ registers | `INTEL_VVP_CORE_IRQ_BASE_REG = 64` |
| 72–79 | Image-info registers | `INTEL_VVP_CORE_IMG_INFO_BASE_REG = 72` |
| 80+ | VFR run-time control | `INTEL_VVP_CORE_RT_BASE_REG = 80` |
| 108+ | Buffer set descriptors | `RT_BASE + 28 = word 108` |

All word addresses are multiplied by 4 to produce the byte offsets used
in `altera_vfr_regs.h`.

### VFR compile-time parameters  (word 2+, read-only)

| Register | Description |
|----------|-------------|
| `VFR_REG_LITE_MODE` | Lite mode flag |
| `VFR_REG_DEBUG_ENABLED` | Debug readback enabled |
| `VFR_REG_MAX_BUFFER_SETS` | Maximum buffer sets supported |
| `VFR_REG_MAX_WIDTH / HEIGHT` | Maximum resolution |
| `VFR_REG_BPS` | Bits per sample |
| `VFR_REG_NUM_COLOR_PLANES` | Number of colour planes |
| `VFR_REG_PIXELS_IN_PARALLEL` | Pixels per clock |
| `VFR_REG_PACKING` | Memory packing mode |

### VFR run-time control  (word 80+)

| Register | Description |
|----------|-------------|
| `VFR_REG_STATUS` | Running (bit 0), Commit pending (bit 1) |
| `VFR_REG_LAST_BUF_READ` | Physical address of last buffer read |
| `VFR_REG_COMMIT` | Write 1 to latch pending settings |
| `VFR_REG_NUM_BUFFER_SETS` | Number of active buffer sets |
| `VFR_REG_BUFFER_MODE` | Single-set or all-sets cycling |
| `VFR_REG_STARTING_BUFSET` | Index of first buffer set to use |
| `VFR_REG_RUN` | Stop / Free-running / Fsync / Single-shot |

### Buffer set 0 descriptor  (word 108+)

| Register | Value written by driver |
|----------|------------------------|
| `VFR_BUFSET_REG(0, VFR_BUFSET_BASE_ADDR)` | CMA framebuffer physical address |
| `VFR_BUFSET_REG(0, VFR_BUFSET_NUM_BUFFERS)` | 1 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_INTER_BUF_OFFS)` | 0 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_INTER_LINE_OFFS)` | 7680  (stride = 1920 × 4) |
| `VFR_BUFSET_REG(0, VFR_BUFSET_WIDTH)` | 1920 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_HEIGHT)` | 1080 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_INTERLACE)` | 0  (progressive) |
| `VFR_BUFSET_REG(0, VFR_BUFSET_COLORSPACE)` | 0  (RGB) |
| `VFR_BUFSET_REG(0, VFR_BUFSET_SUBSAMPLING)` | 0  (4:4:4) |
| `VFR_BUFSET_REG(0, VFR_BUFSET_COSITING)` | 0 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_BPS)` | 8 |
| `VFR_BUFSET_REG(0, VFR_BUFSET_FIELD_COUNT)` | 0  (continuous) |

---

## Hardware Programming Sequence

### First enable — `vfr_program_bufset()`

Mirrors the sequence in `intel_vvp_vfr.c`:

```
 1.  VFR_REG_RUN = STOP
 2–13. Write all buffer set 0 descriptor fields (see table above)
14.  VFR_REG_NUM_BUFFER_SETS = 1
15.  VFR_REG_BUFFER_MODE     = SINGLE_SET
16.  VFR_REG_STARTING_BUFSET = 0
17.  VFR_REG_COMMIT          = 1   latch all settings
18.  VFR_REG_RUN             = FREE_RUNNING
```

### Page flip — `vfr_update_base_addr()`

```
 1.  BUFSET[0].BASE_ADDR = new CMA physical address
 2.  VFR_REG_COMMIT      = 1   latch at next frame boundary
```

The hardware latches the new address at the start of the next frame;
DMA output is uninterrupted.

---

## Device Tree Entry

```dts
display-controller@ff200000 {
    compatible = "altr,vfr-drm-1.0";
    reg = <0xff200000 0x400>;
    interrupts = <0 45 IRQ_TYPE_LEVEL_HIGH>;
};
```

The `reg` size of `0x400` (1 KiB) covers the full register map including
all 16 buffer set descriptors (word 108 + 16×16 = word 364, byte 1456).

| Property | Required | Description |
|----------|----------|-------------|
| `compatible` | Yes | `"altr,vfr-drm-1.0"` or `"altr,vip-frame-reader-2.0"` |
| `reg` | Yes | Base address and size of the VFR hardware IP register block |
| `interrupts` | Yes | VFR frame interrupt specifier |

---

## Kernel Configuration Dependencies

```
CONFIG_DRM=y (or m)
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_GEM_CMA_HELPER=y
CONFIG_OF=y
```

---

## Building

### In-tree

```bash
# 1. Copy this directory to drivers/gpu/drm/altera-vfr/

# 2. Add to drivers/gpu/drm/Kconfig:
source "drivers/gpu/drm/altera-vfr/Kconfig"

# 3. Add to drivers/gpu/drm/Makefile:
obj-$(CONFIG_DRM_ALTERA_VFR) += altera-vfr/

# 4. Enable in .config:
CONFIG_DRM_ALTERA_VFR=m
```

### Out-of-tree

```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo insmod altera-vfr-drm.ko
```

---

## Page Flip and Vblank Flow

```
atomic_update()              atomic_flush()              IRQ handler
─────────────────            ──────────────              ──────────────────────
vfr_update_base_addr()  ──►  queue flip_event       ──►  drm_crtc_handle_vblank()
  iowrite32(BASE_ADDR)        vfr->pending_flip=true       deliver flip_event
  iowrite32(COMMIT=1)         drm_crtc_vblank_get()        drm_crtc_vblank_put()
                                                           pending_flip = false
```

---

## Licence

`GPL-2.0` — see `SPDX-License-Identifier` in each source file.

The register definitions in `altera_vfr_regs.h` are derived from
`intel_vvp_core_regs.h` and `intel_vvp_vfr_regs.h`, which are Altera
copyrighted materials provided under the Altera IP licence. Ensure you
hold the appropriate licence for the VFR IP core before deploying this
driver in a product.
