/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Rockchip Electronics Co. Ltd.
 *
 * Author: Dingxian Wen <shawn.wen@rock-chips.com>
 */

#ifndef __RK_HDMIRX_H__
#define __RK_HDMIRX_H__

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/extcon-provider.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/rk_hdmirx_config.h>
#include <linux/rockchip/rockchip_sip.h>
#include <linux/seq_file.h>
#include <linux/sync_file.h>
#include <linux/v4l2-dv-timings.h>
#include <linux/workqueue.h>
#include <media/cec.h>
#include <media/cec-notifier.h>
#include <media/v4l2-common.h>
#include <media/v4l2-controls_rockchip.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>
#include <soc/rockchip/rockchip-system-status.h>
#include <sound/hdmi-codec.h>
#include <linux/rk_hdmirx_class.h>

#include "rk_hdmirx_cec.h"
#include "rk_hdmirx_hdcp.h"
#include "rk_hdmirx_audio.h"

#define UPDATE(x, h, l)		(((x) << (l)) & GENMASK((h), (l)))
#define HIWORD_UPDATE(v, h, l)	(((v) << (l)) | (GENMASK((h), (l)) << 16))

// -------------------- SYS_GRF ---------------------------------
#define SYS_GRF_SOC_CON1			0x0304
#define HDMIRXPHY_SRAM_EXT_LD_DONE		BIT(1)
#define HDMIRXPHY_SRAM_BYPASS			BIT(0)
#define SYS_GRF_SOC_STATUS1			0x0384
#define HDMIRXPHY_SRAM_INIT_DONE		BIT(10)
#define SYS_GRF_CHIP_ID				0x0600

// -------------------- VO1_GRF ---------------------------------
#define VO1_GRF_VO1_CON1			0x0004
#define HDCP1_P0_GPIO_IN_SEL			BIT(8)

#define VO1_GRF_VO1_CON2			0x0008
#define HDCP1_GATING_EN				BIT(10)
#define HDMIRX_SDAIN_MSK			BIT(2)
#define HDMIRX_SCLIN_MSK			BIT(1)
#define HDCP2_SWITCH_LCK			BIT(0)
#define HDCP2_ESM_P0_GPIO_IN			0x0300

// -------------------- HDMIRX PHY -------------------------------
#define SUP_DIG_ANA_CREGS_SUP_ANA_NC			0x004f

#define	LANE0_DIG_ASIC_RX_OVRD_OUT_0			0x100f
#define	LANE1_DIG_ASIC_RX_OVRD_OUT_0			0x110f
#define	LANE2_DIG_ASIC_RX_OVRD_OUT_0			0x120f
#define	LANE3_DIG_ASIC_RX_OVRD_OUT_0			0x130f
#define ASIC_ACK_OVRD_EN				BIT(1)
#define ASIC_ACK					BIT(0)

#define	LANE0_DIG_RX_VCOCAL_RX_VCO_CAL_CTRL_2		0x104a
#define	LANE1_DIG_RX_VCOCAL_RX_VCO_CAL_CTRL_2		0x114a
#define	LANE2_DIG_RX_VCOCAL_RX_VCO_CAL_CTRL_2		0x124a
#define	LANE3_DIG_RX_VCOCAL_RX_VCO_CAL_CTRL_2		0x134a
#define FREQ_TUNE_START_VAL_MASK			GENMASK(9, 0)
#define FREQ_TUNE_START_VAL(x)				UPDATE(x, 9, 0)

#define	HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_FSM_CONFIG	0x20c4
#define	HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_ADAPT_REF_FOM	0x20c7
#define HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_RATE_CALC_HDMI14_CDR_SETTING_3_REG	0x20e9
#define CDR_SETTING_BOUNDARY_3_DEFAULT			0x52da
#define HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_RATE_CALC_HDMI14_CDR_SETTING_4_REG	0x20ea
#define CDR_SETTING_BOUNDARY_4_DEFAULT			0x43cd
#define HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_RATE_CALC_HDMI14_CDR_SETTING_5_REG	0x20eb
#define CDR_SETTING_BOUNDARY_5_DEFAULT			0x35b3
#define HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_RATE_CALC_HDMI14_CDR_SETTING_6_REG	0x20fb
#define	CDR_SETTING_BOUNDARY_6_DEFAULT			0x2799
#define HDMIPCS_DIG_CTRL_PATH_MAIN_FSM_RATE_CALC_HDMI14_CDR_SETTING_7_REG	0x20fc
#define CDR_SETTING_BOUNDARY_7_DEFAULT			0x1b65

#define	RAWLANE0_DIG_PCS_XF_RX_OVRD_OUT			0x300e
#define	RAWLANE1_DIG_PCS_XF_RX_OVRD_OUT			0x310e
#define	RAWLANE2_DIG_PCS_XF_RX_OVRD_OUT			0x320e
#define	RAWLANE3_DIG_PCS_XF_RX_OVRD_OUT			0x330e
#define PCS_ACK_WRITE_SELECT				BIT(14)
#define PCS_EN_CTL					BIT(1)
#define PCS_ACK						BIT(0)

