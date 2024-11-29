#include "rk_hdmirx_audio.h"

#include <linux/clk.h>
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

#define is_validfs(x) (x == 32000 || \
			x == 44100 || \
			x == 48000 || \
			x == 88200 || \
			x == 96000 || \
			x == 176400 || \
			x == 192000 || \
			x == 768000)

static void process_audio_change(struct rk_hdmirx_dev *hdmirx_dev)
{
	struct hdmirx_stream *stream = &hdmirx_dev->stream;
	const struct v4l2_event evt_audio_info = {
		.type = RK_HDMIRX_V4L2_EVENT_AUDIOINFO,
	};
	v4l2_event_queue(&stream->vdev, &evt_audio_info);
}

static u32 hdmirx_audio_ch(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 acr_pb3_0, acr_pb7_4, ch, ca;

	hdmirx_readl(hdmirx_dev, PKTDEC_AUDIF_PH2_1);
	acr_pb3_0 = hdmirx_readl(hdmirx_dev, PKTDEC_AUDIF_PB3_0);
	acr_pb7_4 =  hdmirx_readl(hdmirx_dev, PKTDEC_AUDIF_PB7_4);
	ca = acr_pb7_4 & 0xff;
	ch = ((acr_pb3_0>>8) & 0x07) + 1;
	dev_dbg(hdmirx_dev->dev, "%s: acr_pb3_0=%#x; ch=%u; ca=%#x\n",
		__func__, acr_pb3_0, ch, ca);
	return ch;
}

static u32 hdmirx_audio_fs(struct rk_hdmirx_dev *hdmirx_dev)
{
	u64 tmds_clk, fs_audio = 0;
	u32 acr_cts, acr_n, tmdsqpclk_freq;
	u32 acr_pb7_4, acr_pb3_0;

	tmdsqpclk_freq = hdmirx_readl(hdmirx_dev, CMU_TMDSQPCLK_FREQ);
	hdmirx_readl(hdmirx_dev, PKTDEC_ACR_PH2_1);
	acr_pb7_4 = hdmirx_readl(hdmirx_dev, PKTDEC_ACR_PB3_0);
	acr_pb3_0 = hdmirx_readl(hdmirx_dev, PKTDEC_ACR_PB7_4);
	acr_cts = __be32_to_cpu(acr_pb7_4) & 0xfffff;
	acr_n = (__be32_to_cpu(acr_pb3_0) & 0x0fffff00) >> 8;
	tmds_clk = tmdsqpclk_freq * 4 * 1000U;
	if (acr_cts != 0) {
		fs_audio = div_u64((tmds_clk * acr_n), acr_cts);
		fs_audio /= 128;
		fs_audio = div_u64(fs_audio + 50, 100);
		fs_audio *= 100;
	}
	dev_dbg(hdmirx_dev->dev, "%s: fs_audio=%llu; acr_cts=%u; acr_n=%u\n",
		__func__, fs_audio, acr_cts, acr_n);
	return fs_audio;
}

static void hdmirx_audio_set_ch(struct rk_hdmirx_dev *hdmirx_dev, u32 ch_audio)
{
	hdmirx_dev->audio_state.ch_audio = ch_audio;
}

static void hdmirx_audio_set_fs(struct rk_hdmirx_dev *hdmirx_dev, u32 fs_audio)
{
	u32 hdmirx_aud_clkrate_t = fs_audio*128;

	dev_dbg(hdmirx_dev->dev, "%s: %u to %u with fs %u\n", __func__,
		hdmirx_dev->audio_state.hdmirx_aud_clkrate, hdmirx_aud_clkrate_t,
		fs_audio);
	clk_set_rate(hdmirx_dev->clks[1].clk, hdmirx_aud_clkrate_t);
	hdmirx_dev->audio_state.hdmirx_aud_clkrate = hdmirx_aud_clkrate_t;
	hdmirx_dev->audio_state.fs_audio = fs_audio;
}

void hdmirx_audio_fifo_init(struct rk_hdmirx_dev *hdmirx_dev)
{
	dev_info(hdmirx_dev->dev, "%s\n", __func__);
	hdmirx_writel(hdmirx_dev, AUDIO_FIFO_CONTROL, 1);
	usleep_range(200, 210);
	hdmirx_writel(hdmirx_dev, AUDIO_FIFO_CONTROL, 0);
}

