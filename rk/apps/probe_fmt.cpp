// probe_fmt.cpp — rkisp NV12 布局探针（一次性诊断工具，不入产品链路）
// 打印：G_FMT/S_FMT 协商结果、二次 S_FMT 效果、QUERYBUF 长度、DQBUF bytesused、
// 各候选 UV 偏移处的相邻字节差分（Y 平面平滑、UV 交错平面差分显著→定位 UV）。
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
  return r;
}

static void show_fmt(int fd, const char* tag) {
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  ioctl(fd, VIDIOC_G_FMT, &fmt);
  printf("%s: %ux%u bpl=%u sizeimage=%u\n", tag,
         fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
         fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
         fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
}

// 相邻字节平均差分：UV 交错平面的特征是奇偶字节跳变大
static double adj_diff(const uint8_t* p, size_t n) {
  if (n < 2) return 0;
  double s = 0;
  for (size_t i = 1; i < n; ++i) s += std::abs(int(p[i]) - int(p[i - 1]));
  return s / (n - 1);
}

int main(int argc, char** argv) {
  const char* dev = argc > 1 ? argv[1] : "/dev/video11";
  int fd = open(dev, O_RDWR);
  if (fd < 0) { perror("open"); return 1; }
  show_fmt(fd, "boot-default");

  // 第一次 S_FMT 1920x1080
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  ioctl(fd, VIDIOC_G_FMT, &fmt);
  fmt.fmt.pix_mp.width = 1920;
  fmt.fmt.pix_mp.height = 1080;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  xioctl(fd, VIDIOC_S_FMT, &fmt);
  show_fmt(fd, "after S_FMT#1");

  // 二次 S_FMT：看 bpl 是否被纠正
  xioctl(fd, VIDIOC_S_FMT, &fmt);
  show_fmt(fd, "after S_FMT#2");

  const unsigned bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  const unsigned w = fmt.fmt.pix_mp.width, h = fmt.fmt.pix_mp.height;

  v4l2_requestbuffers req{};
  req.count = 3; req.type = fmt.type; req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
  void* bufs[4] = {};
  size_t lens[4] = {};
  for (unsigned i = 0; i < req.count; ++i) {
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer qb{};
    qb.type = fmt.type; qb.memory = V4L2_MEMORY_MMAP; qb.index = i;
    qb.m.planes = planes; qb.length = 1;
    if (xioctl(fd, VIDIOC_QUERYBUF, &qb) < 0) { perror("QUERYBUF"); return 1; }
    lens[i] = planes[0].length;
    bufs[i] = mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, planes[0].m.mem_offset);
    if (bufs[i] == MAP_FAILED) { perror("mmap"); return 1; }
    v4l2_buffer q{}; q.type = fmt.type; q.memory = V4L2_MEMORY_MMAP; q.index = i;
    xioctl(fd, VIDIOC_QBUF, &q);
  }
  printf("QUERYBUF len=%zu (期望 stride 布局 %lu / 紧凑布局 %lu)\n", lens[0],
         (unsigned long)((size_t)bpl * h * 3 / 2),
         (unsigned long)((size_t)w * h * 3 / 2));

  int type = fmt.type;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
  v4l2_plane planes[VIDEO_MAX_PLANES]{};
  v4l2_buffer db{};
  db.type = fmt.type; db.memory = V4L2_MEMORY_MMAP; db.m.planes = planes; db.length = 1;
  if (xioctl(fd, VIDIOC_DQBUF, &db) < 0) { perror("DQBUF"); return 1; }
  printf("DQBUF bytesused=%llu flags=%08x\n",
         (unsigned long long)planes[0].bytesused, db.flags);

  const uint8_t* p = static_cast<const uint8_t*>(bufs[db.index]);
  const size_t L = lens[db.index];
  printf("相邻字节差分（UV 交错平面应显著高于平滑 Y）：\n");
  printf("  Y 区   [0..64K)         adj=%.1f\n", adj_diff(p, 65536));
  if ((size_t)w * h + 65536 <= L)
    printf("  紧凑UV [%zu..+64K)     adj=%.1f\n", (size_t)w * h,
           adj_diff(p + (size_t)w * h, 65536));
  if ((size_t)bpl * h + 65536 <= L)
    printf("  对齐UV [%zu..+64K)     adj=%.1f\n", (size_t)bpl * h,
           adj_diff(p + (size_t)bpl * h, 65536));
  else
    printf("  对齐UV 偏移 %zu 超出缓冲 %zu（即当前崩溃根因）\n", (size_t)bpl * h, L);
  // 缓冲尾部 64K 差分：判断数据实际写到多远
  printf("  尾部   [%zu-64K..%zu)  adj=%.1f\n", L - 65536, L, adj_diff(p + L - 65536, 65536));
  return 0;
}
