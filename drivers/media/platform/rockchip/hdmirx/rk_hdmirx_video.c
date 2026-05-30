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

#include "rk_hdmirx.h"
#include "rk_hdmirx_video.h"

#define HDMIRX_PLANE_Y			0
#define HDMIRX_PLANE_CBCR		1

static int debug = 0;
static int low_latency = 0;

static const struct v4l2_dv_timings_cap hdmirx_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.reserved = { 0 },
	V4L2_INIT_BT_TIMINGS(640, 4096,			/* min/max width */
			     480, 2160,			/* min/max height */
			     20000000, 600000000,	/* min/max pixelclock */
			     /* standards */
			     V4L2_DV_BT_STD_CEA861,
			     /* capabilities */
			     V4L2_DV_BT_CAP_PROGRESSIVE |
			     V4L2_DV_BT_CAP_INTERLACED)
};

struct hdmirx_output_fmt {
	u32 fourcc;
	u8 cplanes;
	u8 mplanes;
	u8 bpp[VIDEO_MAX_PLANES];
};

static const struct hdmirx_output_fmt g_out_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_BGR24,
		.cplanes = 1,
		.mplanes = 1,
		.bpp = { 24 },
	}, {
		.fourcc = V4L2_PIX_FMT_NV24,
		.cplanes = 2,
		.mplanes = 1,
		.bpp = { 8, 16 },
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.cplanes = 2,
		.mplanes = 1,
		.bpp = { 8, 16 },
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.cplanes = 2,
		.mplanes = 1,
		.bpp = { 8, 16 },
	}
};

static int hdmirx_get_detected_timings(struct rk_hdmirx_dev *hdmirx_dev, struct v4l2_dv_timings *timings, bool from_dma);

static bool port_no_link(struct rk_hdmirx_dev *hdmirx_dev)
{
	return !tx_5v_power_present(hdmirx_dev);
}

static bool signal_not_lock(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 mu_status, dma_st10, cmu_st;

	mu_status = hdmirx_readl(hdmirx_dev, MAINUNIT_STATUS);
	dma_st10 = hdmirx_readl(hdmirx_dev, DMA_STATUS10);
	cmu_st = hdmirx_readl(hdmirx_dev, CMU_STATUS);

	if ((mu_status & TMDSVALID_STABLE_ST) &&
	    (dma_st10 & HDMIRX_LOCK) &&
	    (cmu_st & TMDSQPCLK_LOCKED_ST))
		return false;

	return true;
}

static const char *hdmirx_fence_get_name(struct dma_fence *fence)
{
	return RK_HDMIRX_DRVNAME;
}

static const struct dma_fence_ops hdmirx_fence_ops = {
	.get_driver_name = hdmirx_fence_get_name,
	.get_timeline_name = hdmirx_fence_get_name,
};

static struct dma_fence *hdmirx_dma_fence_alloc(struct hdmirx_fence_context *fence_ctx)
{
	struct dma_fence *fence = NULL;

	if (fence_ctx == NULL) {
		pr_err("fence_context is NULL!\n");
		return ERR_PTR(-EINVAL);
	}

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	dma_fence_init(fence, &hdmirx_fence_ops, &fence_ctx->spinlock,
		       fence_ctx->context, ++fence_ctx->seqno);

	return fence;
}

static int hdmirx_dma_fence_get_fd(struct dma_fence *fence)
{
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;

	if (!fence)
		return -EINVAL;

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0)
		return fence_fd;

	sync_file = sync_file_create(fence);
	if (!sync_file) {
		put_unused_fd(fence_fd);
		return -ENOMEM;
	}

	fd_install(fence_fd, sync_file->file);

	return fence_fd;
}

static void hdmirx_qbuf_alloc_fence(struct rk_hdmirx_dev *hdmirx_dev)
{
	struct dma_fence *fence;
	int fence_fd;
	struct hdmirx_fence *hdmirx_fence;
	unsigned long lock_flags = 0;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	fence = hdmirx_dma_fence_alloc(&hdmirx_dev->fence_ctx);
	if (!IS_ERR(fence)) {
		fence_fd = hdmirx_dma_fence_get_fd(fence);
		if (fence_fd >= 0) {
			hdmirx_fence = kzalloc(sizeof(struct hdmirx_fence), GFP_KERNEL);
			if (!hdmirx_fence) {
				v4l2_err(v4l2_dev, "%s: failed to alloc hdmirx_fence!\n", __func__);
				return;
			}
			hdmirx_fence->fence = fence;
			hdmirx_fence->fence_fd = fence_fd;
			spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);
			list_add_tail(&hdmirx_fence->fence_list, &hdmirx_dev->qbuf_fence_list_head);
			spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);
			v4l2_dbg(3, debug, v4l2_dev, "%s: fence:%p, fence_fd:%d\n",
				 __func__, fence, fence_fd);
		} else {
			dma_fence_put(fence);
			v4l2_err(v4l2_dev, "%s: failed to get fence fd!\n", __func__);
		}
	} else {
		v4l2_err(v4l2_dev, "%s: alloc fence failed!\n", __func__);
	}
}

static void hdmirx_free_fence(struct rk_hdmirx_dev *hdmirx_dev)
{
	unsigned long lock_flags = 0;
	struct hdmirx_fence *vb_fence, *done_fence;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	LIST_HEAD(local_list);

	spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);
	if (hdmirx_dev->hdmirx_fence) {
		v4l2_dbg(2, debug, v4l2_dev, "%s: signal hdmirx_fence fd:%d\n",
			 __func__, hdmirx_dev->hdmirx_fence->fence_fd);
		dma_fence_signal(hdmirx_dev->hdmirx_fence->fence);
		dma_fence_put(hdmirx_dev->hdmirx_fence->fence);
		kfree(hdmirx_dev->hdmirx_fence);
		hdmirx_dev->hdmirx_fence = NULL;
	}

	list_replace_init(&hdmirx_dev->qbuf_fence_list_head, &local_list);
	spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);

	while (!list_empty(&local_list)) {
		vb_fence = list_first_entry(&local_list, struct hdmirx_fence, fence_list);
		list_del(&vb_fence->fence_list);
		v4l2_dbg(2, debug, v4l2_dev, "%s: free qbuf_fence fd:%d\n",
			 __func__, vb_fence->fence_fd);
		dma_fence_put(vb_fence->fence);
		put_unused_fd(vb_fence->fence_fd);
		kfree(vb_fence);
	}

	spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);
	list_replace_init(&hdmirx_dev->done_fence_list_head, &local_list);
	spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);
	while (!list_empty(&local_list)) {
		done_fence = list_first_entry(&local_list, struct hdmirx_fence, fence_list);
		list_del(&done_fence->fence_list);
		v4l2_dbg(2, debug, v4l2_dev, "%s: free done_fence fd:%d\n",
			 __func__, done_fence->fence_fd);
		dma_fence_put(done_fence->fence);
		put_unused_fd(done_fence->fence_fd);
		kfree(done_fence);
	}
}

static void return_all_buffers(struct hdmirx_stream *stream,
			       enum vb2_buffer_state state)
{
	struct hdmirx_buffer *buf;
	unsigned long flags;
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;

	spin_lock_irqsave(&stream->vbq_lock, flags);
	if (stream->curr_buf)
		list_add_tail(&stream->curr_buf->queue, &stream->buf_head);
	if ((stream->next_buf) && (stream->next_buf != stream->curr_buf))
		list_add_tail(&stream->next_buf->queue, &stream->buf_head);
	stream->curr_buf = NULL;
	stream->next_buf = NULL;

	while (!list_empty(&stream->buf_head)) {
		buf = list_first_entry(&stream->buf_head,
				       struct hdmirx_buffer, queue);
		list_del(&buf->queue);
		spin_unlock_irqrestore(&stream->vbq_lock, flags);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		spin_lock_irqsave(&stream->vbq_lock, flags);
	}
	spin_unlock_irqrestore(&stream->vbq_lock, flags);

	hdmirx_free_fence(hdmirx_dev);
}

