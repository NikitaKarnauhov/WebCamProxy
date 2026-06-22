#pragma once
#include <linux/videodev2.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct V4L2Buffer {
    void* start = nullptr;
    size_t length = 0;
};

class V4L2Device {
public:
    V4L2Device();
    ~V4L2Device();

    bool open(const char* path, v4l2_buf_type buf_type);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const std::string& path() const { return path_; }

    const v4l2_capability& caps() const { return cap_; }
    bool hasCapture() const { return cap_.capabilities & V4L2_CAP_VIDEO_CAPTURE; }
    bool hasOutput() const { return cap_.capabilities & V4L2_CAP_VIDEO_OUTPUT; }
    bool hasStreaming() const { return cap_.capabilities & V4L2_CAP_STREAMING; }
    std::string driver() const {
        return std::string(reinterpret_cast<const char*>(cap_.driver));
    }
    std::string card() const {
        return std::string(reinterpret_cast<const char*>(cap_.card));
    }

    bool getFormat(uint32_t& pixelformat, uint32_t& width, uint32_t& height);
    bool setFormat(uint32_t& pixelformat, uint32_t& width, uint32_t& height);
    uint32_t frameSize() const { return frame_size_; }

    bool initBuffers(uint32_t num_buffers);
    bool startStreaming(uint32_t initial_bytesused = 0);
    bool stopStreaming();

    bool dequeueBuffer(size_t& index, size_t& bytesused);
    bool enqueueBuffer(size_t index, size_t bytesused);
    V4L2Buffer& buffer(size_t index) { return buffers_[index]; }
    size_t numBuffers() const { return buffers_.size(); }

private:
    int xioctl(unsigned long request, void* arg);

    int fd_ = -1;
    std::string path_;
    v4l2_buf_type buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_capability cap_ = {};
    std::vector<V4L2Buffer> buffers_;
    uint32_t frame_size_ = 0;
    bool streaming_ = false;
};
