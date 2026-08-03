/* cam_v4l2.c —— 见 cam_v4l2.h
 *
 * ★ 两个必须按这块板子来的地方：
 * 1) 全志 vin 是 **MPLANE** 驱动（vin_video.c 里注册的是
 *    vidioc_enum_fmt_vid_cap_mplane），所以 buf type 必须用
 *    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE、格式走 fmt.pix_mp、
 *    buffer 带 m.planes[]。用单平面 API 的话 ENUM_FMT 直接返回空。
 * 2) 驱动的格式表（vin_core.c 的 vin_formats[]）里**没有 MJPEG**，所以
 *    JPEG 只能软编码；也**没有 YUYV**。可用的是 RGB24 / NV12 / NV21 /
 *    YUV420 / YVU420，其中 RGB24 能零转换直接喂给编码器。
 *    V821 有硬件 JPEG 编码器，但要走 MPP，而官方固件没编 MPP 音视频。
 * 另外：厂商驱动经常不实现 ENUM_FMT，所以枚举只用来打印诊断信息，
 * **不作为能力判断** —— 真正靠逐个 S_FMT 试。
 */
#include "camera_v4l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <pthread.h>

#include "turbojpeg.h"

#define NBUF     3
#define MAXPLANE 3
#define BTYPE    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int      g_fd = -1;
static uint32_t g_fmt = 0;
static int      g_w = 0, g_h = 0;
static int      g_nplane = 1;
static void    *g_map[NBUF][MAXPLANE];
static size_t   g_maplen[NBUF][MAXPLANE];
static int      g_nbuf = 0;

static uint8_t *g_rgb = NULL;
static uint8_t *g_jpg = NULL;
static size_t   g_jpglen = 0;
static int      g_jpg_from_tj = 0;
static tjhandle g_tj = NULL;

/* 拍照（MCP 线程）和本地视觉推理（presence 线程）共用同一个 /dev/video0。
 * 只开一次设备，靠这把锁串行化取帧 —— 两个线程各自 DQBUF 会乱序拿到 buffer。 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static const char *fourcc(uint32_t f, char *b)
{
    b[0] = f & 0xff; b[1] = (f >> 8) & 0xff; b[2] = (f >> 16) & 0xff;
    b[3] = (f >> 24) & 0xff; b[4] = 0;
    return b;
}

/* 候选格式。NV21 放第一 —— 全志官方样例 sample_driverVipp 的默认就是它，
 * 是这块平台走得最通的路。RGB24 放最后：S_FMT 会接受（驱动只查静态表
 * vin_formats[]，不校验 ISP 实际能力），但 STREAMON 后一帧都出不来。 */
static const uint32_t CAND[] = {
    V4L2_PIX_FMT_NV21,
    V4L2_PIX_FMT_NV12,
    V4L2_PIX_FMT_YUV420,
    V4L2_PIX_FMT_YVU420,
    V4L2_PIX_FMT_RGB24,
};

/* 全志私有：v4l2_streamparm.parm.capture.capturemode 的取值
 * （bsp/drivers/vin/vin_test/sunxi_camera_v2.h）。不设的话 sensor 不会被配成
 * 正确的工作模式 —— 表现就是 STREAMON 成功但 DQBUF 永远超时。 */
#define V4L2_MODE_VIDEO 0x0002

/* 拆掉当前格式的 buffer/流，供换格式重试 */
static void teardown_stream(void)
{
    if (g_fd < 0) return;
    enum v4l2_buf_type t = BTYPE;
    xioctl(g_fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < g_nbuf; i++)
        for (int p = 0; p < MAXPLANE; p++)
            if (g_map[i][p] && g_map[i][p] != MAP_FAILED) {
                munmap(g_map[i][p], g_maplen[i][p]);
                g_map[i][p] = NULL;
            }
    g_nbuf = 0;
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 0; rb.type = BTYPE; rb.memory = V4L2_MEMORY_MMAP;
    xioctl(g_fd, VIDIOC_REQBUFS, &rb);
}