static void hdmirx_dqbuf_get_done_fence(struct rk_hdmirx_dev *hdmirx_dev)
{
	unsigned long lock_flags = 0;
	struct hdmirx_fence *done_fence;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);
	if (!list_empty(&hdmirx_dev->done_fence_list_head)) {
		done_fence = list_first_entry(&hdmirx_dev->done_fence_list_head,
				struct hdmirx_fence, fence_list);
		list_del(&done_fence->fence_list);
	} else {
		done_fence = NULL;
	}
	spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);

	if (done_fence) {
		spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);
		if (hdmirx_dev->hdmirx_fence) {
			v4l2_err(v4l2_dev, "%s: last fence not signal, signal now!\n", __func__);
			dma_fence_signal(hdmirx_dev->hdmirx_fence->fence);
			dma_fence_put(hdmirx_dev->hdmirx_fence->fence);
			v4l2_dbg(2, debug, v4l2_dev, "%s: signal fence:%p, old_fd:%d\n",
				 __func__,
				 hdmirx_dev->hdmirx_fence->fence,
				 hdmirx_dev->hdmirx_fence->fence_fd);
			kfree(hdmirx_dev->hdmirx_fence);
			hdmirx_dev->hdmirx_fence = NULL;
		}
		hdmirx_dev->hdmirx_fence = done_fence;
		spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);
		v4l2_dbg(3, debug, v4l2_dev, "%s: fence:%p, fence_fd:%d\n",
			 __func__, done_fence->fence, done_fence->fence_fd);
	}
}

static void hdmirx_vb_done(struct hdmirx_stream *stream,
				   struct vb2_v4l2_buffer *vb_done)
{
	const struct hdmirx_output_fmt *fmt = stream->out_fmt;
	u32 i;
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	/* Dequeue a filled buffer */
	for (i = 0; i < fmt->mplanes; ++i)
		vb2_set_plane_payload(&vb_done->vb2_buf, i, stream->pixm.plane_fmt[i].sizeimage);

	vb_done->vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&vb_done->vb2_buf, VB2_BUF_STATE_DONE);
	v4l2_dbg(4, debug, v4l2_dev, "vb_done fd:%d", vb_done->vb2_buf.planes[0].m.fd);
}

static void dma_idle_int_handler(struct rk_hdmirx_dev *hdmirx_dev, bool *handled)
{
	unsigned long lock_flags = 0;
	struct hdmirx_stream *stream = &hdmirx_dev->stream;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct v4l2_dv_timings timings = hdmirx_dev->timings;
	struct v4l2_bt_timings *bt = &timings.bt;
	struct vb2_v4l2_buffer *vb_done = NULL;

	if (!(stream->irq_stat & LINE_FLAG_INT_EN))
		v4l2_dbg(1, debug, v4l2_dev, "%s: last time have no line_flag_irq\n", __func__);

	if (!stream->next_buf) {
		v4l2_info(v4l2_dev, "%s: dma idle with no next_buf\n", __func__);
		goto DMA_IDLE_OUT;
	}

	if (stream->curr_buf) {
		vb_done = &stream->curr_buf->vb;
		vb_done->vb2_buf.timestamp = ktime_get_ns();
		vb_done->sequence = stream->frame_idx;
		/* config userbits 0 or 0xffffffff as invalid fence_fd*/
		memset(vb_done->timecode.userbits, 0xff, sizeof(vb_done->timecode.userbits));
		hdmirx_vb_done(stream, vb_done);
		stream->frame_idx++;
	}

	stream->curr_buf = stream->next_buf;
	stream->next_buf = NULL;

DMA_IDLE_OUT:
	*handled = true;
}


static void line_flag_int_handler(struct rk_hdmirx_dev *hdmirx_dev, bool *handled)
{
	unsigned long lock_flags = 0;
	struct hdmirx_stream *stream = &hdmirx_dev->stream;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct v4l2_dv_timings timings = hdmirx_dev->timings;
	struct v4l2_bt_timings *bt = &timings.bt;
	u32 dma_cfg6;
	struct vb2_v4l2_buffer *vb_done = NULL;

	stream->line_flag_int_cnt++;
	if (!(stream->irq_stat & HDMIRX_DMA_IDLE_INT))
		v4l2_info(v4l2_dev, "%s: last have no dma_idle_irq\n", __func__);

	dma_cfg6 = hdmirx_readl(hdmirx_dev, DMA_CONFIG6);

	if (!(dma_cfg6 & HDMIRX_DMA_EN)) {
		v4l2_dbg(2, debug, v4l2_dev, "%s: dma not on\n", __func__);
		goto LINE_FLAG_OUT;
	}

	spin_lock_irqsave(&hdmirx_dev->fence_lock, lock_flags);

	if (hdmirx_dev->hdmirx_fence) {
		dma_fence_signal(hdmirx_dev->hdmirx_fence->fence);
		dma_fence_put(hdmirx_dev->hdmirx_fence->fence);
		v4l2_dbg(2, debug, v4l2_dev, "%s: signal last fence:%p, old_fd:%d\n", __func__, hdmirx_dev->hdmirx_fence->fence, hdmirx_dev->hdmirx_fence->fence_fd);
		kfree(hdmirx_dev->hdmirx_fence);
		hdmirx_dev->hdmirx_fence = NULL;
	}

	spin_unlock_irqrestore(&hdmirx_dev->fence_lock, lock_flags);

	// Don't have a next_buf? Try get one
	if (!stream->next_buf) {
		spin_lock(&stream->vbq_lock);
		if (!list_empty(&stream->buf_head)) {
			stream->next_buf = list_first_entry(&stream->buf_head, struct hdmirx_buffer, queue);
			list_del(&stream->next_buf->queue);
		} else {
			stream->next_buf = NULL;
		}
		spin_unlock(&stream->vbq_lock);
	}

	// Still don't have one? We'll drop the frame. Try again next time.
	if (!stream->next_buf) {
		v4l2_info(v4l2_dev, "%s: next_buf NULL, drop the frame!\n", __func__);
		goto LINE_FLAG_OUT;
	}

	hdmirx_writel(hdmirx_dev, DMA_CONFIG2, stream->next_buf->buff_addr[HDMIRX_PLANE_Y]);
	hdmirx_writel(hdmirx_dev, DMA_CONFIG3, stream->next_buf->buff_addr[HDMIRX_PLANE_CBCR]);

	// if (stream->line_flag_int_cnt % 2 == 0) {
	// 	if (!stream->curr_buf)
	// 		goto LINE_FLAG_OUT;
	// 	vb_done = &stream->curr_buf->vb;
	// 	vb_done->vb2_buf.timestamp = ktime_get_ns();
	// 	vb_done->sequence = stream->frame_idx;
	// 	/* config userbits 0 or 0xffffffff as invalid fence_fd*/
	// 	memset(vb_done->timecode.userbits, 0xff, sizeof(vb_done->timecode.userbits));
	// 	hdmirx_vb_done(stream, vb_done);
	// 	stream->frame_idx++;
	// 	if (stream->frame_idx == 30)
	// 		v4l2_info(v4l2_dev, "rcv frames\n");
	// }

	// if (low_latency) {
	// 	if (stream->curr_buf)
	// 		vb_done = &stream->curr_buf->vb;

	// 	if (vb_done) {
	// 		hdmirx_add_fence_to_vb_done(stream, vb_done);
	// 		vb_done->vb2_buf.timestamp = ktime_get_ns();
	// 		vb_done->sequence = stream->frame_idx;
	// 		hdmirx_vb_done(stream, vb_done);
	// 		stream->frame_idx++;
	// 		if (stream->frame_idx == 30)
	// 			v4l2_info(v4l2_dev, "rcv frames\n");
	// 	}

	// 	stream->curr_buf = stream->next_buf;
	// 	stream->next_buf = NULL;
	// }

	if (stream->curr_buf) v4l2_dbg(4, debug, v4l2_dev, "%s: curr_fd:%d\n", __func__, stream->curr_buf->vb.vb2_buf.planes[0].m.fd);
	if (stream->next_buf) v4l2_dbg(4, debug, v4l2_dev, "%s: next_fd:%d\n", __func__, stream->next_buf->vb.vb2_buf.planes[0].m.fd);

LINE_FLAG_OUT:
	*handled = true;
}