static void hdmirx_audio_clk_ppm_inc(struct rk_hdmirx_dev *hdmirx_dev, int ppm)
{
	int delta, rate, inc;

	rate = hdmirx_dev->audio_state.hdmirx_aud_clkrate;
	if (ppm < 0) {
		ppm = -ppm;
		inc = -1;
	} else
		inc = 1;
	delta = (int)div64_u64((uint64_t)rate * ppm + 500000, 1000000);
	delta *= inc;
	rate = hdmirx_dev->audio_state.hdmirx_aud_clkrate + delta;
	dev_dbg(hdmirx_dev->dev, "%s: %u to %u(delta:%d)\n",
		__func__, hdmirx_dev->audio_state.hdmirx_aud_clkrate, rate, delta);
	clk_set_rate(hdmirx_dev->clks[1].clk, rate);
	hdmirx_dev->audio_state.hdmirx_aud_clkrate = rate;
}

void hdmirx_audio_set_state(struct rk_hdmirx_dev *hdmirx_dev, enum audio_stat stat)
{
	switch (stat) {
	case AUDIO_OFF:
		cancel_delayed_work_sync(&hdmirx_dev->delayed_work_audio);
		hdmirx_update_bits(hdmirx_dev, GLOBAL_SWENABLE, AUDIO_ENABLE, 0);
		break;
	case AUDIO_ON:
		schedule_delayed_work_on(hdmirx_dev->bound_cpu,
					 &hdmirx_dev->delayed_work_audio, HZ / 2);
		break;
	default:
		break;
	}
}

void hdmirx_audio_interrupts_setup(struct rk_hdmirx_dev *hdmirx_dev, bool en)
{
	//dev_info(hdmirx_dev->dev, "%s: %d", __func__, en);
	if (en) {
		hdmirx_update_bits(hdmirx_dev, AVPUNIT_1_INT_MASK_N,
				   DEFRAMER_VSYNC_THR_REACHED_MASK_N,
				   DEFRAMER_VSYNC_THR_REACHED_MASK_N);
	} else {
		hdmirx_update_bits(hdmirx_dev, AVPUNIT_1_INT_MASK_N,
				   DEFRAMER_VSYNC_THR_REACHED_MASK_N,
				   0);
	}
}

void hdmirx_audio_setup(struct rk_hdmirx_dev *hdmirx_dev)
{
	struct hdmirx_audiostate *as = &hdmirx_dev->audio_state;

	as->ctsn_flag = 0;
	as->fs_audio = 0;
	as->ch_audio = 0;
	as->pre_state = 0;
	as->init_state = INIT_FIFO_STATE*4;
	as->fifo_int = false;
	as->audio_enabled = false;
	hdmirx_audio_set_fs(hdmirx_dev, 44100);
	/* Disable audio domain */
	hdmirx_update_bits(hdmirx_dev, GLOBAL_SWENABLE, AUDIO_ENABLE, 0);
	/* Configure Vsync interrupt threshold */
	hdmirx_update_bits(hdmirx_dev, DEFRAMER_CONFIG0, VS_CNT_THR_QST_MASK, VS_CNT_THR_QST(3));
	hdmirx_audio_interrupts_setup(hdmirx_dev, true);
	hdmirx_writel(hdmirx_dev, DEFRAMER_VSYNC_CNT_CLEAR, VSYNC_CNT_CLR_P);
	hdmirx_clear_interrupt(hdmirx_dev, AVPUNIT_1_INT_CLEAR, DEFRAMER_VSYNC_THR_REACHED_CLEAR);
	hdmirx_writel(hdmirx_dev, AUDIO_FIFO_THR_PASS, INIT_FIFO_STATE);
	hdmirx_writel(hdmirx_dev, AUDIO_FIFO_THR, AFIFO_THR_LOW_QST(0x20) | AFIFO_THR_HIGH_QST(0x160));
	hdmirx_writel(hdmirx_dev, AUDIO_FIFO_MUTE_THR, AFIFO_THR_MUTE_LOW_QST(0x8) | AFIFO_THR_MUTE_HIGH_QST(0x178));
}

static int hdmirx_audio_hw_params(struct device *dev, void *data,
				  struct hdmi_codec_daifmt *daifmt,
				  struct hdmi_codec_params *params)
{
	dev_dbg(dev, "%s\n", __func__);
	return 0;
}

static int hdmirx_audio_startup(struct device *dev, void *data)
{
	struct rk_hdmirx_dev *hdmirx_dev = dev_get_drvdata(dev);

	if (tx_5v_power_present(hdmirx_dev) && hdmirx_dev->audio_present)
		return 0;
	dev_err(dev, "%s: device is no connected or audio is off\n", __func__);
	return -ENODEV;
}