/* 抓一帧丢掉，纯粹用来验证这个格式真的能出流。0 = 能。 */
static int probe_one_frame(void)
{
    struct v4l2_plane pl[MAXPLANE];
    struct v4l2_buffer b;
    fd_set fds; FD_ZERO(&fds); FD_SET(g_fd, &fds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    if (select(g_fd + 1, &fds, NULL, NULL, &tv) <= 0) return -1;

    memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
    b.type = BTYPE; b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl; b.length = (uint32_t)g_nplane;
    if (xioctl(g_fd, VIDIOC_DQBUF, &b) != 0) return -1;
    xioctl(g_fd, VIDIOC_QBUF, &b);
    return 0;
}

/* 按给定格式完整配置一遍并验证能出流。0 = 成功（流保持开着）。 */
static int try_format(uint32_t pixfmt, int want_w, int want_h)
{
    char cc[5];

    /* ★ S_PARM 必须在 S_FMT 之前，capturemode 要设成 V4L2_MODE_VIDEO。
     * 这是全志官方样例 sample_driverVipp 的顺序。不设 capturemode 的话
     * sensor 不会被配成正确的工作模式 —— 现象就是 STREAMON 返回成功、
     * 但 DQBUF 永远超时（上一版就卡在这里）。 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = BTYPE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
    parm.parm.capture.reserved[0] = 0;   /* 0: 找最接近的 sensor win */
    parm.parm.capture.reserved[1] = 0;   /* 0: normal（非 wdr/comanding） */
    if (xioctl(g_fd, VIDIOC_S_PARM, &parm) != 0)
        printf("[相机] S_PARM 失败（继续试）：%s\n", strerror(errno));

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = BTYPE;
    f.fmt.pix_mp.width       = (uint32_t)want_w;
    f.fmt.pix_mp.height      = (uint32_t)want_h;
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    f.fmt.pix_mp.colorspace  = V4L2_COLORSPACE_DEFAULT;
    if (xioctl(g_fd, VIDIOC_S_FMT, &f) != 0) {
        printf("[相机] %s：S_FMT 被拒\n", fourcc(pixfmt, cc));
        return -1;
    }
    if (f.fmt.pix_mp.pixelformat != pixfmt) {
        printf("[相机] %s：驱动改成了 %s\n", fourcc(pixfmt, cc),
               fourcc(f.fmt.pix_mp.pixelformat, cc));
        return -1;
    }
    g_fmt    = f.fmt.pix_mp.pixelformat;
    g_w      = (int)f.fmt.pix_mp.width;
    g_h      = (int)f.fmt.pix_mp.height;
    g_nplane = f.fmt.pix_mp.num_planes ? (int)f.fmt.pix_mp.num_planes : 1;
    if (g_nplane > MAXPLANE) g_nplane = MAXPLANE;

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = NBUF; rb.type = BTYPE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(g_fd, VIDIOC_REQBUFS, &rb) != 0 || rb.count < 1) {
        printf("[相机] %s：REQBUFS 失败\n", fourcc(pixfmt, cc));
        return -1;
    }
    g_nbuf = (int)rb.count;

    for (int i = 0; i < g_nbuf; i++) {
        struct v4l2_plane pl[MAXPLANE];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = BTYPE; b.memory = V4L2_MEMORY_MMAP; b.index = (uint32_t)i;
        b.m.planes = pl; b.length = (uint32_t)g_nplane;
        if (xioctl(g_fd, VIDIOC_QUERYBUF, &b) != 0) goto bad;
        for (int p = 0; p < g_nplane; p++) {
            g_maplen[i][p] = pl[p].length;
            g_map[i][p] = mmap(NULL, pl[p].length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, g_fd, (off_t)pl[p].m.mem_offset);
            if (g_map[i][p] == MAP_FAILED) goto bad;
        }
        if (xioctl(g_fd, VIDIOC_QBUF, &b) != 0) goto bad;
    }

    enum v4l2_buf_type t = BTYPE;
    if (xioctl(g_fd, VIDIOC_STREAMON, &t) != 0) {
        printf("[相机] %s：STREAMON 失败（%s）\n", fourcc(pixfmt, cc),
               strerror(errno));
        goto bad;
    }

    /* ★ 关键验证：STREAMON 成功不代表出得了流。驱动的 S_FMT 只查静态格式表
     * （vin_core.c 的 vin_formats[]），不校验 ISP/sensor 实际能不能产出。
     * 所以必须真的抓一帧才算数。 */
    if (probe_one_frame() != 0) {
        printf("[相机] %s：STREAMON 成功但取不到帧，换下一个格式\n",
               fourcc(pixfmt, cc));
        goto bad;
    }
    printf("[相机] 采用 %s %dx%d（%d 个内存平面）—— 已验证可出流\n",
           fourcc(g_fmt, cc), g_w, g_h, g_nplane);
    return 0;
bad:
    teardown_stream();
    return -1;
}