irqreturn_t hdmirx_dma_irq_handler(int irq, void *dev_id)
{
	struct rk_hdmirx_dev *hdmirx_dev = dev_id;
	struct hdmirx_stream *stream = &hdmirx_dev->stream;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	u32 dma_stat1, dma_stat13;
	bool handled = false;

	dma_stat1 = hdmirx_readl(hdmirx_dev, DMA_STATUS1);
	dma_stat13 = hdmirx_readl(hdmirx_dev, DMA_STATUS13);
	// v4l2_dbg(3, debug, v4l2_dev, "dma_irq st1:%#x, st13:%d\n", dma_stat1, dma_stat13);

	if (dma_stat1 & HDMIRX_DMA_IDLE_INT) {
		if (stream->stopping) {
			v4l2_dbg(1, debug, v4l2_dev, "%s: stop stream!\n", __func__);
			stream->stopping = false;
			hdmirx_writel(hdmirx_dev, DMA_CONFIG5, 0xffffffff);
			hdmirx_update_bits(hdmirx_dev, DMA_CONFIG4, DMA_CONFIG_4_BITS, 0);
			wake_up(&stream->wq_stopped);
			return IRQ_HANDLED;
		}

		dma_idle_int_handler(hdmirx_dev, &handled);
	}

	if (dma_stat1 & LINE_FLAG_INT_EN)
		line_flag_int_handler(hdmirx_dev, &handled);

	if (!handled)
		v4l2_dbg(3, debug, v4l2_dev, "%s: dma irq not handled, dma_stat1:%#x!\n", __func__, dma_stat1);

	stream->irq_stat = dma_stat1;
	hdmirx_writel(hdmirx_dev, DMA_CONFIG5, 0xffffffff);

	return IRQ_HANDLED;
}

static int hdmirx_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	int ret;
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;

	if (!hdmirx_dev->get_timing)
		return -EINVAL;

	ret = vb2_ioctl_dqbuf(file, priv, p);
	hdmirx_dqbuf_get_done_fence(hdmirx_dev);

	return ret;
}

static int hdmirx_queue_setup(struct vb2_queue *queue,
			     unsigned int *num_buffers,
			     unsigned int *num_planes,
			     unsigned int sizes[],
			     struct device *alloc_ctxs[])
{
	struct hdmirx_stream *stream = vb2_get_drv_priv(queue);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	const struct v4l2_pix_format_mplane *pixm = NULL;
	const struct hdmirx_output_fmt *out_fmt;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	u32 i, height;

	pixm = &stream->pixm;
	out_fmt = stream->out_fmt;

	if (!out_fmt) {
		v4l2_err(v4l2_dev, "%s: out_fmt null pointer err!\n", __func__);
		return -EINVAL;
	}

	*num_planes = out_fmt->mplanes;
	height = pixm->height;

	for (i = 0; i < out_fmt->mplanes; i++) {
		const struct v4l2_plane_pix_format *plane_fmt;
		int h = height;

		plane_fmt = &pixm->plane_fmt[i];
		sizes[i] = plane_fmt->sizeimage / height * h;
	}

	v4l2_dbg(1, debug, v4l2_dev, "%s count %d, size %d\n", v4l2_type_names[queue->type], *num_buffers, sizes[0]);

	return 0;
}

static inline struct hdmirx_buffer *to_hdmirx_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct hdmirx_buffer, vb);
}

/*
 * The vb2_buffer are stored in hdmirx_buffer, in order to unify
 * mplane buffer and none-mplane buffer.
 */
static void hdmirx_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf;
	struct hdmirx_buffer *hdmirx_buf;
	struct vb2_queue *queue;
	struct hdmirx_stream *stream;
	struct v4l2_pix_format_mplane *pixm;
	const struct hdmirx_output_fmt *out_fmt;
	unsigned long lock_flags = 0;
	int i;
	struct rk_hdmirx_dev *hdmirx_dev;
	struct v4l2_device *v4l2_dev;

	if (vb == NULL) {
		pr_err("%s: vb null pointer err!\n", __func__);
		return;
	}

	vbuf = to_vb2_v4l2_buffer(vb);
	hdmirx_buf = to_hdmirx_buffer(vbuf);
	queue = vb->vb2_queue;
	stream = vb2_get_drv_priv(queue);
	pixm = &stream->pixm;
	out_fmt = stream->out_fmt;

	hdmirx_dev = stream->hdmirx_dev;
	v4l2_dev = &hdmirx_dev->v4l2_dev;

	memset(hdmirx_buf->buff_addr, 0, sizeof(hdmirx_buf->buff_addr));
	/*
	 * If mplanes > 1, every c-plane has its own m-plane,
	 * otherwise, multiple c-planes are in the same m-plane
	 */
	for (i = 0; i < out_fmt->mplanes; i++)
		hdmirx_buf->buff_addr[i] = vb2_dma_contig_plane_dma_addr(vb, i);

	if (out_fmt->mplanes == 1) {
		if (out_fmt->cplanes == 1) {
			hdmirx_buf->buff_addr[HDMIRX_PLANE_CBCR] =
				hdmirx_buf->buff_addr[HDMIRX_PLANE_Y];
		} else {
			for (i = 0; i < out_fmt->cplanes - 1; i++)
				hdmirx_buf->buff_addr[i + 1] =
					hdmirx_buf->buff_addr[i] +
					pixm->plane_fmt[i].bytesperline *
					pixm->height;
		}
	}

	v4l2_dbg(4, debug, v4l2_dev, "qbuf fd:%d\n", vb->planes[0].m.fd);

	spin_lock_irqsave(&stream->vbq_lock, lock_flags);
	list_add_tail(&hdmirx_buf->queue, &stream->buf_head);
	spin_unlock_irqrestore(&stream->vbq_lock, lock_flags);

	if (low_latency)
		hdmirx_qbuf_alloc_fence(hdmirx_dev);
}

static void hdmirx_stop_streaming(struct vb2_queue *queue)
{
	struct hdmirx_stream *stream = vb2_get_drv_priv(queue);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	int ret;

	v4l2_info(v4l2_dev, "stream start stopping\n");
	mutex_lock(&hdmirx_dev->stream_lock);
	stream->stopping = true;

	/* wait last irq to return the buffer */
	ret = wait_event_timeout(stream->wq_stopped, stream->stopping != true,
			msecs_to_jiffies(50));
	if (!ret) {
		v4l2_err(v4l2_dev, "%s wait last irq timeout, return bufs!\n",
				__func__);
		stream->stopping = false;
	}

	hdmirx_update_bits(hdmirx_dev, DMA_CONFIG6, HDMIRX_DMA_EN, 0);
	hdmirx_writel(hdmirx_dev, DMA_CONFIG5, 0xffffffff);
	hdmirx_update_bits(hdmirx_dev, DMA_CONFIG4,
			   LINE_FLAG_INT_EN |
			   HDMIRX_DMA_IDLE_INT |
			   HDMIRX_LOCK_DISABLE_INT |
			   LAST_FRAME_AXI_UNFINISH_INT_EN |
			   FIFO_OVERFLOW_INT_EN |
			   FIFO_UNDERFLOW_INT_EN |
			   HDMIRX_AXI_ERROR_INT_EN, 0);
	return_all_buffers(stream, VB2_BUF_STATE_ERROR);
	sip_hdmirx_config(HDMIRX_INFO_NOTIFY, 0, DMA_CONFIG6, 0);
	sip_hdmirx_config(HDMIRX_AUTO_TOUCH_EN, 0, 0, 0);
	mutex_unlock(&hdmirx_dev->stream_lock);
	v4l2_info(v4l2_dev, "stream stopping finished\n");
}