static void hdmirx_audio_shutdown(struct device *dev, void *data)
{
	dev_dbg(dev, "%s\n", __func__);
}

static int hdmirx_audio_get_dai_id(struct snd_soc_component *comment,
				   struct device_node *endpoint)
{
	dev_dbg(comment->dev, "%s\n", __func__);
	return 0;
}

void hdmirx_audio_handle_plugged_change(struct rk_hdmirx_dev *hdmirx_dev, bool plugged)
{
	if (hdmirx_dev->plugged_cb && hdmirx_dev->codec_dev)
		hdmirx_dev->plugged_cb(hdmirx_dev->codec_dev, plugged);
}

static int hdmirx_audio_hook_plugged_cb(struct device *dev, void *data,
					hdmi_codec_plugged_cb fn,
					struct device *codec_dev)
{
	struct rk_hdmirx_dev *hdmirx_dev = dev_get_drvdata(dev);

	dev_dbg(dev, "%s\n", __func__);
	mutex_lock(&hdmirx_dev->work_lock);
	hdmirx_dev->plugged_cb = fn;
	hdmirx_dev->codec_dev = codec_dev;
	hdmirx_audio_handle_plugged_change(hdmirx_dev, tx_5v_power_present(hdmirx_dev));
	mutex_unlock(&hdmirx_dev->work_lock);
	return 0;
}

const struct hdmi_codec_ops hdmirx_audio_codec_ops = {
	.hw_params = hdmirx_audio_hw_params,
	.audio_startup = hdmirx_audio_startup,
	.audio_shutdown = hdmirx_audio_shutdown,
	.get_dai_id = hdmirx_audio_get_dai_id,
	.hook_plugged_cb = hdmirx_audio_hook_plugged_cb
};

int hdmirx_register_audio_device(struct rk_hdmirx_dev *hdmirx_dev)
{
	struct hdmirx_audiostate *as = &hdmirx_dev->audio_state;
	struct hdmi_codec_pdata codec_data = {
		.ops = &hdmirx_audio_codec_ops,
		.spdif = 1,
		.i2s = 1,
		.max_i2s_channels = 8,
		.data = hdmirx_dev,
	};

	as->pdev = platform_device_register_data(hdmirx_dev->dev,
						 HDMI_CODEC_DRV_NAME,
						 PLATFORM_DEVID_AUTO,
						 &codec_data,
						 sizeof(codec_data));

	return PTR_ERR_OR_ZERO(as->pdev);
}

void hdmirx_unregister_audio_device(void *data)
{
	struct rk_hdmirx_dev *hdmirx_dev = data;
	struct hdmirx_audiostate *as = &hdmirx_dev->audio_state;

	if (as->pdev) {
		platform_device_unregister(as->pdev);
		as->pdev = NULL;
	}
}

static const char *audio_fifo_err(u32 fifo_status)
{
	switch (fifo_status & (AFIFO_UNDERFLOW_ST | AFIFO_OVERFLOW_ST)) {
	case AFIFO_UNDERFLOW_ST:
		return "underflow";
	case AFIFO_OVERFLOW_ST:
		return "overflow";
	case AFIFO_UNDERFLOW_ST | AFIFO_OVERFLOW_ST:
		return "underflow and overflow";
	}
	return "underflow or overflow";
}

static void hdmirx_enable_audio_output(struct rk_hdmirx_dev *hdmirx_dev,
				      int ch_audio, int fs_audio, int spdif)
{
	if (spdif) {
		dev_warn(hdmirx_dev->dev, "We don't recommend using spdif\n");
	} else {
		if (ch_audio > 2) {
			hdmirx_update_bits(hdmirx_dev, AUDIO_PROC_CONFIG0,
					   SPEAKER_ALLOC_OVR_EN | I2S_EN,
					   SPEAKER_ALLOC_OVR_EN | I2S_EN);
			hdmirx_writel(hdmirx_dev, AUDIO_PROC_CONFIG3, 0xffffffff);
		} else {
			hdmirx_update_bits(hdmirx_dev, AUDIO_PROC_CONFIG0,
					   SPEAKER_ALLOC_OVR_EN | I2S_EN, I2S_EN);
		}
	}
}