int cam_open(const char *dev, int want_w, int want_h)
{
    char cc[5];
    g_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
        fprintf(stderr, "[相机] 打不开 %s：%s\n", dev, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(g_fd, VIDIOC_QUERYCAP, &cap) == 0)
        printf("[相机] %s driver=%s card=%s caps=0x%08x\n",
               dev, cap.driver, cap.card, cap.capabilities);

    int inp = 0;
    xioctl(g_fd, VIDIOC_S_INPUT, &inp);

    /* 枚举只用于诊断打印。驱动返回的是静态格式表，不代表实际能出流。 */
    printf("[相机] 驱动声称支持：");
    int nfmt = 0;
    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc fd = { .index = (uint32_t)i, .type = BTYPE };
        if (xioctl(g_fd, VIDIOC_ENUM_FMT, &fd) != 0) break;
        if (nfmt < 12) printf("%s%s", nfmt ? ", " : "", fourcc(fd.pixelformat, cc));
        nfmt++;
    }
    printf("%s（共 %d 个，仅供参考）\n", nfmt > 12 ? " ..." : "", nfmt);

    int ok = 0;
    for (size_t i = 0; i < sizeof(CAND) / sizeof(CAND[0]); i++) {
        if (try_format(CAND[i], want_w, want_h) == 0) { ok = 1; break; }
    }
    if (!ok) {
        fprintf(stderr, "[相机] 所有候选格式都出不了流。\n"
                        "       板上查：dmesg | tail -30（看 vin/gc2083 报错）\n"
                        "       以及确认没有别的进程占着 /dev/video0\n");
        close(g_fd); g_fd = -1; return -1;
    }

    g_tj = tjInitCompress();
    if (!g_tj) { fprintf(stderr, "[相机] JPEG 编码器初始化失败\n"); goto fail; }
    if (g_fmt != V4L2_PIX_FMT_RGB24) {
        g_rgb = (uint8_t *)malloc((size_t)g_w * g_h * 3);
        if (!g_rgb) { fprintf(stderr, "[相机] RGB 缓冲分配失败\n"); goto fail; }
    }
    printf("[相机] 就绪\n");
    return 0;
fail:
    cam_close();
    return -1;
}

void cam_close(void)
{
    if (g_fd >= 0) {
        enum v4l2_buf_type t = BTYPE;
        xioctl(g_fd, VIDIOC_STREAMOFF, &t);
        for (int i = 0; i < g_nbuf; i++)
            for (int p = 0; p < MAXPLANE; p++)
                if (g_map[i][p] && g_map[i][p] != MAP_FAILED) {
                    munmap(g_map[i][p], g_maplen[i][p]);
                    g_map[i][p] = NULL;
                }
        close(g_fd); g_fd = -1;
    }
    if (g_tj)  { tjDestroy(g_tj); g_tj = NULL; }
    if (g_rgb) { free(g_rgb); g_rgb = NULL; }
    if (g_jpg) { if (g_jpg_from_tj) tjFree(g_jpg); else free(g_jpg); g_jpg = NULL; }
    g_jpglen = 0; g_nbuf = 0;
}

/* --- 转 RGB24 ----------------------------------------------------------- */
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static inline void yuv2rgb(int y, int u, int v, uint8_t *p)
{
    int c = y - 16, d = u - 128, e = v - 128;
    p[0] = clamp8((298 * c + 409 * e + 128) >> 8);
    p[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    p[2] = clamp8((298 * c + 516 * d + 128) >> 8);
}

/* NV12: Y 平面 + UV 交织(U 在前)；NV21: VU 交织 */
static void nv12_to_rgb(const uint8_t *s, uint8_t *d, int w, int h, int u_first)
{
    const uint8_t *Y = s, *UV = s + (size_t)w * h;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint8_t *c = UV + (size_t)(j / 2) * w + (i / 2) * 2;
            int u = u_first ? c[0] : c[1];
            int v = u_first ? c[1] : c[0];
            yuv2rgb(Y[(size_t)j * w + i], u, v, d + ((size_t)j * w + i) * 3);
        }
}

/* I420: Y,U,V 三平面连续；YV12: Y,V,U */
static void i420_to_rgb(const uint8_t *s, uint8_t *d, int w, int h, int u_first)
{
    size_t ys = (size_t)w * h, cs = ys / 4;
    const uint8_t *Y = s;
    const uint8_t *A = s + ys, *B = s + ys + cs;
    const uint8_t *U = u_first ? A : B, *V = u_first ? B : A;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            size_t ci = (size_t)(j / 2) * (w / 2) + (i / 2);
            yuv2rgb(Y[(size_t)j * w + i], U[ci], V[ci], d + ((size_t)j * w + i) * 3);
        }
}