static int hdmirx_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct hdmirx_stream *stream = vb2_get_drv_priv(queue);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	unsigned long lock_flags = 0;
	struct v4l2_dv_timings timings = hdmirx_dev->timings;
	struct v4l2_bt_timings *bt = &timings.bt;
	int line_flag;
	int delay_line;
	uint32_t touch_flag;

	if (!hdmirx_dev->get_timing) {
		v4l2_err(v4l2_dev, "Err, timing is invalid\n");
		return 0;
	}

	if (signal_not_lock(hdmirx_dev)) {
		v4l2_err(v4l2_dev, "%s: signal is not locked, retry!\n", __func__);
		process_signal_change(hdmirx_dev);
		return 0;
	}

	if (!bt->height) {
		v4l2_err(v4l2_dev, "height err: %d\n", bt->height);
		process_signal_change(hdmirx_dev);
		return 0;
	}

	mutex_lock(&hdmirx_dev->stream_lock);
	touch_flag = (hdmirx_dev->phy_cpuid << 1) | 0x1;
	sip_hdmirx_config(HDMIRX_AUTO_TOUCH_EN, 0, touch_flag, 100);
	stream->frame_idx = 0;
	stream->line_flag_int_cnt = 0;
	stream->curr_buf = NULL;
	stream->next_buf = NULL;
	stream->irq_stat = 0;
	stream->stopping = false;

	spin_lock_irqsave(&stream->vbq_lock, lock_flags);
	if (!stream->curr_buf) {
		if (!list_empty(&stream->buf_head)) {
			stream->curr_buf = list_first_entry(&stream->buf_head,
					struct hdmirx_buffer, queue);
			list_del(&stream->curr_buf->queue);
		} else {
			stream->curr_buf = NULL;
		}
	}
	spin_unlock_irqrestore(&stream->vbq_lock, lock_flags);

	if (!stream->curr_buf) {
		mutex_unlock(&hdmirx_dev->stream_lock);
		return -ENOMEM;
	}

	v4l2_dbg(2, debug, v4l2_dev, "%s: start_stream cur_buf y_addr:%#x, uv_addr:%#x\n", __func__, stream->curr_buf->buff_addr[HDMIRX_PLANE_Y], stream->curr_buf->buff_addr[HDMIRX_PLANE_CBCR]);
	hdmirx_writel(hdmirx_dev, DMA_CONFIG2, stream->curr_buf->buff_addr[HDMIRX_PLANE_Y]);
	hdmirx_writel(hdmirx_dev, DMA_CONFIG3, stream->curr_buf->buff_addr[HDMIRX_PLANE_CBCR]);

	line_flag = (bt->interlaced == V4L2_DV_INTERLACED) ? bt->height / 2 : bt->height;
	delay_line = (low_latency && hdmirx_dev->fps >= 59) ? 10 : line_flag * 2 / 3;

	v4l2_info(v4l2_dev, "%s: delay_line:%d\n", __func__, delay_line);
	hdmirx_update_bits(hdmirx_dev, DMA_CONFIG7, LINE_FLAG_NUM_MASK, LINE_FLAG_NUM(delay_line));

	sip_hdmirx_config(HDMIRX_INFO_NOTIFY, 0, DMA_CONFIG6, HDMIRX_DMA_EN);
	hdmirx_writel(hdmirx_dev, DMA_CONFIG5, 0xffffffff);
	hdmirx_writel(hdmirx_dev, CED_DYN_CONTROL, 0x1);
	hdmirx_update_bits(hdmirx_dev, DMA_CONFIG4, DMA_CONFIG_4_BITS, DMA_CONFIG_4_BITS);
	hdmirx_update_bits(hdmirx_dev, DMA_CONFIG6, HDMIRX_DMA_EN, HDMIRX_DMA_EN);
	v4l2_dbg(1, debug, v4l2_dev, "%s: enable dma", __func__);
	mutex_unlock(&hdmirx_dev->stream_lock);

	return 0;
}


// ---------------------- vb2 queue -------------------------
static struct vb2_ops hdmirx_vb2_ops = {
	.queue_setup = hdmirx_queue_setup,
	.buf_queue = hdmirx_buf_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.stop_streaming = hdmirx_stop_streaming,
	.start_streaming = hdmirx_start_streaming,
};

static int hdmirx_init_vb2_queue(struct vb2_queue *q,
				struct hdmirx_stream *stream,
				enum v4l2_buf_type buf_type)
{
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;

	q->type = buf_type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = stream;
	q->ops = &hdmirx_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct hdmirx_buffer);
	q->min_buffers_needed = HDMIRX_REQ_BUFS_MIN;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &stream->vlock;
	q->dev = hdmirx_dev->dev;
	// q->allow_cache_hints = 1;
	// q->bidirectional = 1;
	q->dma_attrs = DMA_ATTR_FORCE_CONTIGUOUS;
	q->gfp_flags = GFP_DMA32;
	return vb2_queue_init(q);
}

static int hdmirx_querycap(struct file *file, void *priv,
			  struct v4l2_capability *cap)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct device *dev = stream->hdmirx_dev->dev;

	strscpy(cap->driver, dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, dev->driver->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "%s", dev_name(dev));

	return 0;
}