void hdmirx_delayed_work_audio(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct rk_hdmirx_dev *hdmirx_dev = container_of(dwork,
							struct rk_hdmirx_dev,
							delayed_work_audio);
	struct hdmirx_audiostate *as = &hdmirx_dev->audio_state;
	u32 fs_audio, ch_audio, sample_flat;
	int cur_state, init_state, pre_state, fifo_status2;
	unsigned long delay = 200;

	if (!as->audio_enabled) {
		dev_info(hdmirx_dev->dev, "%s: enable audio\n", __func__);
		hdmirx_update_bits(hdmirx_dev, GLOBAL_SWENABLE, AUDIO_ENABLE, AUDIO_ENABLE);
		hdmirx_writel(hdmirx_dev, GLOBAL_SWRESET_REQUEST, AUDIO_SWRESETREQ);
		as->audio_enabled = true;
	}
	fs_audio = hdmirx_audio_fs(hdmirx_dev);
	ch_audio = hdmirx_audio_ch(hdmirx_dev);
	fifo_status2 =  hdmirx_readl(hdmirx_dev, AUDIO_FIFO_STATUS2);
	if (fifo_status2 & (AFIFO_UNDERFLOW_ST | AFIFO_OVERFLOW_ST)) {
		dev_warn(hdmirx_dev->dev, "%s: audio %s %#x, with fs %svalid %d\n",
			 __func__, audio_fifo_err(fifo_status2), fifo_status2,
			 is_validfs(fs_audio) ? "" : "in", fs_audio);
		if (is_validfs(fs_audio)) {
			hdmirx_audio_set_fs(hdmirx_dev, fs_audio);
			hdmirx_audio_set_ch(hdmirx_dev, ch_audio);
			hdmirx_enable_audio_output(hdmirx_dev, ch_audio, fs_audio, 0);
		}
		hdmirx_audio_fifo_init(hdmirx_dev);
		as->pre_state = 0;
		goto exit;
	}
	cur_state = fifo_status2 & 0xFFFF;
	init_state = as->init_state;
	pre_state = as->pre_state;
	dev_dbg(hdmirx_dev->dev, "%s: HDMI_RX_AUD_FIFO_FILLSTS1:%#x, single offset:%d, total offset:%d\n",
		__func__, cur_state, cur_state - pre_state, cur_state - init_state);
	if (!is_validfs(fs_audio)) {
		delay = 1000;
	} else if (abs(fs_audio - as->fs_audio) > 1000 || ch_audio != as->ch_audio) {
		dev_info(hdmirx_dev->dev, "%s: restart audio fs(%d -> %d) ch(%d -> %d)\n",
			 __func__, as->fs_audio, fs_audio, as->ch_audio, ch_audio);
		hdmirx_audio_set_fs(hdmirx_dev, fs_audio);
		hdmirx_audio_set_ch(hdmirx_dev, ch_audio);
		hdmirx_enable_audio_output(hdmirx_dev, ch_audio, fs_audio, 0);
		hdmirx_audio_fifo_init(hdmirx_dev);
		as->pre_state = 0;
		goto exit;
	}

	if (cur_state != 0) {
		if (!hdmirx_dev->audio_present) {
			dev_info(hdmirx_dev->dev, "audio on");
			hdmirx_audio_handle_plugged_change(hdmirx_dev, 1);
			process_audio_change(hdmirx_dev);
			hdmirx_dev->audio_present = true;
		}
		if (cur_state - init_state > 16 && cur_state - pre_state > 0)
			hdmirx_audio_clk_ppm_inc(hdmirx_dev, 10);
		else if (cur_state - init_state < -16 && cur_state - pre_state < 0)
			hdmirx_audio_clk_ppm_inc(hdmirx_dev, -10);
	} else {
		if (hdmirx_dev->audio_present) {
			dev_info(hdmirx_dev->dev, "audio off");
			hdmirx_audio_handle_plugged_change(hdmirx_dev, 0);
			process_audio_change(hdmirx_dev);
			hdmirx_dev->audio_present = false;
		}
	}
	as->pre_state = cur_state;

	sample_flat = hdmirx_readl(hdmirx_dev, AUDIO_PROC_STATUS1) & AUD_SAMPLE_FLAT;
	hdmirx_update_bits(hdmirx_dev, AUDIO_PROC_CONFIG0, I2S_EN, sample_flat ? 0 : I2S_EN);

exit:
	schedule_delayed_work_on(hdmirx_dev->bound_cpu,
			&hdmirx_dev->delayed_work_audio,
			msecs_to_jiffies(delay));
}
