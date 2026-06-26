/* SPDX-License-Identifier: GPL-2.0 */
/*
 * altera_vfr_regs.h - Register definitions for the Intel VVP Video Frame
 *                     Reader (VFR) IP core.
 *
 * Byte addresses taken directly from:
 * "Video and Vision Processing Suite IP User Guide" UG-20344 2026.03.03
 * Table 935 "Video Frame Reader IP Registers"
 * Table 936 "Buffer set register banks"
 *
 * All offsets are byte addresses relative to the VFR peripheral base
 * address as given in the device tree "reg" property.
 */

#ifndef _ALTERA_VFR_REGS_H_
#define _ALTERA_VFR_REGS_H_

/* -----------------------------------------------------------------------
 * Parameterisation registers (read-only)
 * ----------------------------------------------------------------------- */
#define VFR_REG_VID_PID             0x0000  /* Always 0x6AF7_024A           */
#define VFR_REG_VERSION             0x0004
#define VFR_REG_LITE_MODE           0x0008
#define VFR_REG_DEBUG_ENABLED       0x000C
#define VFR_REG_MAX_BUFFER_SETS     0x0010
#define VFR_REG_MAX_HEIGHT          0x0014
#define VFR_REG_MAX_WIDTH           0x0018
#define VFR_REG_BITS_PER_SYMBOL     0x001C
#define VFR_REG_NUM_COLOR_PLANES    0x0020
#define VFR_REG_PIXELS_IN_PARALLEL  0x0024
#define VFR_REG_PACKING             0x0028
/* 0x002C - 0x00FF reserved */

/* -----------------------------------------------------------------------
 * Interrupt registers
 * ----------------------------------------------------------------------- */
#define VFR_REG_IRQ_CONTROL         0x0100
#define   VFR_IRQ_ENABLE            BIT(0)  /* Enable end-of-field interrupt */

#define VFR_REG_IRQ_STATUS          0x0104
#define   VFR_IRQ_PENDING           BIT(0)  /* Interrupt fired; write 1 to clear */
/* 0x0108 - 0x013F reserved */

/* -----------------------------------------------------------------------
 * Control and debug registers
 * ----------------------------------------------------------------------- */
#define VFR_REG_STATUS              0x0140
#define   VFR_STATUS_RUNNING        BIT(0)  /* 1 = outputting video field    */
#define   VFR_STATUS_PENDING_COMMIT BIT(1)  /* 1 = commit pending            */

#define VFR_REG_LAST_BUF_READ       0x0144
/* 0x0148 - 0x018F reserved */

#define VFR_REG_COMMIT              0x0190
#define VFR_REG_NUM_BUFFER_SETS     0x0194
#define VFR_REG_BUFFER_MODE         0x0198
#define VFR_REG_STARTING_BUFSET     0x019C
#define VFR_REG_RUN                 0x01A0
#define VFR_REG_FSYNC_MODE          0x01A4

/* VFR_REG_RUN values */
#define VFR_RUN_STOP                0
#define VFR_RUN_FREE_RUNNING        1
#define VFR_RUN_FSYNC               2
#define VFR_RUN_SINGLE_SHOT         3

/* VFR_REG_BUFFER_MODE values */
#define VFR_BUF_MODE_SINGLE_SET     0
#define VFR_BUF_MODE_ALL_SETS       1

/* -----------------------------------------------------------------------
 * Buffer set register banks
 *
 * Buffer set N base address: 0x01B0 + N * 0x40
 * Each bank is 0x40 (64) bytes.
 * ----------------------------------------------------------------------- */
#define VFR_BUFSET_BANK_BASE        0x01B0
#define VFR_BUFSET_BANK_STRIDE      0x0040

#define VFR_BUFSET_BASE(n)          (VFR_BUFSET_BANK_BASE + (n) * VFR_BUFSET_BANK_STRIDE)

/* Offsets within each buffer set bank */
#define VFR_BUFSET_BASE_ADDR        0x00   /* DMA source address            */
#define VFR_BUFSET_NUM_BUFFERS      0x04   /* Number of buffers in set      */
#define VFR_BUFSET_INTER_BUF_OFFS   0x08   /* Inter-buffer address offset   */
#define VFR_BUFSET_INTER_LINE_OFFS  0x0C   /* Inter-line address offset     */
#define VFR_BUFSET_WIDTH            0x10   /* Frame width                   */
#define VFR_BUFSET_HEIGHT           0x14   /* Frame height                  */
#define VFR_BUFSET_INTERLACE        0x18   /* Interlace mode                */
#define VFR_BUFSET_COLORSPACE       0x1C   /* Colorspace                    */
#define VFR_BUFSET_SUBSAMPLING      0x20   /* Chroma subsampling            */
#define VFR_BUFSET_COSITING         0x24   /* Chroma cositing               */
#define VFR_BUFSET_BPS              0x28   /* Bits per sample               */
#define VFR_BUFSET_FIELD_COUNT      0x2C   /* Fields to read; 0 = infinite  */

/* Convenience accessor: buffer set N, field F */
#define VFR_BUFSET_REG(n, field)    (VFR_BUFSET_BASE(n) + (field))

/* -----------------------------------------------------------------------
 * Fixed pipeline parameters — 1080p60 progressive XRGB8888
 *
 * VFR_PIXELS_IN_PARALLEL must match the Qsys instantiation parameter.
 * VFR_FIXED_VFR_WIDTH is the value written to the WIDTH buffer set
 * register — it is actual pixels divided by pixels in parallel.
 * ----------------------------------------------------------------------- */
#define VFR_PIXELS_IN_PARALLEL      1
#define VFR_FIXED_WIDTH             1920
#define VFR_FIXED_HEIGHT            1080
#define VFR_FIXED_STRIDE            (VFR_FIXED_WIDTH * 4)			/* 7680 bytes — XRGB8888 */
#define VFR_FIXED_VFR_WIDTH         (VFR_FIXED_WIDTH * 2)			/* 3840 bytes */
#define VFR_FIXED_BPS               8
#define VFR_COLORSPACE_RGB          0
#define VFR_SUBSAMPLING_444         0
#define VFR_COSITING_NONE           0
#define VFR_PROGRESSIVE             0

/* Expected VID_PID value — sanity check at probe */
#define VFR_VID_PID_EXPECTED        0x6AF7024A

/* -----------------------------------------------------------------------
 * CVO (Clocked Video Output) IMG_INFO registers
 * Base address supplied by device tree reg property index 1.
 * In Lite mode these must be programmed by software — the CVO does not
 * receive frame dimension metadata from the AXI4-S stream.
 * ----------------------------------------------------------------------- */
#define CVO_REG_IMG_INFO_WIDTH       0x0120
#define CVO_REG_IMG_INFO_HEIGHT      0x0124
#define CVO_REG_IMG_INFO_INTERLACE   0x0128
#define CVO_REG_IMG_INFO_COLORSPACE  0x012C
#define CVO_REG_IMG_INFO_SUBSAMPLING 0x0130
#define CVO_REG_IMG_INFO_BPS         0x0138
#define CVO_REG_COMMIT               0x0190
#define CVO_REG_FALLBACK             0x0154
#define CVO_FALLBACK_FORCE_VID       0x8    /* bit 3: force video input */

#endif /* _ALTERA_VFR_REGS_H_ */
