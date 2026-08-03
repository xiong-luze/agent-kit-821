/*
 * cam_v4l2.h —— V4L2 拍照，输出 JPEG
 *
 * 板子是 IPC SoC，MIPI 摄像头（GC2083，2-lane），内核 vin 驱动注册成 /dev/video0。
 * 打开时会枚举并打印所有支持的像素格式（顺带就是探针），按 MJPEG > NV12 > YUYV
 * 的顺序挑：
 *   - MJPEG：驱动直出 JPEG，零转换（最省 CPU）
 *   - NV12 / YUYV：转 RGB24 再用 libjpeg-turbo 软编码
 * V821 有硬件 JPEG 编码器，但要走 MPP，而官方固件没编 MPP 音视频
 * （/usr/lib/eyesee-mpp 里只有 librgb_ctrl.so），所以只能软编。
 */
#ifndef CAMERA_V4L2_H
#define CAMERA_V4L2_H

#include <stdint.h>
#include <stddef.h>

/* 打开摄像头并开流。0 成功；失败会打印原因（包括枚举到的格式列表）。 */
int  cam_open(const char *dev, int want_w, int want_h);
void cam_close(void);

/* 抓一帧并给出 JPEG。*jpeg 由内部持有，下次调用前有效，不要 free。
 * 会先丢 4 帧等 AE/AWB 收敛。0 成功。 */
int  cam_capture_jpeg(const uint8_t **jpeg, size_t *len, int *w, int *h);

/* 协商到的取帧分辨率（cam_open 之后有效）。 */
void cam_get_size(int *w, int *h);

/* 抓一帧，面积平均缩放成 dst_w×dst_h 的 RGB24 拷进 dst（需 dst_w*dst_h*3 字节）。
 * 只做缩放，不管宽高比 —— 要不要 letterbox 由调用方决定（那是视觉侧的事）。
 * 不编码、不丢帧预热，给本地视觉推理连续调用用。0 成功。
 * 与 cam_capture_jpeg 内部互斥，两者可在不同线程调。 */
int  cam_capture_rgb(uint8_t *dst, int dst_w, int dst_h);

#endif /* CAMERA_V4L2_H */