#define	RAWLANE0_DIG_AON_FAST_FLAGS			0x305c
#define	RAWLANE1_DIG_AON_FAST_FLAGS			0x315c
#define	RAWLANE2_DIG_AON_FAST_FLAGS			0x325c
#define	RAWLANE3_DIG_AON_FAST_FLAGS			0x335c

// -------------------- HDMIRX Ctrler -------------------------------
#define GLOBAL_SWRESET_REQUEST			0x0020
#define DATAPATH_SWRESETREQ			BIT(12)
#define AUDIO_SWRESETREQ			BIT(9)
#define GLOBAL_SWENABLE				0x0024
#define PHYCTRL_ENABLE				BIT(21)
#define CEC_ENABLE				BIT(16)
#define TMDS_ENABLE				BIT(13)
#define DATAPATH_ENABLE				BIT(12)
#define PKTFIFO_ENABLE				BIT(11)
#define HDCP_ENABLE				BIT(10)
#define AUDIO_ENABLE				BIT(9)
#define AVPUNIT_ENABLE				BIT(8)
#define MAIN_ENABLE				BIT(0)
#define GLOBAL_TIMER_REF_BASE			0x0028
#define CORE_CONFIG				0x0050
#define CMU_CONFIG0				0x0060
#define TMDSQPCLK_STABLE_FREQ_MARGIN_MASK	GENMASK(30, 16)
#define TMDSQPCLK_STABLE_FREQ_MARGIN(x)		UPDATE(x, 30, 16)
#define AUDCLK_STABLE_FREQ_MARGIN_MASK		GENMASK(11, 9)
#define AUDCLK_STABLE_FREQ_MARGIN(x)		UPDATE(x, 11, 9)
#define CMU_STATUS				0x007c
#define TMDSQPCLK_LOCKED_ST			BIT(4)
#define CMU_TMDSQPCLK_FREQ			0x0084
#define PHY_CONFIG				0x00c0
#define LDO_AFE_PROG_MASK			GENMASK(24, 23)
#define LDO_AFE_PROG(x)				UPDATE(x, 24, 23)
#define LDO_PWRDN				BIT(21)
#define TMDS_CLOCK_RATIO			BIT(16)
#define RXDATA_WIDTH				BIT(15)
#define REFFREQ_SEL_MASK			GENMASK(11, 9)
#define REFFREQ_SEL(x)				UPDATE(x, 11, 9)
#define HDMI_DISABLE				BIT(8)
#define PHY_PDDQ				BIT(1)
#define PHY_RESET				BIT(0)
#define PHY_STATUS				0x00c8
#define HDMI_DISABLE_ACK			BIT(1)
#define PDDQ_ACK				BIT(0)
#define PHYCREG_CONFIG0				0x00e0
#define PHYCREG_CR_PARA_SELECTION_MODE_MASK	GENMASK(1, 0)
#define PHYCREG_CR_PARA_SELECTION_MODE(x)	UPDATE(x, 1, 0)
#define PHYCREG_CONFIG1				0x00e4
#define PHYCREG_CONFIG2				0x00e8
#define PHYCREG_CONFIG3				0x00ec
#define PHYCREG_CONTROL				0x00f0
#define PHYCREG_CR_PARA_WRITE_P			BIT(1)
#define PHYCREG_CR_PARA_READ_P			BIT(0)
#define PHYCREG_STATUS				0x00f4