static int fcc_xysubs(u32 fcc, u32 *xsubs, u32 *ysubs)
{
	/* Note: cbcr plane bpp is 16 bit */
	switch (fcc) {
	case V4L2_PIX_FMT_NV24:
		*xsubs = 1;
		*ysubs = 1;
		break;
	case V4L2_PIX_FMT_NV16:
		*xsubs = 2;
		*ysubs = 1;
		break;
	case V4L2_PIX_FMT_NV12:
		*xsubs = 2;
		*ysubs = 2;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static u32 hdmirx_align_bits_per_pixel(const struct hdmirx_output_fmt *fmt,
				      int plane_index)
{
	u32 bpp = 0;

	if (fmt) {
		switch (fmt->fourcc) {
		case V4L2_PIX_FMT_NV24:
		case V4L2_PIX_FMT_NV16:
		case V4L2_PIX_FMT_NV12:
		case V4L2_PIX_FMT_BGR24:
			bpp = fmt->bpp[plane_index];
			break;

		default:
			pr_err("fourcc: %#x is not supported!\n", fmt->fourcc);
			break;
		}
	}

	return bpp;
}

static const struct
hdmirx_output_fmt *find_output_fmt(struct hdmirx_stream *stream, u32 pixelfmt)
{
	const struct hdmirx_output_fmt *fmt;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(g_out_fmts); i++) {
		fmt = &g_out_fmts[i];
		if (fmt->fourcc == pixelfmt)
			return fmt;
	}

	return NULL;
}

static void hdmirx_set_fmt(struct hdmirx_stream *stream, struct v4l2_pix_format_mplane *pixm, bool try)
{
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct v4l2_bt_timings *bt = &hdmirx_dev->timings.bt;
	const struct hdmirx_output_fmt *fmt;
	unsigned int imagesize = 0, planes;
	u32 xsubs = 1, ysubs = 1, i;

	memset(&pixm->plane_fmt[0], 0, sizeof(struct v4l2_plane_pix_format));
	fmt = find_output_fmt(stream, pixm->pixelformat);
	if (!fmt) {
		fmt = &g_out_fmts[0];
		v4l2_err(v4l2_dev,
			"%s: set_fmt:%#x not support, use def_fmt:%x\n",
			__func__, pixm->pixelformat, fmt->fourcc);
	}

	if ((bt->width == 0) || (bt->height == 0))
		v4l2_err(v4l2_dev, "%s: err resolution:%#xx%#x!!!\n",
				__func__, bt->width, bt->height);

	pixm->width = bt->width;
	pixm->height = bt->height;
	pixm->num_planes = fmt->mplanes;
	pixm->field = V4L2_FIELD_NONE;

	switch (hdmirx_dev->cur_color_range) {
	case HDMIRX_DEFAULT_RANGE:
		pixm->quantization = V4L2_QUANTIZATION_DEFAULT;
		break;
	case HDMIRX_LIMIT_RANGE:
		pixm->quantization = V4L2_QUANTIZATION_LIM_RANGE;
		break;
	case HDMIRX_FULL_RANGE:
		pixm->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		break;

	default:
		pixm->quantization = V4L2_QUANTIZATION_DEFAULT;
		break;
	}

	if (hdmirx_dev->pix_fmt == HDMIRX_RGB888) {
		if (hdmirx_dev->cur_color_space == HDMIRX_BT2020_RGB_OR_YCC)
			pixm->colorspace = V4L2_COLORSPACE_BT2020;
		else if (hdmirx_dev->cur_color_space == HDMIRX_ADOBE_RGB)
			pixm->colorspace = V4L2_COLORSPACE_OPRGB;
		else
			pixm->colorspace = V4L2_COLORSPACE_SRGB;
	} else {
		switch (hdmirx_dev->cur_color_space) {
		case HDMIRX_XVYCC601:
			pixm->colorspace = V4L2_COLORSPACE_DEFAULT;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_XV601;
			break;
		case HDMIRX_XVYCC709:
			pixm->colorspace = V4L2_COLORSPACE_REC709;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_XV709;
			break;
		case HDMIRX_SYCC601:
			pixm->colorspace = V4L2_COLORSPACE_DEFAULT;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_601;
			break;
		case HDMIRX_ADOBE_YCC601:
			pixm->colorspace = V4L2_COLORSPACE_DEFAULT;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_601;
			break;
		case HDMIRX_BT2020_YCC_CONST_LUM:
			pixm->colorspace = V4L2_COLORSPACE_BT2020;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_BT2020_CONST_LUM;
			break;
		case HDMIRX_BT2020_RGB_OR_YCC:
			pixm->colorspace = V4L2_COLORSPACE_BT2020;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_BT2020;
			break;

		default:
			pixm->colorspace = V4L2_COLORSPACE_DEFAULT;
			pixm->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
			break;
		}
	}

	/* calculate plane size and image size */
	fcc_xysubs(fmt->fourcc, &xsubs, &ysubs);
	planes = fmt->cplanes ? fmt->cplanes : fmt->mplanes;

	for (i = 0; i < planes; i++) {
		struct v4l2_plane_pix_format *plane_fmt;
		int width, height, bpl, size, bpp;

		if (i == 0) {
			width = pixm->width;
			height = pixm->height;
		} else {
			width = pixm->width / xsubs;
			height = pixm->height / ysubs;
		}

		bpp = hdmirx_align_bits_per_pixel(fmt, i);
		bpl = ALIGN(width * bpp / HDMIRX_STORED_BIT_WIDTH, MEMORY_ALIGN_ROUND_UP_BYTES);
		size = bpl * height;
		imagesize += size;

		if (fmt->mplanes > i) {
			/* Set bpl and size for each mplane */
			plane_fmt = pixm->plane_fmt + i;
			plane_fmt->bytesperline = bpl;
			plane_fmt->sizeimage = size;
		}

		v4l2_dbg(1, debug, v4l2_dev,
			 "C-Plane %i size: %d, Total imagesize: %d\n",
			 i, size, imagesize);
	}

	/* convert to non-MPLANE format.
	 * It's important since we want to unify non-MPLANE and MPLANE.
	 */
	if (fmt->mplanes == 1)
		pixm->plane_fmt[0].sizeimage = imagesize;

	if (!try) {
		stream->out_fmt = fmt;
		stream->pixm = *pixm;

		v4l2_dbg(1, debug, v4l2_dev,
			"%s: req(%d, %d), out(%d, %d), fmt:%#x\n", __func__,
			pixm->width, pixm->height, stream->pixm.width,
			stream->pixm.height, fmt->fourcc);
	}
}


static int hdmirx_try_fmt_vid_cap_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (pix->pixelformat != hdmirx_dev->cur_fmt_fourcc)
		return -EINVAL;

	hdmirx_set_fmt(stream, &f->fmt.pix_mp, true);

	return 0;
}

static int hdmirx_s_fmt_vid_cap_mplane(struct file *file,
				      void *priv, struct v4l2_format *f)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (vb2_is_busy(&stream->buf_queue)) {
		v4l2_err(v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	if (pix->pixelformat != hdmirx_dev->cur_fmt_fourcc) {
		v4l2_err(v4l2_dev, "%s: err, set_fmt:%#x, cur_fmt:%#x!\n",
			__func__, pix->pixelformat, hdmirx_dev->cur_fmt_fourcc);
		return -EINVAL;
	}

	hdmirx_set_fmt(stream, &f->fmt.pix_mp, false);

	return 0;
}

static int hdmirx_g_fmt_vid_cap_mplane(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_pix_format_mplane pixm;

	pixm.pixelformat = hdmirx_dev->cur_fmt_fourcc;
	hdmirx_set_fmt(stream, &pixm, false);
	f->fmt.pix_mp = stream->pixm;

	return 0;
}

static int hdmirx_enum_fmt_vid_cap_mplane(struct file *file, void *priv,
					 struct v4l2_fmtdesc *f)
{
	const struct hdmirx_output_fmt *fmt;

	if (f->index >= ARRAY_SIZE(g_out_fmts))
		return -EINVAL;

	fmt = &g_out_fmts[f->index];
	f->pixelformat = fmt->fourcc;

	return 0;
}

static int hdmirx_s_dv_timings(struct file *file, void *_fh,
				 struct v4l2_dv_timings *timings)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	if (!timings)
		return -EINVAL;

	if (debug)
		v4l2_print_dv_timings(hdmirx_dev->v4l2_dev.name, "hdmirx_s_dv_timings: ", timings, false);

	if (!v4l2_valid_dv_timings(timings, &hdmirx_timings_cap, NULL, NULL)) {
		v4l2_dbg(1, debug, v4l2_dev, "%s: timings out of range\n", __func__);
		return -ERANGE;
	}

	/* Check if the timings are part of the CEA-861 timings. */
	if (!v4l2_find_dv_timings_cap(timings, &hdmirx_timings_cap,
				      0, NULL, NULL))
		return -EINVAL;

	if (v4l2_match_dv_timings(&hdmirx_dev->timings, timings, 0, false)) {
		v4l2_dbg(1, debug, v4l2_dev, "%s: no change\n", __func__);
		return 0;
	}

	/*
	 * Changing the timings implies a format change, which is not allowed
	 * while buffers for use with streaming have already been allocated.
	 */
	if (vb2_is_busy(&stream->buf_queue))
		return -EBUSY;

	hdmirx_dev->timings = *timings;
	/* Update the internal format */
	hdmirx_set_fmt(stream, &stream->pixm, false);

	return 0;
}

static int hdmirx_g_dv_timings(struct file *file, void *_fh,
				 struct v4l2_dv_timings *timings)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	u32 dma_cfg1;

	if (port_no_link(hdmirx_dev)) {
		v4l2_err(v4l2_dev, "%s port has no link!\n", __func__);
		return -ENOLINK;
	}

	if (signal_not_lock(hdmirx_dev)) {
		v4l2_err(v4l2_dev, "%s signal is not locked!\n", __func__);
		return -ENOLCK;
	}

	*timings = hdmirx_dev->timings;
	dma_cfg1 = hdmirx_readl(hdmirx_dev, DMA_CONFIG1);
	v4l2_dbg(1, debug, v4l2_dev, "%s: pix_fmt: %s, DMA_CONFIG1:%#x\n", __func__, pix_fmt_str[hdmirx_dev->pix_fmt], dma_cfg1);

	return 0;
}

static int hdmirx_enum_dv_timings(struct file *file, void *_fh,
				    struct v4l2_enum_dv_timings *timings)
{
	return v4l2_enum_dv_timings_cap(timings, &hdmirx_timings_cap, NULL, NULL);
}

