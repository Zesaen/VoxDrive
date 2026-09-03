// v4l2_capture.cpp — V4L2 mmap 采集实现
#define VOX_LOG_TAG "rk.capture"

#include "v4l2_capture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

#include "vox_log.h"

#include <linux/videodev2.h>

namespace vox {
namespace {

// ioctl 包装：EINTR 自动重试（信号打断），其余原样返回 -1
int xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

const char* fourcc_str(uint32_t f) {
  static thread_local char s[5];
  s[0] = static_cast<char>(f & 0xff);
  s[1] = static_cast<char>((f >> 8) & 0xff);
  s[2] = static_cast<char>((f >> 16) & 0xff);
  s[3] = static_cast<char>((f >> 24) & 0xff);
  s[4] = '\0';
  return s;
}

}  // namespace

uint32_t V4L2Capture::to_v4l2_fourcc(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::NV12:
      return V4L2_PIX_FMT_NV12;
  }
  return 0;
}

V4L2Capture::V4L2Capture(const Params& params) : params_(params) {}

V4L2Capture::~V4L2Capture() { stop(); }

// ---------- 启动/停止 ----------

bool V4L2Capture::start() {
  if (running_.load()) return true;
  frame_counter_ = 0;
  dequeued_index_ = -1;

  if (!open_device()) return false;
  if (!negotiate_format()) {
    close_device();
    return false;
  }
  if (!request_and_map_buffers()) {
    unmap_buffers();
    close_device();
    return false;
  }
  if (!start_streaming()) {
    unmap_buffers();
    close_device();
    return false;
  }
  running_.store(true);
  VOX_INFO("started: %s", describe().c_str());
  return true;
}

void V4L2Capture::stop() {
  if (fd_ < 0) return;
  running_.store(false);
  // STREAMOFF 冲刷所有排队缓冲（已出队未归还的缓冲也随之作废）
  int type = static_cast<int>(v4l2_buf_type_);
  if (xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
    VOX_WARN("STREAMOFF: %s", strerror(errno));
  }
  dequeued_index_ = -1;
  unmap_buffers();
  close_device();
  VOX_INFO("stopped after %llu frames", static_cast<unsigned long long>(frame_counter_));
}

bool V4L2Capture::open_device() {
  fd_ = open(params_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    VOX_ERROR("open %s: %s", params_.device.c_str(), strerror(errno));
    return false;
  }
  v4l2_capability cap{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
    VOX_ERROR("QUERYCAP %s: %s", params_.device.c_str(), strerror(errno));
    return false;
  }
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
      !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
    VOX_ERROR("%s is not a capture device (caps=%08x)", params_.device.c_str(),
              cap.capabilities);
    return false;
  }
  v4l2_buf_type_ = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
                       ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;
  VOX_INFO("device %s: card=%s driver=%s bus=%s", params_.device.c_str(), cap.card,
           cap.driver, cap.bus_info);
  return true;
}

void V4L2Capture::close_device() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool V4L2Capture::negotiate_format() {
  v4l2_format fmt{};
  fmt.type = v4l2_buf_type_;
  if (xioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) {
    VOX_ERROR("G_FMT: %s", strerror(errno));
    return false;
  }
  if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    fmt.fmt.pix_mp.width = params_.width;
    fmt.fmt.pix_mp.height = params_.height;
    fmt.fmt.pix_mp.pixelformat = to_v4l2_fourcc(params_.format);
  } else {
    fmt.fmt.pix.width = params_.width;
    fmt.fmt.pix.height = params_.height;
    fmt.fmt.pix.pixelformat = to_v4l2_fourcc(params_.format);
  }
  if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
    VOX_ERROR("S_FMT %ux%u %s: %s", params_.width, params_.height,
              fourcc_str(to_v4l2_fourcc(params_.format)), strerror(errno));
    return false;
  }
  // 以协商后的实际值为准（尺寸/stride 可能被驱动调整）
  if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    params_.width = fmt.fmt.pix_mp.width;
    params_.height = fmt.fmt.pix_mp.height;
    negotiated_planes_ = fmt.fmt.pix_mp.num_planes;
    cached_stride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  } else {
    params_.width = fmt.fmt.pix.width;
    params_.height = fmt.fmt.pix.height;
    negotiated_planes_ = 1;
    cached_stride_ = fmt.fmt.pix.bytesperline;
  }
  if (cached_stride_ == 0) cached_stride_ = params_.width;

  // rkisp NV12 实际协商为单平面（Y/UV 物理连续一个缓冲）；真双平面布局本板不出现
  if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && negotiated_planes_ > 1) {
    VOX_ERROR("驱动要求 %u 个平面，暂只支持单平面 NV12 布局", negotiated_planes_);
    return false;
  }

  // 期望帧率（尽力而为，失败不阻断）
  v4l2_streamparm parm{};
  parm.type = v4l2_buf_type_;
  if (xioctl(fd_, VIDIOC_G_PARM, &parm) == 0 &&
      (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = params_.fps;
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
      VOX_WARN("S_PARM %ufps: %s（沿用驱动默认）", params_.fps, strerror(errno));
    }
  }
  return true;
}

