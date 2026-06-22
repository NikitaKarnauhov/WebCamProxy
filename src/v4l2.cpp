#include "v4l2.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

V4L2Device::V4L2Device() = default;

V4L2Device::~V4L2Device() {
    close();
}

int V4L2Device::xioctl(unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd_, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

bool V4L2Device::open(const char* path, v4l2_buf_type buf_type) {
    path_ = path;
    buf_type_ = buf_type;

    fd_ = ::open(path, O_RDWR);
    if (fd_ < 0) return false;

    std::memset(&cap_, 0, sizeof(cap_));
    if (xioctl(VIDIOC_QUERYCAP, &cap_) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void V4L2Device::close() {
    if (fd_ >= 0) {
        stopStreaming();
        ::close(fd_);
        fd_ = -1;
    }
    for (auto& buf : buffers_) {
        if (buf.start) {
            munmap(buf.start, buf.length);
            buf.start = nullptr;
        }
    }
    buffers_.clear();
    streaming_ = false;
}

bool V4L2Device::getFormat(uint32_t& pixelformat, uint32_t& width, uint32_t& height) {
    v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = buf_type_;
    if (xioctl(VIDIOC_G_FMT, &fmt) < 0) return false;
    pixelformat = fmt.fmt.pix.pixelformat;
    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;
    return true;
}

bool V4L2Device::setFormat(uint32_t& pixelformat, uint32_t& width, uint32_t& height) {
    v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = buf_type_;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(VIDIOC_S_FMT, &fmt) < 0) return false;
    pixelformat = fmt.fmt.pix.pixelformat;
    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;
    frame_size_ = fmt.fmt.pix.sizeimage;
    return true;
}

bool V4L2Device::initBuffers(uint32_t num_buffers) {
    v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.type = buf_type_;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = num_buffers;
    if (xioctl(VIDIOC_REQBUFS, &req) < 0) return false;
    if (req.count < 2) return false;

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) return false;

        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(nullptr, buf.length,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) return false;
    }
    return true;
}

bool V4L2Device::startStreaming(uint32_t initial_bytesused) {
    for (size_t i = 0; i < buffers_.size(); i++) {
        v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = buffers_[i].length;
        buf.bytesused = initial_bytesused;
        if (xioctl(VIDIOC_QBUF, &buf) < 0) return false;
    }
    int type = buf_type_;
    if (xioctl(VIDIOC_STREAMON, &type) < 0) return false;
    streaming_ = true;
    return true;
}

bool V4L2Device::stopStreaming() {
    if (!streaming_) return true;
    int type = buf_type_;
    if (xioctl(VIDIOC_STREAMOFF, &type) < 0) return false;
    streaming_ = false;
    return true;
}

bool V4L2Device::dequeueBuffer(size_t& index, size_t& bytesused) {
    v4l2_buffer buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.type = buf_type_;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_DQBUF, &buf) < 0) return false;
    index = buf.index;
    bytesused = buf.bytesused;
    return true;
}

bool V4L2Device::enqueueBuffer(size_t index, size_t bytesused) {
    v4l2_buffer buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.type = buf_type_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.length = buffers_[index].length;
    buf.bytesused = bytesused;
    return xioctl(VIDIOC_QBUF, &buf) >= 0;
}