static int hdmirx_query_dv_timings(struct file *file, void *_fh, struct v4l2_dv_timings *timings)
{
	int ret;
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	if (port_no_link(hdmirx_dev)) {
		v4l2_err(v4l2_dev, "%s port has no link!\n", __func__);
		return -ENOLINK;
	}

	if (signal_not_lock(hdmirx_dev)) {
		v4l2_err(v4l2_dev, "%s signal is not locked!\n", __func__);
		return -ENOLCK;
	}

	ret = hdmirx_get_detected_timings(hdmirx_dev, timings, false);
	if (ret)
		return ret;

	if (debug)
		v4l2_print_dv_timings(hdmirx_dev->v4l2_dev.name, "query_dv_timings: ", timings, false);

	if (!v4l2_valid_dv_timings(timings, &hdmirx_timings_cap, NULL, NULL)) {
		v4l2_dbg(1, debug, v4l2_dev, "%s: timings out of range\n", __func__);
		return -ERANGE;
	}

	return 0;
}

static void hdmirx_get_timings(struct rk_hdmirx_dev *hdmirx_dev, struct v4l2_bt_timings *bt, bool from_dma)
{
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	u32 hact, vact, htotal, vtotal, fps;
	u32 hfp, hs, hbp, vfp, vs, vbp;
	u32 val;

	if (from_dma) {
		hfp = 0;
		val = hdmirx_readl(hdmirx_dev, DMA_STATUS2);
		hact = (val >> 16) & 0xffff;
		vact = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, DMA_STATUS3);
		htotal = (val >> 16) & 0xffff;
		vtotal = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, DMA_STATUS4);
		hs = (val >> 16) & 0xffff;
		vs = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, DMA_STATUS5);
		hbp = (val >> 16) & 0xffff;
		vbp = val & 0xffff;
	} else {
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS1);
		hs = (val >> 16) & 0xffff;
		hfp = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS2);
		hbp = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS3);
		htotal = (val >> 16) & 0xffff;
		hact = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS4);
		vs = (val >> 16) & 0xffff;
		vfp = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS5);
		vbp = val & 0xffff;
		val = hdmirx_readl(hdmirx_dev, VMON_STATUS6);
		vtotal = (val >> 16) & 0xffff;
		vact = val & 0xffff;
	}

	if (hdmirx_dev->pix_fmt == HDMIRX_YUV420) {
		htotal *= 2;
		hfp *= 2;
		hbp *= 2;
		hs *= 2;
		if (!from_dma)
			hact *= 2;
	}

	if (from_dma) {
		hfp = htotal - hact - hs - hbp;
		vfp = vtotal - vact - vs - vbp;
	}

	if (!from_dma)
		hact = (hact * 24) / hdmirx_dev->color_depth;

	if (htotal && vtotal)
		fps = (bt->pixelclock + (htotal * vtotal) / 2) / (htotal * vtotal);
	else
		fps = 0;
	bt->width = hact;
	bt->height = vact;
	bt->hfrontporch = hfp;
	bt->hsync = hs;
	bt->hbackporch = hbp;
	bt->vfrontporch = vfp;
	bt->vsync = vs;
	bt->vbackporch = vbp;
	hdmirx_dev->fps = fps;

	if (bt->interlaced == V4L2_DV_INTERLACED) {
		bt->height *= 2;
		bt->il_vfrontporch = bt->vfrontporch;
		bt->il_vsync = bt->vsync + 1;
		bt->il_vbackporch = bt->vbackporch;
	}

	v4l2_dbg(1, debug, v4l2_dev, "get timings from %s\n", from_dma ? "dma" : "ctrl");
	v4l2_dbg(1, debug, v4l2_dev,
		 "act:%ux%u%s, total:%ux%u, fps:%u, pixclk:%llu\n",
		 bt->width, bt->height, bt->interlaced ? "i" : "p",
		 htotal, vtotal, fps, bt->pixelclock);

	v4l2_dbg(2, debug, v4l2_dev,
		 "hfp:%u, hs:%u, hbp:%u, vfp:%u, vs:%u, vbp:%u\n",
		 bt->hfrontporch, bt->hsync, bt->hbackporch,
		 bt->vfrontporch, bt->vsync, bt->vbackporch);
}

bool hdmirx_check_timing_valid(struct v4l2_bt_timings *bt)
{
	if (bt->width < 100 || bt->width > 5000 ||
	    bt->height < 100 || bt->height > 5000)
		return false;

	if (bt->hsync == 0 || bt->hsync > 500 ||
	    bt->vsync == 0 || bt->vsync > 100)
		return false;

	if (bt->hbackporch == 0 || bt->hbackporch > 3000 ||
	    bt->vbackporch == 0 || bt->vbackporch > 3000)
		return false;

	if (bt->hfrontporch == 0 || bt->hfrontporch > 3000 ||
	    bt->vfrontporch == 0 || bt->vfrontporch > 3000)
		return false;

	return true;
}

void hdmirx_get_colordepth(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 val, color_depth_reg;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	val = hdmirx_readl(hdmirx_dev, DMA_STATUS11);
	color_depth_reg = (val & HDMIRX_COLOR_DEPTH_MASK) >> 3;

	switch (color_depth_reg) {
	case 0x4:
		hdmirx_dev->color_depth = 24;
		break;
	case 0x5:
		hdmirx_dev->color_depth = 30;
		break;
	case 0x6:
		hdmirx_dev->color_depth = 36;
		break;
	case 0x7:
		hdmirx_dev->color_depth = 48;
		break;

	default:
		hdmirx_dev->color_depth = 24;
		break;
	}

	v4l2_dbg(1, debug, v4l2_dev, "%s: color_depth: %d, reg_val:%d\n", __func__, hdmirx_dev->color_depth, color_depth_reg);
}

void hdmirx_get_pix_fmt(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 val;
	int timeout = 10;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

try_loop:
	val = hdmirx_readl(hdmirx_dev, DMA_STATUS11);
	hdmirx_dev->pix_fmt = val & HDMIRX_FORMAT_MASK;

	switch (hdmirx_dev->pix_fmt) {
	case HDMIRX_RGB888:
		hdmirx_dev->cur_fmt_fourcc = V4L2_PIX_FMT_BGR24;
		break;
	case HDMIRX_YUV422:
		hdmirx_dev->cur_fmt_fourcc = V4L2_PIX_FMT_NV16;
		break;
	case HDMIRX_YUV444:
		hdmirx_dev->cur_fmt_fourcc = V4L2_PIX_FMT_NV24;
		break;
	case HDMIRX_YUV420:
		hdmirx_dev->cur_fmt_fourcc = V4L2_PIX_FMT_NV12;
		break;

	default:
		if (timeout-- > 0) {
			usleep_range(200 * 1000, 200 * 1010);
			v4l2_err(v4l2_dev, "%s: get format failed, read again!\n", __func__);
			goto try_loop;
		}
		hdmirx_dev->pix_fmt = HDMIRX_RGB888;
		hdmirx_dev->cur_fmt_fourcc = V4L2_PIX_FMT_BGR24;
		v4l2_err(v4l2_dev,
			"%s: err pix_fmt: %d, set RGB888 as default\n",
			__func__, hdmirx_dev->pix_fmt);
		break;
	}

	/*
	 * set avmute value to black
	 * RGB:    R:bit[47:40],  G:bit[31:24],  B:bit[15:8]
	 * YUV444: Y:bit[47:40],  U:bit[31:24],  V:bit[15:8]
	 * YUV422: Y:bit[47:40], UV:bit[15:8]
	 * YUV420: Y:bit[47:40],  Y:bit[31:24], UV:bit[15:8]
	 */
	if (hdmirx_dev->pix_fmt == HDMIRX_RGB888) {
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_H, 0x0);
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_L, 0x0);
	} else if (hdmirx_dev->pix_fmt == HDMIRX_YUV444) {
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_H, 0x0);
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_L, 0x80008000);
	} else {
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_H, 0x0);
		hdmirx_writel(hdmirx_dev, VIDEO_MUTE_VALUE_L, 0x00008000);
	}

	v4l2_dbg(1, debug, v4l2_dev, "%s: pix_fmt: %s\n", __func__, pix_fmt_str[hdmirx_dev->pix_fmt]);
}