bool V4L2Capture::request_and_map_buffers() {
  v4l2_requestbuffers req{};
  req.count = params_.buffer_count;
  req.type = v4l2_buf_type_;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    VOX_ERROR("REQBUFS(%u): %s", params_.buffer_count, strerror(errno));
    return false;
  }
  if (req.count < 2) {
    VOX_ERROR("driver only gave %u buffers, need >=2", req.count);
    return false;
  }
  params_.buffer_count = req.count;
  buffers_.resize(req.count);

  for (uint32_t i = 0; i < req.count; ++i) {
    // mplane API 下 buf.m 联合体承载 planes 指针，偏移/长度都在 planes[] 里
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buf{};
    buf.type = v4l2_buf_type_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      buf.m.planes = planes;
      buf.length = 1;  // 单平面布局（negotiate_format 已拦截多平面）
    }
    if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      VOX_ERROR("QUERYBUF[%u]: %s", i, strerror(errno));
      return false;
    }
    size_t length = 0;
    off_t offset = 0;
    if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      length = planes[0].length;
      offset = planes[0].m.mem_offset;
    } else {
      length = buf.length;
      offset = buf.m.offset;
    }
    void* addr =
        mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
    if (addr == MAP_FAILED) {
      VOX_ERROR("mmap[%u] len=%zu: %s", i, length, strerror(errno));
      return false;
    }
    buffers_[i].start = addr;
    buffers_[i].length = length;

    // 导出 DMA-BUF fd：单平面布局下即整帧缓冲，供 MPP/RGA 零拷贝
    v4l2_exportbuffer exp{};
    exp.type = v4l2_buf_type_;
    exp.index = i;
    exp.plane = 0;
    exp.flags = O_CLOEXEC;
    if (xioctl(fd_, VIDIOC_EXPBUF, &exp) < 0) {
      VOX_WARN("EXPBUF[%u]: %s（dma_fd=-1，走用户态指针路径）", i, strerror(errno));
      buffers_[i].dma_fd = -1;
    } else {
      buffers_[i].dma_fd = exp.fd;
    }
  }
  return true;
}

void V4L2Capture::unmap_buffers() {
  for (auto& b : buffers_) {
    if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    if (b.dma_fd >= 0) close(b.dma_fd);
  }
  buffers_.clear();
}

bool V4L2Capture::start_streaming() {
  for (uint32_t i = 0; i < params_.buffer_count; ++i) {
    if (!enqueue(static_cast<int>(i))) return false;
  }
  int type = static_cast<int>(v4l2_buf_type_);
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    VOX_ERROR("STREAMON: %s", strerror(errno));
    return false;
  }
  return true;
}

bool V4L2Capture::enqueue(int index) {
  v4l2_plane planes[VIDEO_MAX_PLANES]{};
  v4l2_buffer buf{};
  buf.type = v4l2_buf_type_;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = static_cast<uint32_t>(index);
  if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    buf.m.planes = planes;
    buf.length = 1;
  }
  if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
    VOX_ERROR("QBUF[%d]: %s", index, strerror(errno));
    return false;
  }
  return true;
}

// ---------- 出帧 ----------

