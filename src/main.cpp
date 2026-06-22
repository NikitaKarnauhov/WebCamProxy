#include "v4l2.h"
#include "rotate.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t running = 1;

static void sig_handler(int) { running = 0; }

// Return "sudo" if we have a terminal (password prompt works),
// "sudo -n" otherwise (fails fast with no tty).
static const char* sudo_cmd() {
    return isatty(STDIN_FILENO) ? "sudo" : "sudo -n";
}

static bool module_loaded() {
    struct stat st;
    return stat("/sys/module/v4l2loopback", &st) == 0;
}

static void print_sudo_help() {
    fprintf(stderr,
        "\n---\n"
        "To avoid entering your sudo password each time, run:\n"
        "  sudo visudo -f /etc/sudoers.d/webcamproxy\n"
        "And add this line:\n"
        "  %%wheel ALL=(ALL) NOPASSWD: /usr/sbin/modprobe v4l2loopback*,"
        " /usr/bin/v4l2loopback-ctl *\n"
        "---\n\n");
}

static bool load_module() {
    fprintf(stderr, "v4l2loopback module not loaded, loading...\n");
    std::string cmd = std::string(sudo_cmd()) +
        " modprobe v4l2loopback 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0 && !module_loaded()) {
        fprintf(stderr,
            "Failed to load v4l2loopback module"
            " (sudo modprobe returned %d).\n"
            "Is the akmod-v4l2loopback package installed?\n", ret);
        print_sudo_help();
        return false;
    }
    if (!module_loaded()) {
        fprintf(stderr, "Module still not loaded after modprobe.\n");
        return false;
    }
    fprintf(stderr, "v4l2loopback module loaded.\n");
    return true;
}

static std::string find_device_by_card(const char* card_name) {
    glob_t gl;
    if (glob("/dev/video*", 0, nullptr, &gl) != 0) return "";

    std::string result;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        V4L2Device dev;
        // Try opening as OUTPUT first (for loopback), then CAPTURE
        bool opened = dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_OUTPUT);
        if (!opened)
            opened = dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_CAPTURE);
        if (!opened) continue;
        if (dev.card() == card_name) {
            result = gl.gl_pathv[i];
            dev.close();
            break;
        }
        dev.close();
    }
    globfree(&gl);
    return result;
}

static std::string create_loopback_device() {
    fprintf(stderr, "Creating new v4l2loopback device"
            " (sudo v4l2loopback-ctl add)...\n");
    std::string cmd = std::string(sudo_cmd()) +
        " v4l2loopback-ctl add -n WebCamProxy -x 1 -b 4 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "v4l2loopback-ctl add failed (exit code %d).\n",
                WEXITSTATUS(ret));
        return "";
    }

    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) usleep(100000);
        std::string dev = find_device_by_card("WebCamProxy");
        if (!dev.empty()) return dev;
    }

    return "";
}

static void delete_loopback_device(const std::string& path) {
    if (path.empty()) return;
    std::string cmd = std::string(sudo_cmd()) +
        " v4l2loopback-ctl delete " + path + " 2>/dev/null";
    system(cmd.c_str());
}

static std::string find_fallback_loopback(const std::string& cam_path) {
    std::string result;
    glob_t gl;
    if (glob("/dev/video*", 0, nullptr, &gl) != 0) return "";

    for (size_t i = 0; i < gl.gl_pathc; i++) {
        if (gl.gl_pathv[i] == cam_path) continue;
        V4L2Device dev;
        bool opened = dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_OUTPUT);
        if (!opened)
            opened = dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_CAPTURE);
        if (!opened) continue;
        if (dev.driver().find("loopback") != std::string::npos) {
            result = gl.gl_pathv[i];
            dev.close();
            break;
        }
        dev.close();
    }
    globfree(&gl);
    return result;
}