void hdmirx_get_color_space(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 val;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	/*
	 * Note: PKTDEC_AVIIF_PB3_0 contents only updated after
	 * reading pktdec_aviif_ph2_1 unless snapshot feature
	 * is disabled using pktdec_snapshot_bypass
	 */
	hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PH2_1);
	val = hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PB3_0);
	hdmirx_dev->cur_color_space = (val & EXTEND_COLORIMETRY) >> 28;

	v4l2_dbg(2, debug, v4l2_dev, "%s: video standard: %s\n", __func__, hdmirx_color_space[hdmirx_dev->cur_color_space]);
}

void hdmirx_get_color_range(struct rk_hdmirx_dev *hdmirx_dev)
{
	u32 val;
	int color_range;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	/*
	 * Note: PKTDEC_AVIIF_PB3_0 contents only updated after
	 * reading pktdec_aviif_ph2_1 unless snapshot feature
	 * is disabled using pktdec_snapshot_bypass
	 */
	hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PH2_1);
	val = hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PB3_0);
	color_range = (val & RGB_QUANTIZATION_RANGE) >> 26;
	if (hdmirx_dev->pix_fmt != HDMIRX_RGB888) {
		hdmirx_dev->cur_color_range = color_range;
	} else {
		if (color_range != HDMIRX_DEFAULT_RANGE) {
			hdmirx_dev->cur_color_range = color_range;
		} else {
			(hdmirx_dev->cur_vic) ?
			(hdmirx_dev->cur_color_range = HDMIRX_LIMIT_RANGE) :
			(hdmirx_dev->cur_color_range = HDMIRX_FULL_RANGE);
		}
	}

	v4l2_dbg(2, debug, v4l2_dev, "%s: color_range: %s\n", __func__,
		(hdmirx_dev->cur_color_range == HDMIRX_DEFAULT_RANGE) ? "default" :
		(hdmirx_dev->cur_color_range == HDMIRX_FULL_RANGE ? "full" : "limit"));
}

static int hdmirx_get_detected_timings(struct rk_hdmirx_dev *hdmirx_dev, struct v4l2_dv_timings *timings, bool from_dma)
{
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct v4l2_bt_timings *bt = &timings->bt;
	u32 field_type, color_depth, deframer_st;
	u32 val, tmdsqpclk_freq, pix_clk;
	u64 tmp_data, tmds_clk;

	memset(timings, 0, sizeof(struct v4l2_dv_timings));
	timings->type = V4L2_DV_BT_656_1120;

	val = hdmirx_readl(hdmirx_dev, DMA_STATUS11);
	field_type = (val & HDMIRX_TYPE_MASK) >> 7;
	hdmirx_get_pix_fmt(hdmirx_dev);
	/* VIC must be read before color range: the RGB default-range
	 * heuristic in hdmirx_get_color_range() depends on cur_vic.
	 */
	hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PH2_1);
	val = hdmirx_readl(hdmirx_dev, PKTDEC_AVIIF_PB7_4);
	hdmirx_dev->cur_vic =  val & VIC_VAL_MASK;
	hdmirx_get_color_range(hdmirx_dev);
	hdmirx_get_color_space(hdmirx_dev);
	bt->interlaced = field_type & BIT(0) ? V4L2_DV_INTERLACED : V4L2_DV_PROGRESSIVE;
	hdmirx_get_colordepth(hdmirx_dev);
	color_depth = hdmirx_dev->color_depth;
	deframer_st = hdmirx_readl(hdmirx_dev, DEFRAMER_STATUS);
	hdmirx_dev->is_dvi_mode = deframer_st & OPMODE_STS_MASK ? false : true;
	tmdsqpclk_freq = hdmirx_readl(hdmirx_dev, CMU_TMDSQPCLK_FREQ);
	tmds_clk = tmdsqpclk_freq * 4 * 1000U;
	tmp_data = tmds_clk * 24;
	do_div(tmp_data, color_depth);
	pix_clk = tmp_data;
	bt->pixelclock = tmp_data;
	if (hdmirx_dev->pix_fmt == HDMIRX_YUV420)
		bt->pixelclock *= 2;
	hdmirx_get_timings(hdmirx_dev, bt, from_dma);

	v4l2_dbg(2, debug, v4l2_dev, "tmp_data:%llu, pix_clk:%d\n", tmds_clk, pix_clk);
	v4l2_dbg(1, debug, v4l2_dev, "interlace:%d, fmt:%d, vic:%d, color:%d, mode:%s\n", bt->interlaced, hdmirx_dev->pix_fmt, hdmirx_dev->cur_vic, hdmirx_dev->color_depth, hdmirx_dev->is_dvi_mode ? "dvi" : "hdmi");
	v4l2_dbg(2, debug, v4l2_dev, "deframer_st:%#x\n", deframer_st);

	if (!hdmirx_check_timing_valid(bt))
		return -EINVAL;

	return 0;
}

static int hdmirx_try_to_get_timings(struct rk_hdmirx_dev *hdmirx_dev,
		struct v4l2_dv_timings *timings, int try_cnt)
{
	int i, cnt = 0, ret = 0;
	bool from_dma = false;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	u32 last_w, last_h;
	struct v4l2_bt_timings *bt = &timings->bt;
	enum hdmirx_pix_fmt last_fmt;

	last_w = 0;
	last_h = 0;
	last_fmt = HDMIRX_RGB888;

	for (i = 0; i < try_cnt; i++) {
		ret = hdmirx_get_detected_timings(hdmirx_dev, timings, from_dma);

		if ((last_w == 0) && (last_h == 0)) {
			last_w = bt->width;
			last_h = bt->height;
		}

		if (ret || (last_w != bt->width) || (last_h != bt->height)
			|| (last_fmt != hdmirx_dev->pix_fmt))
			cnt = 0;
		else
			cnt++;

		if (cnt >= 8)
			break;

		last_w = bt->width;
		last_h = bt->height;
		last_fmt = hdmirx_dev->pix_fmt;
		usleep_range(10*1000, 10*1100);
	}

	if (try_cnt > 8 && cnt < 8) {
		v4l2_dbg(1, debug, v4l2_dev, "%s: res not stable!\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}

void hdmirx_format_change(struct rk_hdmirx_dev *hdmirx_dev)
{
	struct v4l2_dv_timings timings;
	struct hdmirx_stream *stream = &hdmirx_dev->stream;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	const struct v4l2_event ev_src_chg = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (hdmirx_try_to_get_timings(hdmirx_dev, &timings, 20)) {
		schedule_delayed_work_on(hdmirx_dev->bound_cpu,
				&hdmirx_dev->delayed_work_hotplug,
				msecs_to_jiffies(1000));
		return;
	}

	if (!v4l2_match_dv_timings(&hdmirx_dev->timings, &timings, 0, false)) {
		/* automatically set timing rather than set by userspace */
		hdmirx_dev->timings = timings;
		v4l2_print_dv_timings(hdmirx_dev->v4l2_dev.name,
				"hdmirx_format_change: New format: ",
				&timings, false);
	}

	hdmirx_dev->get_timing = true;
	v4l2_dbg(1, debug, v4l2_dev, "%s: queue res_chg_event\n", __func__);
	v4l2_event_queue(&stream->vdev, &ev_src_chg);
}

static int hdmirx_dv_timings_cap(struct file *file, void *fh,
				   struct v4l2_dv_timings_cap *cap)
{
	*cap = hdmirx_timings_cap;

	return 0;
}

static int hdmirx_enum_input(struct file *file, void *priv,
			    struct v4l2_input *input)
{
	if (input->index > 0)
		return -EINVAL;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->std = 0;
	strscpy(input->name, "hdmirx", sizeof(input->name));
	input->capabilities = V4L2_IN_CAP_DV_TIMINGS;

	return 0;
}

static int hdmirx_get_edid(struct file *file, void *fh,
		struct v4l2_edid *edid)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;

	memset(edid->reserved, 0, sizeof(edid->reserved));

	if (edid->pad != 0)
		return -EINVAL;

	if (edid->start_block == 0 && edid->blocks == 0) {
		edid->blocks = hdmirx_dev->edid_blocks_written;
		return 0;
	}

	if (hdmirx_dev->edid_blocks_written == 0)
		return -ENODATA;

	if (edid->start_block >= hdmirx_dev->edid_blocks_written ||
			edid->blocks == 0)
		return -EINVAL;

	if (edid->start_block + edid->blocks > hdmirx_dev->edid_blocks_written)
		edid->blocks = hdmirx_dev->edid_blocks_written - edid->start_block;

	memcpy(edid->edid, &hdmirx_dev->edid, edid->blocks * EDID_BLOCK_SIZE);

	v4l2_dbg(1, debug, v4l2_dev, "%s: Read EDID: =====\n", __func__);
	if (debug > 0)
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_NONE, 16, 1,
			edid->edid, edid->blocks * EDID_BLOCK_SIZE, false);

	return 0;
}

static int hdmirx_set_edid(struct file *file, void *fh,
		struct v4l2_edid *edid)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;