const VideoFrame* V4L2Capture::acquire(int timeout_ms) {
  if (!running_.load() || fd_ < 0) return nullptr;
  if (dequeued_index_ >= 0) {
    VOX_ERROR("acquire() 前一帧未 release()（index=%d）", dequeued_index_);
    return nullptr;
  }
  // 分片 poll：stop() 置位后最长 200ms 内退出
  int remaining = timeout_ms;
  while (remaining > 0) {
    pollfd pfd{fd_, POLLIN, 0};
    int slice = remaining > 200 ? 200 : remaining;
    int pr = poll(&pfd, 1, slice);
    if (pr < 0) {
      if (errno == EINTR) continue;
      VOX_ERROR("poll: %s", strerror(errno));
      return nullptr;
    }
    if (!running_.load()) return nullptr;
    if (pr == 0) {
      remaining -= slice;
      continue;
    }
    if (pfd.revents & POLLIN) {
      if (!dequeue_oldest()) return nullptr;
      return &current_frame_;
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) {
      VOX_ERROR("poll revents=%04x，设备异常", pfd.revents);
      return nullptr;
    }
  }
  VOX_WARN("acquire 超时（%dms 无帧）", timeout_ms);
  return nullptr;
}

void V4L2Capture::release(const VideoFrame* frame) {
  if (frame == nullptr) return;
  if (frame != &current_frame_ || dequeued_index_ < 0) {
    VOX_WARN("release() 收到未知帧指针，忽略");
    return;
  }
  enqueue(dequeued_index_);
  dequeued_index_ = -1;
}

bool V4L2Capture::dequeue_oldest() {
  v4l2_plane planes[VIDEO_MAX_PLANES]{};
  v4l2_buffer buf{};
  buf.type = v4l2_buf_type_;
  buf.memory = V4L2_MEMORY_MMAP;
  if (v4l2_buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    buf.m.planes = planes;
    buf.length = 1;
  }
  if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
    VOX_ERROR("DQBUF: %s", strerror(errno));
    return false;
  }
  dequeued_index_ = static_cast<int>(buf.index);
  MappedBuffer& mb = buffers_[buf.index];

  // 单平面 NV12：Y 在缓冲头部，UV 紧随其后（Y/UV 物理连续）
  const size_t stride = cached_stride_;
  current_frame_ = VideoFrame{};
  current_frame_.width = params_.width;
  current_frame_.height = params_.height;
  current_frame_.format = params_.format;
  current_frame_.dma_fd = mb.dma_fd;
  current_frame_.timestamp_ns =
      static_cast<int64_t>(buf.timestamp.tv_sec) * 1000000000ll +
      static_cast<int64_t>(buf.timestamp.tv_usec) * 1000ll;
  current_frame_.sequence = frame_counter_++;
  current_frame_.plane[0] = mb.start;
  current_frame_.plane_stride[0] = stride;
  current_frame_.plane_bytes[0] = stride * params_.height;
  current_frame_.plane[1] = static_cast<uint8_t*>(mb.start) + current_frame_.plane_bytes[0];
  current_frame_.plane_stride[1] = stride;
  current_frame_.plane_bytes[1] = stride * params_.height / 2;
  return true;
}

std::string V4L2Capture::describe() const {
  char s[256];
  std::snprintf(s, sizeof(s), "%s NV12 %ux%u stride=%zu, %u bufs(mmap%s), target %ufps",
                params_.device.c_str(), params_.width, params_.height, cached_stride_,
                params_.buffer_count, buffers_.empty() ? "" : "+dmabuf", params_.fps);
  return s;
}

// ---------- 设备枚举（排障用） ----------

std::string list_video_devices() {
  std::string out;
  DIR* d = opendir("/dev");
  if (!d) return "opendir(/dev) failed";
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (strncmp(e->d_name, "video", 5) != 0) continue;
    std::string path = std::string("/dev/") + e->d_name;
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
        (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
      char line[256];
      std::snprintf(line, sizeof(line), "%s: %s [%s] mplane=%c\n", path.c_str(),
                    cap.card, cap.driver,
                    (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? 'y' : 'n');
      out += line;
    }
    close(fd);
  }
  closedir(d);
  return out.empty() ? "no capture devices" : out;
}

}  // namespace vox