/* 取一帧并转成 RGB24 放进 g_rgb。调用方须持 g_lock。
 * drop: 先丢几帧等 AE/AWB 收敛 —— 拍照要（否则常拿到过曝或全黑的图），
 *       连续检测不要（本来就一直在流，而且每次都丢 4 帧太贵）。
 * 转换完立刻把 buffer 还给驱动，所以返回后 g_rgb 与 mmap 无关。 */
static int grab_rgb_locked(int drop)
{
    if (g_fd < 0) return -1;

    struct v4l2_plane pl[MAXPLANE];
    struct v4l2_buffer b;

    for (int k = 0; k <= drop; k++) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g_fd, &fds);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        if (select(g_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            fprintf(stderr, "[相机] 取帧超时\n");
            return -1;
        }
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = BTYPE; b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl; b.length = (uint32_t)g_nplane;
        if (xioctl(g_fd, VIDIOC_DQBUF, &b) != 0) {
            fprintf(stderr, "[相机] DQBUF 失败：%s\n", strerror(errno));
            return -1;
        }
        /* 中间那些帧直接还回去，只留最后一帧 */
        if (k < drop) xioctl(g_fd, VIDIOC_QBUF, &b);
    }

    const uint8_t *frame = (const uint8_t *)g_map[b.index][0];
    if      (g_fmt == V4L2_PIX_FMT_RGB24)  memcpy(g_rgb, frame, (size_t)g_w * g_h * 3);
    else if (g_fmt == V4L2_PIX_FMT_NV12)   nv12_to_rgb(frame, g_rgb, g_w, g_h, 1);
    else if (g_fmt == V4L2_PIX_FMT_NV21)   nv12_to_rgb(frame, g_rgb, g_w, g_h, 0);
    else if (g_fmt == V4L2_PIX_FMT_YUV420) i420_to_rgb(frame, g_rgb, g_w, g_h, 1);
    else                                   i420_to_rgb(frame, g_rgb, g_w, g_h, 0);

    xioctl(g_fd, VIDIOC_QBUF, &b);
    return 0;
}

int cam_capture_jpeg(const uint8_t **jpeg, size_t *len, int *w, int *h)
{
    if (g_fd < 0) return -1;
    pthread_mutex_lock(&g_lock);

    int rc = -1;
    if (grab_rgb_locked(4) != 0) goto out;

    if (g_jpg) { if (g_jpg_from_tj) tjFree(g_jpg); else free(g_jpg); g_jpg = NULL; }
    unsigned long out_len = 0;
    if (tjCompress2(g_tj, g_rgb, g_w, 0, g_h, TJPF_RGB,
                    &g_jpg, &out_len, TJSAMP_420, 80, 0) == 0) {
        g_jpglen = out_len; g_jpg_from_tj = 1; rc = 0;
        *jpeg = g_jpg; *len = g_jpglen; *w = g_w; *h = g_h;
        printf("[相机] 拍到一张 %dx%d，JPEG %u 字节\n", g_w, g_h, (unsigned)g_jpglen);
    } else {
        fprintf(stderr, "[相机] JPEG 编码失败：%s\n", tjGetErrorStr());
    }
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void cam_get_size(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
}

int cam_capture_rgb(uint8_t *dst, int dst_w, int dst_h)
{
    if (g_fd < 0 || !dst || dst_w <= 0 || dst_h <= 0) return -1;
    pthread_mutex_lock(&g_lock);

    int rc = grab_rgb_locked(0);
    if (rc == 0) {
        /* 面积平均缩放（box filter），不是最近邻。
         * 640x480 → 320x192 是 2x2.5 倍下采样，最近邻会直接丢掉 60% 的行，
         * 人的边缘被采样噪声打碎，小目标检测器的召回掉得很厉害。
         * 平均一下多花几毫秒，相比一帧推理 167ms 可以忽略。 */
        for (int y = 0; y < dst_h; y++) {
            int sy0 = y * g_h / dst_h, sy1 = (y + 1) * g_h / dst_h;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            uint8_t *drow = dst + (size_t)y * dst_w * 3;
            for (int x = 0; x < dst_w; x++) {
                int sx0 = x * g_w / dst_w, sx1 = (x + 1) * g_w / dst_w;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                unsigned r = 0, g = 0, bl = 0, n = 0;
                for (int sy = sy0; sy < sy1; sy++) {
                    const uint8_t *s = g_rgb + ((size_t)sy * g_w + sx0) * 3;
                    for (int sx = sx0; sx < sx1; sx++, s += 3) {
                        r += s[0]; g += s[1]; bl += s[2]; n++;
                    }
                }
                drow[x * 3 + 0] = (uint8_t)(r / n);
                drow[x * 3 + 1] = (uint8_t)(g / n);
                drow[x * 3 + 2] = (uint8_t)(bl / n);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