#define MAINUNIT_STATUS				0x0150
#define TMDSVALID_STABLE_ST			BIT(1)
#define DESCRAND_EN_CONTROL			0x0210
#define SCRAMB_EN_SEL_QST_MASK			GENMASK(1, 0)
#define SCRAMB_EN_SEL_QST(x)			UPDATE(x, 1, 0)
#define DESCRAND_SYNC_CONTROL			0x0214
#define RECOVER_UNSYNC_STREAM_QST		BIT(0)
#define DESCRAND_SYNC_SEQ_CONFIG		0x022c
#define DESCRAND_SYNC_SEQ_ERR_CNT_EN		BIT(0)
#define DESCRAND_SYNC_SEQ_STATUS		0x0234
#define DEFRAMER_CONFIG0			0x0270
#define VS_CNT_THR_QST_MASK			GENMASK(27, 20)
#define VS_CNT_THR_QST(x)			UPDATE(x, 27, 20)
#define HS_POL_QST_MASK				GENMASK(19, 18)
#define HS_POL_QST(x)				UPDATE(x, 19, 18)
#define VS_POL_QST_MASK				GENMASK(17, 16)
#define VS_POL_QST(x)				UPDATE(x, 17, 16)
#define VS_REMAPFILTER_EN_QST			BIT(8)
#define HS_FILTER_ORDER_QST_MASK		GENMASK(3, 2)
#define HS_FILTER_ORDER_QST(x)			UPDATE(x, 3, 2)
#define VS_FILTER_ORDER_QST_MASK		GENMASK(1, 0)
#define VS_FILTER_ORDER_QST(x)			UPDATE(x, 1, 0)
#define DEFRAMER_VSYNC_CNT_CLEAR		0x0278
#define VSYNC_CNT_CLR_P				BIT(0)
#define DEFRAMER_STATUS				0x027c
#define OPMODE_STS_MASK				GENMASK(6, 4)
#define I2C_SLAVE_CONFIG1			0x0164
#define I2C_SDA_OUT_HOLD_VALUE_QST_MASK		GENMASK(15, 8)
#define I2C_SDA_OUT_HOLD_VALUE_QST(x)		UPDATE(x, 15, 8)
#define I2C_SDA_IN_HOLD_VALUE_QST_MASK		GENMASK(7, 0)
#define I2C_SDA_IN_HOLD_VALUE_QST(x)		UPDATE(x, 7, 0)
#define OPMODE_STS_MASK				GENMASK(6, 4)
#define HDCP14_CONFIG				0x0290
#define REPEATER_QST				BIT(28)
#define FASTREAUTH_QST				BIT(27)
#define FEATURES_1DOT1_QST			BIT(26)
#define FASTI2C_QST				BIT(25)
#define EESS_CTL_THR_QST_MASK			GENMASK(19, 16)
#define EESS_CTL_THR_QST(x)			UPDATE(x, 19, 16)
#define OESS_CTL3_THR_QST_MASK			GENMASK(11, 8)
#define OESS_CTL3_THR_QST(x)			UPDATE(x, 11, 8)
#define EESS_OESS_SEL_QST_MASK			GENMASK(5, 4)
#define EESS_OESS_SEL_QST(x)			UPDATE(x, 5, 4)
#define KEY_DECRYPT_EN_QST			BIT(0)
#define HDCP14_KEY_SEED				0x02a0
#define KEY_DECRYPT_SEED_QST_MASK		GENMASK(15, 0)
#define KEY_DECRYPT_SEED_QST(x)			UPDATE(x, 15, 0)
#define HDCP14_STATUS				0x2b8
#define HDCP2_CONFIG				0x02f0
#define HDCP2_CONNECTED				BIT(12)
#define HDCP2_SWITCH_OVR_VALUE			BIT(2)
#define HDCP2_SWITCH_OVR_EN			BIT(1)
#define HDCP2_STATUS				0x02f4
#define HDCP2_ESM_P0_GPIO_OUT			0x0304

#define VIDEO_CONFIG2				0x042c
#define VPROC_VSYNC_POL_OVR_VALUE		BIT(19)
#define VPROC_VSYNC_POL_OVR_EN			BIT(18)
#define VPROC_HSYNC_POL_OVR_VALUE		BIT(17)
#define VPROC_HSYNC_POL_OVR_EN			BIT(16)
#define VPROC_FMT_OVR_VALUE_MASK		GENMASK(6, 4)
#define VPROC_FMT_OVR_VALUE(x)			UPDATE(x, 6, 4)
#define VPROC_FMT_OVR_EN			BIT(0)

#define VIDEO_MUTE_VALUE_H			0x0430
#define VIDEO_MUTE_VALUE_L			0x0434
#define AUDIO_FIFO_CONFIG			0x0460
#define AFIFO_FILL_RESTART			BIT(0)
#define AUDIO_FIFO_CONTROL			0x0464
#define AFIFO_INIT_P				BIT(0)
#define AUDIO_FIFO_THR_PASS			0x0468
#define AUDIO_FIFO_THR				0x046c
#define AFIFO_THR_LOW_QST_MASK			GENMASK(25, 16)
#define AFIFO_THR_LOW_QST(x)			UPDATE(x, 25, 16)
#define AFIFO_THR_HIGH_QST_MASK			GENMASK(9, 0)
#define AFIFO_THR_HIGH_QST(x)			UPDATE(x, 9, 0)
#define AUDIO_FIFO_MUTE_THR			0x0470
#define AFIFO_THR_MUTE_LOW_QST_MASK		GENMASK(25, 16)
#define AFIFO_THR_MUTE_LOW_QST(x)		UPDATE(x, 25, 16)
#define AFIFO_THR_MUTE_HIGH_QST_MASK		GENMASK(9, 0)
#define AFIFO_THR_MUTE_HIGH_QST(x)		UPDATE(x, 9, 0)

#define AUDIO_FIFO_STATUS2			0x0478
#define AFIFO_UNDERFLOW_ST			BIT(25)
#define AFIFO_OVERFLOW_ST			BIT(24)