	disable_irq(hdmirx_dev->hdmi_irq);
	disable_irq(hdmirx_dev->dma_irq);
	sip_fiq_control(RK_SIP_FIQ_CTRL_FIQ_DIS, RK_IRQ_HDMIRX_HDMI, 0);

	/*
	 * Flush any in-flight plug/res-change worker (they take work_lock)
	 * before reconfiguring, then hold work_lock so the plugout/EDID
	 * rewrite cannot race a worker re-arming the PHY.
	 */
	cancel_delayed_work_sync(&hdmirx_dev->delayed_work_hotplug);
	cancel_delayed_work_sync(&hdmirx_dev->delayed_work_res_change);

	mutex_lock(&hdmirx_dev->work_lock);
	if (tx_5v_power_present(hdmirx_dev))
		hdmirx_plugout(hdmirx_dev);
	hdmirx_write_edid(hdmirx_dev, edid, false);
	hdmirx_dev->edid_version = HDMIRX_EDID_USER;
	mutex_unlock(&hdmirx_dev->work_lock);

	enable_irq(hdmirx_dev->hdmi_irq);
	enable_irq(hdmirx_dev->dma_irq);
	sip_fiq_control(RK_SIP_FIQ_CTRL_FIQ_EN, RK_IRQ_HDMIRX_HDMI, 0);
	schedule_delayed_work_on(hdmirx_dev->bound_cpu,
				 &hdmirx_dev->delayed_work_hotplug,
				 msecs_to_jiffies(1000));

	return 0;
}

static int
hdmirx_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		if (fh->vdev->vfl_dir == VFL_DIR_RX)
			return v4l2_src_change_event_subscribe(fh, sub);
		break;
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	case RK_HDMIRX_V4L2_EVENT_SIGNAL_LOST:
	case RK_HDMIRX_V4L2_EVENT_AUDIOINFO:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		break;
	}

	return -EINVAL;
}

static long hdmirx_ioctl_default(struct file *file, void *fh,
				 bool valid_prio, unsigned int cmd, void *arg)
{
	struct hdmirx_stream *stream = video_drvdata(file);
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	long ret = 0;
	bool hpd;
	enum mute_type type;
	enum audio_stat stat;

	if (!arg)
		return -EINVAL;

	switch (cmd) {
	// case RK_HDMIRX_CMD_GET_FPS:
	// 	*(int *)arg = hdmirx_dev->get_timing ? hdmirx_dev->fps : 0;
	// 	break;
	// case RK_HDMIRX_CMD_GET_SIGNAL_STABLE_STATUS:
	// 	*(int *)arg = hdmirx_dev->get_timing;
	// 	break;
	// case RK_HDMIRX_CMD_SET_HPD:
	// 	hpd = *(int *)arg ? true : false;
	// 	hdmirx_hpd_ctrl(hdmirx_dev, hpd);
	// 	break;
	// case RK_HDMIRX_CMD_SET_MUTE:
	// 	type = *(int *)arg;
	// 	hdmirx_set_mute(hdmirx_dev, type);
	// 	break;
	// case RK_HDMIRX_CMD_SOFT_RESET:
	// 	hdmirx_reset_all(hdmirx_dev);
	// 	schedule_delayed_work_on(hdmirx_dev->bound_cpu,
	// 				 &hdmirx_dev->delayed_work_hotplug,
	// 				 msecs_to_jiffies(1000));
	// 	break;
	// case RK_HDMIRX_CMD_GET_HDCP_STATUS:
	// 	*(int *)arg = hdmirx_get_hdcp_auth_status(hdmirx_dev);
	// 	break;
	// case RK_HDMIRX_CMD_SET_AUDIO_STATE:
	// 	stat = *(enum audio_stat *)arg;
	// 	hdmirx_audio_set_state(hdmirx_dev, stat);
	// 	break;
	// case RK_HDMIRX_CMD_RESET_AUDIO_FIFO:
	// 	hdmirx_audio_fifo_init(hdmirx_dev);
	// 	break;
	// case RK_HDMIRX_CMD_GET_INPUT_MODE:
	// 	*(int *)arg = hdmirx_dev->is_dvi_mode ? MODE_DVI : MODE_HDMI;
	// 	break;
	// case RK_HDMIRX_CMD_GET_COLOR_RANGE:
	// 	hdmirx_get_color_range(hdmirx_dev);
	// 	*(int *)arg = hdmirx_dev->cur_color_range;
	// 	break;
	// case RK_HDMIRX_CMD_GET_COLOR_SPACE:
	// 	hdmirx_get_color_space(hdmirx_dev);
	// 	*(int *)arg = hdmirx_dev->cur_color_space;
	// 	break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

// ---------------------- video device -------------------------
static const struct v4l2_ioctl_ops hdmirx_v4l2_ioctl_ops = {
	.vidioc_querycap = hdmirx_querycap,
	.vidioc_try_fmt_vid_cap_mplane = hdmirx_try_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = hdmirx_s_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_cap_mplane = hdmirx_g_fmt_vid_cap_mplane,
	.vidioc_enum_fmt_vid_cap = hdmirx_enum_fmt_vid_cap_mplane,

	.vidioc_s_dv_timings = hdmirx_s_dv_timings,
	.vidioc_g_dv_timings = hdmirx_g_dv_timings,
	.vidioc_enum_dv_timings = hdmirx_enum_dv_timings,
	.vidioc_query_dv_timings = hdmirx_query_dv_timings,
	.vidioc_dv_timings_cap = hdmirx_dv_timings_cap,
	.vidioc_enum_input = hdmirx_enum_input,
	.vidioc_g_edid = hdmirx_get_edid,
	.vidioc_s_edid = hdmirx_set_edid,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = hdmirx_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = hdmirx_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_default = hdmirx_ioctl_default,
};

static const struct v4l2_file_operations hdmirx_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

int hdmirx_register_stream_vdev(struct hdmirx_stream *stream)
{
	struct rk_hdmirx_dev *hdmirx_dev = stream->hdmirx_dev;
	struct v4l2_device *v4l2_dev = &hdmirx_dev->v4l2_dev;
	struct video_device *vdev = &stream->vdev;
	int ret = 0;
	char *vdev_name;

	vdev_name = HDMIRX_VDEV_NAME;
	strscpy(vdev->name, vdev_name, sizeof(vdev->name));
	INIT_LIST_HEAD(&stream->buf_head);
	spin_lock_init(&stream->vbq_lock);
	mutex_init(&stream->vlock);
	init_waitqueue_head(&stream->wq_stopped);
	stream->curr_buf = NULL;
	stream->next_buf = NULL;

	vdev->ioctl_ops = &hdmirx_v4l2_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->fops = &hdmirx_fops;
	vdev->minor = -1;
	vdev->v4l2_dev = v4l2_dev;
	vdev->lock = &stream->vlock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, stream);
	vdev->vfl_dir = VFL_DIR_RX;

	hdmirx_init_vb2_queue(&stream->buf_queue, stream, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	vdev->queue = &stream->buf_queue;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		v4l2_err(v4l2_dev,
			 "video_register_device failed with error %d\n", ret);
		return ret;
	}

	return 0;
}
