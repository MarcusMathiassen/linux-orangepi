#ifndef __RK_HDMIRX_VIDEO_H__
#define __RK_HDMIRX_VIDEO_H__

struct hdmirx_stream;

int hdmirx_register_stream_vdev(struct hdmirx_stream *stream);

irqreturn_t hdmirx_dma_irq_handler(int irq, void *dev_id);

void hdmirx_format_change(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_get_color_range(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_get_color_space(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_get_eotf(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_get_pix_fmt(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_get_colordepth(struct rk_hdmirx_dev *hdmirx_dev);

#endif