#define AUDIO_PROC_CONFIG0			0x0480
#define SPEAKER_ALLOC_OVR_EN			BIT(16)
#define AUD_MUTE_OVR_VALUE			BIT(13)
#define AUD_MUTE_OVR_EN				BIT(12)
#define I2S_BPCUV_EN				BIT(4)
#define SPDIF_EN				BIT(2)
#define I2S_EN					BIT(1)
#define AUDIO_PROC_CONFIG1			0x0484
#define AUDIO_PROC_CONFIG2			0x0488
#define AFIFO_THR_PASS_DEMUTEMASK_N		BIT(24)
#define AVMUTE_DEMUTEMASK_N			BIT(16)
#define AFIFO_THR_MUTE_LOW_MUTEMASK_N		BIT(9)
#define AFIFO_THR_MUTE_HIGH_MUTEMASK_N		BIT(8)
#define AUD_FMT_CHG_MUTEMASK_N			BIT(1)
#define AVMUTE_MUTEMASK_N			BIT(0)
#define AUDIO_PROC_CONFIG3			0x048c
#define AUDIO_PROC_STATUS1			0x0490
#define AUD_SAMPLE_PRESENT			GENMASK(20, 17)
#define AUD_SAMPLE_FLAT				GENMASK(16, 13)
#define SCDC_CONFIG				0x0580
#define HPDLOW					BIT(1)
#define POWERPROVIDED				BIT(0)
#define SCDC_REGBANK_STATUS1			0x058c
#define SCDC_TMDSBITCLKRATIO			BIT(1)
#define SCDC_REGBANK_STATUS3			0x0594
#define SCDC_REGBANK_CONFIG0			0x05c0
#define SCDC_SINKVERSION_QST_MASK		GENMASK(7, 0)
#define SCDC_SINKVERSION_QST(x)			UPDATE(x, 7, 0)
#define AUDIO_GEN_CONFIG0			0x0740
#define AGEN_LAYOUT				BIT(4)
#define AGEN_SPEAKER_ALLOC			GENMASK(15, 8)

#define CED_CONFIG				0x0760
#define CED_VIDDATACHECKEN_QST			BIT(27)
#define CED_DATAISCHECKEN_QST			BIT(26)
#define CED_GBCHECKEN_QST			BIT(25)
#define CED_CTRLCHECKEN_QST			BIT(24)
#define CED_CHLOCKMAXER_QST_MASK		GENMASK(14, 0)
#define CED_CHLOCKMAXER_QST(x)			UPDATE(x, 14, 0)
#define CED_DYN_CONFIG				0x0768
#define CED_DYN_CONTROL				0x076c
#define PKTEX_BCH_ERRFILT_CONFIG		0x07c4
#define PKTEX_CHKSUM_ERRFILT_CONFIG		0x07c8

#define PKTDEC_ACR_PH2_1			0x1100
#define PKTDEC_ACR_PB3_0			0x1104
#define PKTDEC_ACR_PB7_4			0x1108
#define PKTDEC_AVIIF_PH2_1			0x1200
#define PKTDEC_AVIIF_PB3_0			0x1204
#define RGB_QUANTIZATION_RANGE			GENMASK(27, 26)
#define EXTEND_COLORIMETRY			GENMASK(30, 28)
#define AVI_COLORIMETRY				GENMASK(23, 22)	/* PB2[7:6]: C bits */
#define AVI_COLORIMETRY_EXTENDED		3		/* EC bits are valid */
#define PKTDEC_AVIIF_PB7_4			0x1208
#define VIC_VAL_MASK				GENMASK(6, 0)
#define YCC_QUANTIZATION_RANGE			GENMASK(15, 14)	/* PB5[7:6]: YQ bits */
#define PKTDEC_AVIIF_PB11_8			0x120c
#define PKTDEC_AVIIF_PB15_12			0x1210
#define PKTDEC_AVIIF_PB19_16			0x1214
#define PKTDEC_AVIIF_PB23_20			0x1218
#define PKTDEC_AVIIF_PB27_24			0x121c
#define PKTDEC_AUDIF_PH2_1			0x1240
#define PKTDEC_AUDIF_PB3_0			0x1244
#define PKTDEC_AUDIF_PB7_4			0x1248
#define PKTDEC_AUDIF_PB11_8			0x124c
#define PKTDEC_AUDIF_PB15_12			0x1250
#define PKTDEC_AUDIF_PB19_16			0x1254
#define PKTDEC_AUDIF_PB23_20			0x1258
#define PKTDEC_AUDIF_PB27_24			0x125c
/* The DRM (HDR) InfoFrame decoder is not in the TRM or vendor header; the
 * PKTDEC blocks sit at 0x20 pitch (ACR 0x1100, AVI 0x1200, SPD 0x1220,
 * AUDIF 0x1240, DRM 0x12a0) and were confirmed live against a PQ source
 * (PH2_1 reads 0x1a01 = DRM InfoFrame version 1 / length 26). Snapshot
 * semantics match AVIIF: PBx regs latch on the PH2_1 read.
 */
#define PKTDEC_DRMIF_PH2_1			0x12a0
#define PKTDEC_DRMIF_PB3_0			0x12a4
#define DRMIF_EOTF_MASK				GENMASK(10, 8)

#define PKTFIFO_CONFIG				0x1500
#define PKTFIFO_STORE_FILT_CONFIG		0x1504
#define PKTFIFO_THR_CONFIG0			0x1508
#define PKTFIFO_THR_CONFIG1			0x150c
#define PKTFIFO_CONTROL				0x1510