int main() {
    if (!module_loaded() && !load_module()) return 1;

    // --- Find real webcam ---
    glob_t gl;
    if (glob("/dev/video*", 0, nullptr, &gl) != 0) {
        fprintf(stderr, "No /dev/video* devices found.\n");
        return 1;
    }

    std::string cam_path;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        V4L2Device dev;
        if (!dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_CAPTURE)) continue;
        if (!dev.hasCapture() || !dev.hasStreaming()) {
            dev.close();
            continue;
        }
        if (dev.driver().find("loopback") != std::string::npos) {
            dev.close();
            continue;
        }
        cam_path = gl.gl_pathv[i];
        fprintf(stderr, "Real webcam: %s (%s)\n",
                cam_path.c_str(), dev.caps().card);
        dev.close();
        break;
    }
    globfree(&gl);

    if (cam_path.empty()) {
        fprintf(stderr, "No real webcam found"
                " (V4L2_CAP_VIDEO_CAPTURE + V4L2_CAP_STREAMING).\n");
        return 1;
    }

    // --- Find or create loopback device ---
    bool created = false;
    std::string out_path;

    // Remove any stale VirtualCam device from a previous run
    std::string stale = find_device_by_card("WebCamProxy");
    if (!stale.empty()) {
        fprintf(stderr, "Removing stale WebCamProxy device: %s\n",
                stale.c_str());
        delete_loopback_device(stale);
    }

    out_path = create_loopback_device();
    if (!out_path.empty()) {
        created = true;
        fprintf(stderr, "Created loopback device: %s\n", out_path.c_str());
    } else {
        fprintf(stderr, "Device creation failed; searching for existing"
                " loopback devices...\n");
        fprintf(stderr, "WARNING: Some apps (Cheese, Firefox) may not"
                " detect a device with exclusive_caps=1.\n");
        out_path = find_fallback_loopback(cam_path);
    }

    if (out_path.empty()) {
        fprintf(stderr, "No loopback output device available.\n");
        return 1;
    }

    print_sudo_help();

    // --- Streaming block ---
    V4L2Device cam, out;
    bool stream_ok = false;

    do {
        if (!cam.open(cam_path.c_str(), V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
            fprintf(stderr, "Failed to open camera %s: %s\n",
                    cam_path.c_str(), strerror(errno));
            break;
        }

        uint32_t pixfmt = V4L2_PIX_FMT_YUYV;
        uint32_t width = 640, height = 480;
        if (!cam.setFormat(pixfmt, width, height)) {
            fprintf(stderr, "Failed to set format on camera\n");
            break;
        }
        if (pixfmt != V4L2_PIX_FMT_YUYV) {
            fprintf(stderr, "Camera does not support YUYV (got: %c%c%c%c)\n",
                    static_cast<char>(pixfmt & 0xFF),
                    static_cast<char>((pixfmt >> 8) & 0xFF),
                    static_cast<char>((pixfmt >> 16) & 0xFF),
                    static_cast<char>((pixfmt >> 24) & 0xFF));
            break;
        }

        uint32_t frame_size = cam.frameSize();
        fprintf(stderr, "Format: YUYV %ux%u (%u bytes/frame)\n",
                width, height, frame_size);

        if (!out.open(out_path.c_str(), V4L2_BUF_TYPE_VIDEO_OUTPUT)) {
            fprintf(stderr, "Failed to open output device %s: %s\n",
                    out_path.c_str(), strerror(errno));
            break;
        }

        uint32_t out_pixfmt = pixfmt;
        uint32_t out_w = width, out_h = height;
        if (!out.setFormat(out_pixfmt, out_w, out_h)) {
            fprintf(stderr, "Failed to set format on output device\n");
            break;
        }
        if (out_pixfmt != V4L2_PIX_FMT_YUYV || out_w != width
            || out_h != height) {
            fprintf(stderr, "Output device rejected YUYV %ux%u format\n",
                    width, height);
            break;
        }

        {
            v4l2_control ctrl;
            std::memset(&ctrl, 0, sizeof(ctrl));
            ctrl.id = 0x0098f900;
            ctrl.value = 1;
            if (ioctl(out.fd(), VIDIOC_S_CTRL, &ctrl) < 0) {
                fprintf(stderr, "Warning: could not set keep_format: %s\n",
                        strerror(errno));
            }
        }

        int out_flags = fcntl(out.fd(), F_GETFL, 0);
        fcntl(out.fd(), F_SETFL, out_flags | O_NONBLOCK);

        const uint32_t NUM_BUFFERS = 4;
        if (!cam.initBuffers(NUM_BUFFERS)) {
            fprintf(stderr, "Failed to init capture buffers\n");
            break;
        }

        if (!cam.startStreaming()) {
            fprintf(stderr, "Failed to start capture streaming\n");
            break;
        }

        fprintf(stderr, "Streaming. Press Ctrl+C to stop.\n");

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sig_handler;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        std::vector<uint8_t> rotated_frame(frame_size);
        bool have_frame = false;
        time_t last_status = time(nullptr);
        uint64_t cap_count = 0, out_count = 0, out_skip = 0;
        setvbuf(stderr, nullptr, _IONBF, 0);

        stream_ok = true;

        while (running) {
            struct pollfd fds[1];
            fds[0].fd = cam.fd();
            fds[0].events = POLLIN;

            int ret = poll(fds, 1, 1000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                perror("poll");
                break;
            }

            {
                time_t now = time(nullptr);
                if (now != last_status) {
                    fprintf(stderr, "[status] cap=%lu out=%lu skip=%lu "
                            "have_frame=%d\n",
                            cap_count, out_count, out_skip, have_frame);
                    last_status = now;
                }
            }
            if (ret == 0) continue;

            if (fds[0].revents & POLLIN) {
                size_t idx, bytesused;
                if (cam.dequeueBuffer(idx, bytesused)) {
                    if (idx >= cam.numBuffers()) {
                        fprintf(stderr,
                                "Capture dequeue: bad index %zu\n", idx);
                        break;
                    }
                    rotate_yuyv_180(
                        static_cast<const uint8_t*>(
                            cam.buffer(idx).start),
                        rotated_frame.data(), width, height);
                    have_frame = true;
                    cap_count++;
                    cam.enqueueBuffer(idx, bytesused);

                    ssize_t written = write(out.fd(), rotated_frame.data(),
                                            frame_size);
                    if (written == static_cast<ssize_t>(frame_size)) {
                        out_count++;
                    } else if (written < 0 && errno != EAGAIN) {
                        fprintf(stderr, "Output write error: %s\n",
                                strerror(errno));
                        break;
                    } else {
                        out_skip++;
                    }
                }
            }

            if (fds[0].revents & (POLLERR | POLLHUP)) {
                fprintf(stderr, "Capture device error/hangup\n");
                break;
            }
        }

        fprintf(stderr, "Shutting down...\n");
    } while (false);

    cam.stopStreaming();
    cam.close();
    out.close();

    if (created) {
        fprintf(stderr, "Removing virtual camera device %s...\n",
                out_path.c_str());
        delete_loopback_device(out_path);
    }
    return 0;
}
