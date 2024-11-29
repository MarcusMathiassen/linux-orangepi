#ifndef __RK_HDMIRX_AUDIO_H__
#define __RK_HDMIRX_AUDIO_H__

struct rk_hdmirx_dev;
struct work_struct;
enum audio_stat;

int hdmirx_register_audio_device(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_unregister_audio_device(void *data);

void hdmirx_delayed_work_audio(struct work_struct *work);

void hdmirx_audio_fifo_init(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_audio_set_state(struct rk_hdmirx_dev *hdmirx_dev, enum audio_stat stat);
void hdmirx_audio_setup(struct rk_hdmirx_dev *hdmirx_dev);
void hdmirx_audio_handle_plugged_change(struct rk_hdmirx_dev *hdmirx_dev, int plugged);
void hdmirx_audio_interrupts_setup(struct rk_hdmirx_dev *hdmirx_dev, int en);

#endif