#define VMON_CONTROL				0x1560
#define VMON_SOURCE_SEL_MASK			GENMASK(30, 28)
#define VMON_SOURCE_SEL_DEFRAMER(x)		UPDATE(x, 30, 28)
#define VMON_IRQ_THR_MASK			BIT(24)
#define VMON_CONTROL2				0x1564
#define VMON_IRQ_VERTICAL_MASK			GENMASK(12, 8)
#define VMON_IRQ_VERTICAL_SEL(x)		UPDATE(x, 12, 8)
#define VMON_IRQ_HORIZONAL_MASK			GENMASK(4, 0)
#define VMON_IRQ_HORIZONAL_SEL(x)		UPDATE(x, 4, 0)
#define VMON_STATUS1				0x1580
#define VMON_STATUS2				0x1584
#define VMON_STATUS3				0x1588
#define VMON_STATUS4				0x158c
#define VMON_STATUS5				0x1590
#define VMON_STATUS6				0x1594
#define VMON_STATUS7				0x1598
#define VMON_ILACE_DETECT			BIT(4)

#define CEC_TX_CONTROL				0x2000
#define CEC_STATUS				0x2004
#define CEC_CONFIG				0x2008
#define RX_AUTO_DRIVE_ACKNOWLEDGE		BIT(9)
#define CEC_ADDR				0x200c
#define CEC_TX_COUNT				0x2020
#define CEC_TX_DATA3_0				0x2024
#define CEC_RX_COUNT_STATUS			0x2040
#define CEC_RX_DATA3_0				0x2044
#define CEC_LOCK_CONTROL			0x2054
#define CEC_RXQUAL_BITTIME_CONFIG		0x2060
#define CEC_RX_BITTIME_CONFIG			0x2064
#define CEC_TX_BITTIME_CONFIG			0x2068

#define DMA_CONFIG1				0x4400
#define UV_WID_MASK				GENMASK(31, 28)
#define UV_WID(x)				UPDATE(x, 31, 28)
#define Y_WID_MASK				GENMASK(27, 24)
#define Y_WID(x)				UPDATE(x, 27, 24)
#define DDR_STORE_FORMAT_MASK			GENMASK(15, 12)
#define DDR_STORE_FORMAT(x)			UPDATE(x, 15, 12)
#define ABANDON_EN				BIT(0)
#define DMA_CONFIG2				0x4404
#define DMA_CONFIG3				0x4408
#define DMA_CONFIG4				0x440c // dma irq en
#define DMA_CONFIG5				0x4410 // dma irq clear status
#define LINE_FLAG_INT_EN			BIT(8)
#define HDMIRX_DMA_IDLE_INT			BIT(7)
#define HDMIRX_LOCK_DISABLE_INT			BIT(6)
#define LAST_FRAME_AXI_UNFINISH_INT_EN		BIT(5)
#define FIFO_OVERFLOW_INT_EN			BIT(2)
#define FIFO_UNDERFLOW_INT_EN			BIT(1)
#define HDMIRX_AXI_ERROR_INT_EN			BIT(0)
#define DMA_CONFIG6				0x4414
#define RB_SWAP_EN				BIT(9)
#define HSYNC_TOGGLE_EN				BIT(5)
#define VSYNC_TOGGLE_EN				BIT(4)
#define HDMIRX_DMA_EN				BIT(1)
#define DMA_CONFIG7				0x4418
#define LINE_FLAG_NUM_MASK			GENMASK(31, 16)
#define LINE_FLAG_NUM(x)			UPDATE(x, 31, 16)
#define LOCK_FRAME_NUM_MASK			GENMASK(11, 0)
#define LOCK_FRAME_NUM(x)			UPDATE(x, 11, 0)
#define DMA_CONFIG8				0x441c
#define REG_MIRROR_EN				BIT(0)
#define DMA_CONFIG9				0x4420
#define DMA_CONFIG10				0x4424
#define DMA_CONFIG11				0x4428
#define EDID_READ_EN_MASK			BIT(8)
#define EDID_READ_EN(x)				UPDATE(x, 8, 8)
#define EDID_WRITE_EN_MASK			BIT(7)
#define EDID_WRITE_EN(x)			UPDATE(x, 7, 7)
#define EDID_SLAVE_ADDR_MASK			GENMASK(6, 0)
#define EDID_SLAVE_ADDR(x)			UPDATE(x, 6, 0)
#define DMA_STATUS1				0x4430 // dma irq status
#define DMA_STATUS2				0x4434
#define DMA_STATUS3				0x4438
#define DMA_STATUS4				0x443c
#define DMA_STATUS5				0x4440
#define DMA_STATUS6				0x4444
#define DMA_STATUS7				0x4448
#define DMA_STATUS8				0x444c
#define DMA_STATUS9				0x4450
#define DMA_STATUS10				0x4454
#define HDMIRX_LOCK				BIT(3)
#define DMA_STATUS11				0x4458
#define HDMIRX_TYPE_MASK			GENMASK(8, 7)
#define HDMIRX_COLOR_DEPTH_MASK			GENMASK(6, 3)
#define HDMIRX_FORMAT_MASK			GENMASK(2, 0)
#define DMA_STATUS12				0x445c
#define DMA_STATUS13				0x4460
#define DMA_STATUS14				0x4464

#define MAINUNIT_INTVEC_INDEX			0x5000
#define MAINUNIT_0_INT_STATUS			0x5010
#define CECRX_NOTIFY_ERR			BIT(12)
#define CECRX_EOM				BIT(11)
#define CECTX_DRIVE_ERR				BIT(10)
#define CECRX_BUSY				BIT(9)
#define CECTX_BUSY				BIT(8)
#define CECTX_FRAME_DISCARDED			BIT(5)
#define CECTX_NRETRANSMIT_FAIL			BIT(4)
#define CECTX_LINE_ERR				BIT(3)
#define CECTX_ARBLOST				BIT(2)
#define CECTX_NACK				BIT(1)
#define CECTX_DONE				BIT(0)
#define MAINUNIT_0_INT_MASK_N			0x5014
#define MAINUNIT_0_INT_CLEAR			0x5018
#define MAINUNIT_0_INT_FORCE			0x501c
#define TIMER_BASE_LOCKED_IRQ			BIT(26)
#define TMDSQPCLK_OFF_CHG			BIT(5)
#define TMDSQPCLK_LOCKED_CHG			BIT(4)
#define MAINUNIT_1_INT_STATUS			0x5020
#define MAINUNIT_1_INT_MASK_N			0x5024
#define MAINUNIT_1_INT_CLEAR			0x5028
#define MAINUNIT_1_INT_FORCE			0x502c
#define MAINUNIT_2_INT_STATUS			0x5030
#define MAINUNIT_2_INT_MASK_N			0x5034
#define MAINUNIT_2_INT_CLEAR			0x5038
#define MAINUNIT_2_INT_FORCE			0x503c
#define PHYCREG_CR_READ_DONE			BIT(11)
#define PHYCREG_CR_WRITE_DONE			BIT(10)
#define TMDSVALID_STABLE_CHG			BIT(1)

#define AVPUNIT_0_INT_STATUS			0x5040
#define AVPUNIT_0_INT_MASK_N			0x5044
#define AVPUNIT_0_INT_CLEAR			0x5048
#define AVPUNIT_0_INT_FORCE			0x504c
#define CED_DYN_CNT_CH2_IRQ			BIT(22)
#define CED_DYN_CNT_CH1_IRQ			BIT(21)
#define CED_DYN_CNT_CH0_IRQ			BIT(20)
#define AVPUNIT_1_INT_STATUS			0x5050
#define VMON_VMEAS_IRQ				BIT(31)
#define VMON_HMEAS_IRQ				BIT(30)
#define DEFRAMER_VSYNC_THR_REACHED_IRQ		BIT(1)
#define AVPUNIT_1_INT_MASK_N			0x5054
#define DEFRAMER_VSYNC_THR_REACHED_MASK_N	BIT(1)
#define DEFRAMER_VSYNC_MASK_N			BIT(0)
#define AVPUNIT_1_INT_CLEAR			0x5058
#define DEFRAMER_VSYNC_THR_REACHED_CLEAR	BIT(1)
#define AVPUNIT_1_INT_FORCE			0x505C
#define PKT_0_INT_STATUS			0x5080
/* CHG irq bits follow the decoder block order: bit = 3 + (block - 0x1100)/0x20
 * (ACR=3, VSIF=10, AVIIF=11, SPDIF=12, AUDIF=13), which puts the DRM decoder
 * at 0x12a0 on bit 16. If the bit is wrong the cost is only a spurious or
 * missed early EOTF re-read: every AVIIF change and timing detection re-reads
 * the EOTF anyway. */
#define PKTDEC_DRMIF_CHG_IRQ			BIT(16)
#define PKTDEC_AUDIF_CHG_IRQ			BIT(13)
#define PKTDEC_AVIIF_CHG_IRQ			BIT(11)
#define PKTDEV_VSIF_CHG_IRQ			BIT(10)
#define PKTDEC_ACR_CHG_IRQ			BIT(3)
#define PKT_0_INT_MASK_N			0x5084
#define PKTDEC_DRMIF_CHG_MASK_N			BIT(16)
#define PKTDEC_AVIIF_CHG_MASK_N			BIT(11)
#define PKTDEV_VSIF_CHG_MASK_N			BIT(10)
#define PKTDEC_ACR_CHG_MASK_N			BIT(3)
#define PKT_0_INT_CLEAR				0x5088
#define PKT_0_INT_FORCE				0x508c
#define PKT_1_INT_STATUS			0x5090
#define PKT_1_INT_MASK_N			0x5094
#define PKT_1_INT_CLEAR				0x5098
#define PKT_2_INT_STATUS			0x50a0
#define PKTDEC_AUDIF_RCV_IRQ			BIT(13)
#define PKTDEC_ACR_RCV_IRQ			BIT(3)
#define PKT_2_INT_MASK_N			0x50a4
#define PKTDEC_AUDIF_RCV_MASK_N			BIT(13)
#define PKTDEC_AVIIF_RCV_IRQ			BIT(11)
#define PKTDEC_ACR_RCV_MASK_N			BIT(3)
#define PKT_2_INT_CLEAR				0x50a8
#define PKTDEC_AUDIF_RCV_CLEAR			BIT(13)
#define PKTDEC_AVIIF_RCV_CLEAR			BIT(11)
#define PKTDEC_ACR_RCV_CLEAR			BIT(3)
#define SCDC_INT_STATUS				0x50c0
#define SCDC_INT_MASK_N				0x50c4
#define SCDC_INT_CLEAR				0x50c8
#define SCDCTMDSCCFG_CHG			BIT(2)

#define HDCP_INT_STATUS				0x50d0
#define HDCP_INT_MASK_N				0x50d4
#define HDCP_INT_CLEAR				0x50d8
#define HDCP_1_INT_STATUS			0x50e0
#define HDCP_1_INT_MASK_N			0x50e4
#define HDCP_1_INT_CLEAR			0x50e8
#define CEC_INT_STATUS				0x5100
#define CEC_INT_MASK_N				0x5104
#define CEC_INT_CLEAR				0x5108

#define INIT_FIFO_STATE			64

#define	RK_HDMIRX_DRVNAME		"mota_hdmi_rx"
#define EDID_NUM_BLOCKS_MAX		2
#define EDID_BLOCK_SIZE			128
#define HDMIRX_DEFAULT_TIMING		V4L2_DV_BT_CEA_640X480P59_94
#define HDMIRX_VDEV_NAME		"mota_hdmi_rx"
#define HDMIRX_REQ_BUFS_MIN		2
#define HDMIRX_STORED_BIT_WIDTH		8
#define IREF_CLK_FREQ_HZ		428571429
#define MEMORY_ALIGN_ROUND_UP_BYTES	64
#define HDMIRX_PLANE_Y			0
#define HDMIRX_PLANE_CBCR		1
#define RK_IRQ_HDMIRX_HDMI		210
#define CPU_LIMIT_FREQ_KHZ		1200000
#define WAIT_PHY_REG_TIME		50
#define WAIT_CR_DONE_MS			50
#define WAIT_TIMER_LOCK_TIME		50
#define WAIT_SIGNAL_LOCK_TIME		600 /* if 5V present: 7ms each time */
#define NO_LOCK_CFG_RETRY_TIME		300
#define WAIT_LOCK_STABLE_TIME		20
#define WAIT_AVI_PKT_TIME		300

#define DMA_CONFIG_4_BITS (LINE_FLAG_INT_EN | HDMIRX_DMA_IDLE_INT | HDMIRX_LOCK_DISABLE_INT | LAST_FRAME_AXI_UNFINISH_INT_EN | FIFO_OVERFLOW_INT_EN | FIFO_UNDERFLOW_INT_EN | HDMIRX_AXI_ERROR_INT_EN)

static char *hdmirx_color_space[9] = {
	"xvYCC601", "xvYCC709", "sYCC601", "Adobe_YCC601",
	"Adobe_RGB", "BT2020_YcCbcCrc", "BT2020_RGB_OR_YCbCr", "ITU601", "ITU709"
};

enum hdmirx_pix_fmt {
	HDMIRX_RGB888 = 0,
	HDMIRX_YUV422 = 1,
	HDMIRX_YUV444 = 2,
	HDMIRX_YUV420 = 3,
};

/* CTA-861 DRM InfoFrame EOTF codes (data byte 1, bits 2:0) */
enum hdmirx_eotf {
	HDMIRX_EOTF_SDR = 0,
	HDMIRX_EOTF_HDR_GAMMA = 1,
	HDMIRX_EOTF_ST2084 = 2,
	HDMIRX_EOTF_HLG = 3,
};

static const char * const pix_fmt_str[] = {
	"RGB888",
	"YUV422",
	"YUV444",
	"YUV420",
};

/* Packed 10-bit 4:2:0 (four samples in five bytes), mainline's NV15. This
 * kernel predates the uapi define, so provide it here for the 10-bit store path. */
#ifndef V4L2_PIX_FMT_NV15
#define V4L2_PIX_FMT_NV15 v4l2_fourcc('N', 'V', '1', '5')
#endif
#ifndef V4L2_PIX_FMT_NV20
#define V4L2_PIX_FMT_NV20 v4l2_fourcc('N', 'V', '2', '0') /* packed 10-bit 4:2:2 */
#endif

enum ddr_store_fmt {
	STORE_RGB888 = 0,
	STORE_RGBA_ARGB,
	STORE_YUV420_8BIT,
	STORE_YUV420_10BIT,
	STORE_YUV422_8BIT,
	STORE_YUV422_10BIT,
	STORE_YUV444_8BIT,
	STORE_YUV420_16BIT = 8,
	STORE_YUV422_16BIT = 9,
};

enum hdmirx_reg_attr {
	HDMIRX_ATTR_RW = 0,
	HDMIRX_ATTR_RO = 1,
	HDMIRX_ATTR_WO = 2,
	HDMIRX_ATTR_RE = 3,
};

enum hdmirx_edid_version {
	HDMIRX_EDID_USER = 0,
	HDMIRX_EDID_340M = 1,
	HDMIRX_EDID_600M = 2,
};

struct hdmirx_reg_table {
	int reg_base;
	int reg_end;
	enum hdmirx_reg_attr attr;
};

struct hdmirx_fence_context {
	u64 context;
	u64 seqno;
	spinlock_t spinlock;
};

struct hdmirx_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	union {
		u32 buff_addr[VIDEO_MAX_PLANES];
		void *vaddr[VIDEO_MAX_PLANES];
	};
};

struct hdmirx_stream {
	struct rk_hdmirx_dev *hdmirx_dev;
	struct video_device vdev;
	struct vb2_queue buf_queue;
	struct list_head buf_head;
	struct hdmirx_buffer *curr_buf;
	struct hdmirx_buffer *next_buf;
	struct v4l2_pix_format_mplane pixm;
	const struct hdmirx_output_fmt *out_fmt;
	struct mutex vlock;
	spinlock_t vbq_lock;
	bool stopping;
	wait_queue_head_t wq_stopped;
	u32 frame_idx;
	u32 line_flag_int_cnt;
	u32 irq_stat;
};

struct hdmirx_fence {
	struct list_head fence_list;
	struct dma_fence *fence;
	int fence_fd;
};

struct hdmirx_audiostate {
	struct platform_device *pdev;
	u32 hdmirx_aud_clkrate;
	u32 fs_audio;
	u32 ch_audio;
	u32 ctsn_flag;
	u32 fifo_flag;
	int init_state;
	int pre_state;
	bool fifo_int;
	bool audio_enabled;
};

struct rk_hdmirx_dev {
	struct cec_notifier *cec_notifier;
	struct cpufreq_policy *policy;
	struct device *dev;
	struct device *classdev;
	struct device *codec_dev;
	struct device_node *of_node;
	struct hdmirx_stream stream;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *detect_tx_5v_ctrl;
	struct v4l2_ctrl *audio_sampling_rate_ctrl;
	struct v4l2_ctrl *audio_present_ctrl;
	struct v4l2_dv_timings timings;
	struct gpio_desc *hdmirx_det_gpio;
	struct work_struct work_wdt_config;
	struct delayed_work delayed_work_hotplug;
	struct delayed_work delayed_work_res_change;
	struct delayed_work delayed_work_signal_check;
	struct delayed_work delayed_work_audio;
	struct delayed_work delayed_work_heartbeat;
	struct delayed_work delayed_work_cec;
	struct dentry *debugfs_dir;
	struct freq_qos_request min_sta_freq_req;
	struct hdmirx_audiostate audio_state;
	struct extcon_dev *extcon;
	struct hdmirx_cec *cec;
	struct hdmirx_fence_context fence_ctx;
	struct mutex stream_lock;
	struct mutex work_lock;
	struct pm_qos_request pm_qos;
	struct reset_control *rst_a;
	struct reset_control *rst_p;
	struct reset_control *rst_ref;
	struct reset_control *rst_biu;
	struct clk_bulk_data *clks;
	struct regmap *grf;
	struct regmap *vo1_grf;
	struct rk_hdmirx_hdcp *hdcp;
	struct hdmirx_fence *hdmirx_fence;
	struct list_head qbuf_fence_list_head;
	struct list_head done_fence_list_head;
	void __iomem *regs;
	int edid_version;
	int audio_present;
	int hdmi_irq;
	int dma_irq;
	int det_irq;
	enum hdmirx_pix_fmt pix_fmt;
	bool avi_pkt_rcv;
	struct completion cr_done;
	bool timer_base_lock;
	bool tmds_clk_ratio;
	bool is_dvi_mode;
	bool power_on;
	bool initialized;
	bool freq_qos_add;
	bool get_timing;
	bool cec_enable;
	bool hpd_on;
	bool force_off;
	u8 hdcp_enable;
	u32 num_clks;
	u32 edid_blocks_written;
	u32 hpd_trigger_level;
	u32 cur_vic;
	u32 cur_fmt_fourcc;
	u32 cur_color_range;
	u32 cur_color_space;
	u32 cur_eotf;
	u32 color_depth;
	u32 cpu_freq_khz;
	u32 bound_cpu;
	u32 phy_cpuid;
	u32 fps;
	u32 wdt_cfg_bound_cpu;
	u8 edid[EDID_BLOCK_SIZE * 2];
	hdmi_codec_plugged_cb plugged_cb;
	spinlock_t rst_lock;
	spinlock_t fence_lock;
};

/* module parameters, defined in rk_hdmirx.c */
extern int debug;
extern bool low_latency;

void hdmirx_writel(struct rk_hdmirx_dev *hdmirx_dev, int reg, u32 val);
u32 hdmirx_readl(struct rk_hdmirx_dev *hdmirx_dev, int reg);
void hdmirx_clear_interrupt(struct rk_hdmirx_dev *hdmirx_dev, u32 reg, u32 val);
void hdmirx_update_bits(struct rk_hdmirx_dev *hdmirx_dev, int reg, u32 mask, u32 data);
void hdmirx_hpd_config(struct rk_hdmirx_dev *hdmirx_dev, bool en);

bool tx_5v_power_present(struct rk_hdmirx_dev *hdmirx_dev);
bool hdmirx_signal_locked(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_plugout(struct rk_hdmirx_dev *hdmirx_dev);
void process_signal_change(struct rk_hdmirx_dev *hdmirx_dev);

int hdmirx_write_edid(struct rk_hdmirx_dev *hdmirx_dev, struct v4l2_edid *edid, bool hpd_up);

#endif
