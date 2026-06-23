#include "v4l2.h"
#include "rotate.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t running = 1;

static std::string opt_source;   // --source /dev/videoN or card name
static std::string opt_name = "WebCamProxy";  // --name

static void sig_handler(int) { running = 0; }

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

static const char* sudo_cmd() {
    return isatty(STDIN_FILENO) ? "sudo" : "sudo -n";
}

static bool load_module() {
    fprintf(stderr, "v4l2loopback module not loaded, loading...\n");
    std::string cmd = std::string(sudo_cmd()) +
        " modprobe v4l2loopback 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0 && !module_loaded()) {
        fprintf(stderr,
            "Failed to load v4l2loopback module (exit %d).\n"
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
        " v4l2loopback-ctl add -n " + opt_name + " -x 1 -b 4 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "v4l2loopback-ctl add failed (exit %d).\n",
                WEXITSTATUS(ret));
        return "";
    }
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) usleep(100000);
        std::string dev = find_device_by_card(opt_name.c_str());
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

static bool has_other_opener(const std::string& path) {
    pid_t my_pid = getpid();
    glob_t gl;
    if (glob("/proc/*/fd/*", GLOB_NOSORT, nullptr, &gl) != 0) return false;
    bool found = false;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        char buf[256];
        ssize_t len = readlink(gl.gl_pathv[i], buf, sizeof(buf) - 1);
        if (len < 0) continue;
        buf[len] = '\0';
        if (path == buf) {
            pid_t pid = atoi(gl.gl_pathv[i] + 6);
            if (pid > 0 && pid != my_pid) { found = true; break; }
        }
    }
    globfree(&gl);
    return found;
}

int main(int argc, char** argv) {
    static struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"name",   required_argument, nullptr, 'n'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "s:n:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's': opt_source = optarg; break;
        case 'n': opt_name   = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: %s [--source DEVICE] [--name NAME]\n"
                "  --source DEV    real webcam path or card name"
                " (default: auto-detect)\n"
                "  --name   NAME   virtual camera name"
                " (default: WebCamProxy)\n",
                argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    if (!module_loaded() && !load_module()) return 1;

    // --- Find real webcam ---
    glob_t gl;
    if (glob("/dev/video*", 0, nullptr, &gl) != 0) {
        fprintf(stderr, "No /dev/video* devices found.\n");
        return 1;
    }
    std::string cam_path;

    if (!opt_source.empty()) {
        // User specified a source — try as device path first, then card name
        if (opt_source.find("/dev/") == 0) {
            V4L2Device dev;
            if (dev.open(opt_source.c_str(), V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
                dev.hasCapture() && dev.hasStreaming() &&
                dev.driver().find("loopback") == std::string::npos) {
                cam_path = opt_source;
                fprintf(stderr, "Real webcam: %s (%s)\n",
                        cam_path.c_str(), dev.caps().card);
            }
            dev.close();
        }
        if (cam_path.empty()) {
            // Search by card name
            for (size_t i = 0; i < gl.gl_pathc; i++) {
                V4L2Device dev;
                if (!dev.open(gl.gl_pathv[i],
                              V4L2_BUF_TYPE_VIDEO_CAPTURE)) continue;
                if (!dev.hasCapture() || !dev.hasStreaming()) {
                    dev.close(); continue;
                }
                if (dev.driver().find("loopback") != std::string::npos) {
                    dev.close(); continue;
                }
                if (dev.card().find(opt_source) != std::string::npos) {
                    cam_path = gl.gl_pathv[i];
                    fprintf(stderr, "Real webcam: %s (%s)\n",
                            cam_path.c_str(), dev.caps().card);
                    dev.close();
                    break;
                }
                dev.close();
            }
        }
        if (cam_path.empty()) {
            fprintf(stderr, "No webcam matching --source '%s' found.\n",
                    opt_source.c_str());
        }
    }

    // Auto-detect if no source specified or specified source not found
    if (cam_path.empty() && opt_source.empty()) {
        for (size_t i = 0; i < gl.gl_pathc; i++) {
            V4L2Device dev;
            if (!dev.open(gl.gl_pathv[i], V4L2_BUF_TYPE_VIDEO_CAPTURE))
                continue;
            if (!dev.hasCapture() || !dev.hasStreaming()) {
                dev.close(); continue;
            }
            if (dev.driver().find("loopback") != std::string::npos) {
                dev.close(); continue;
            }
            cam_path = gl.gl_pathv[i];
            fprintf(stderr, "Real webcam: %s (%s)\n",
                    cam_path.c_str(), dev.caps().card);
            dev.close();
            break;
        }
    }
    globfree(&gl);
    if (cam_path.empty()) {
        fprintf(stderr, "No real webcam found at startup,"
                " will retry on demand.\n");
    }

    // --- Find or create loopback device ---
    bool created = false;
    std::string out_path;
    std::string stale = find_device_by_card(opt_name.c_str());
    if (!stale.empty()) {
        fprintf(stderr, "Removing stale %s device: %s\n",
                opt_name.c_str(), stale.c_str());
        delete_loopback_device(stale);
    }
    out_path = create_loopback_device();
    if (!out_path.empty()) {
        created = true;
        fprintf(stderr, "Created loopback device: %s\n", out_path.c_str());
    } else {
        fprintf(stderr, "Device creation failed; falling back...\n");
        fprintf(stderr, "WARNING: Some apps may not detect the fallback"
                " device.\n");
        out_path = find_fallback_loopback(cam_path);
    }
    if (out_path.empty()) {
        fprintf(stderr, "No loopback output device available.\n");
        return 1;
    }

    print_sudo_help();

    // --- Negotiate camera format (or use defaults if not present) ---
    uint32_t width = 640, height = 480;
    uint32_t pixfmt = V4L2_PIX_FMT_YUYV;
    uint32_t frame_size = width * height * 2;  // YUYV default
    {
        V4L2Device cam_tmp;
        if (cam_tmp.open(cam_path.c_str(), V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
            if (cam_tmp.setFormat(pixfmt, width, height) &&
                pixfmt == V4L2_PIX_FMT_YUYV) {
                frame_size = cam_tmp.frameSize();
                fprintf(stderr, "Format: YUYV %ux%u (%u bytes/frame)\n",
                        width, height, frame_size);
            } else {
                fprintf(stderr, "Camera doesn't support YUYV, using"
                        " defaults\n");
                width = 640; height = 480;
                pixfmt = V4L2_PIX_FMT_YUYV;
                frame_size = width * height * 2;
            }
        } else {
            fprintf(stderr, "Camera not available, using default format"
                    " (will retry on demand)\n");
        }
    }

    // --- Everything below uses these initialized variables ---
    {
        V4L2Device out;
        std::vector<uint8_t> rotated_frame(frame_size);
        std::vector<uint8_t> blank_frame(frame_size, 0);
        // Prepare black frame
        for (size_t i = 0; i < frame_size; i += 4) {
            blank_frame[i]     = 16;
            blank_frame[i + 1] = 128;
            blank_frame[i + 2] = 16;
            blank_frame[i + 3] = 128;
        }
        time_t last_status = time(nullptr);
        bool setup_ok = false;
        int inotify_fd = -1;
        int inotify_wd = -1;
        int open_count = 1; // our own open
        bool use_inotify = false;

        if (!out.open(out_path.c_str(), V4L2_BUF_TYPE_VIDEO_OUTPUT)) {
            fprintf(stderr, "Failed to open output device %s: %s\n",
                    out_path.c_str(), strerror(errno));
            goto out_done;
        }
        {
            uint32_t opf = pixfmt, ow = width, oh = height;
            if (!out.setFormat(opf, ow, oh) ||
                opf != V4L2_PIX_FMT_YUYV || ow != width || oh != height) {
                fprintf(stderr, "Output device rejected format\n");
                goto out_done;
            }
        }
        {
            v4l2_control ctrl;
            std::memset(&ctrl, 0, sizeof(ctrl));
            ctrl.id = 0x0098f900;
            ctrl.value = 1;
            ioctl(out.fd(), VIDIOC_S_CTRL, &ctrl);
        }
        {
            int fl = fcntl(out.fd(), F_GETFL, 0);
            fcntl(out.fd(), F_SETFL, fl | O_NONBLOCK);
        }

        // Set up inotify to detect consumer opens/closes
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd < 0) {
            fprintf(stderr, "inotify_init1 failed: %s\n", strerror(errno));
        } else {
            inotify_wd = inotify_add_watch(inotify_fd, out_path.c_str(),
                                            IN_OPEN | IN_CLOSE);
            if (inotify_wd < 0) {
                fprintf(stderr, "inotify_add_watch failed: %s\n",
                        strerror(errno));
                close(inotify_fd);
                inotify_fd = -1;
            }
        }
        use_inotify = (inotify_fd >= 0);

        signal(SIGINT, sig_handler);
        signal(SIGTERM, sig_handler);
        setvbuf(stderr, nullptr, _IONBF, 0);
        setup_ok = true;

        fprintf(stderr, "Waiting for consumer to connect"
                " to %s...\n", out_path.c_str());

        while (running) {
            // --- Idle: wait for consumer ---
            fprintf(stderr, "[idle] camera closed, waiting...\n");
            if (use_inotify) {
                while (running && open_count <= 1) {
                    struct pollfd pfd;
                    pfd.fd = inotify_fd;
                    pfd.events = POLLIN;
                    int ret = poll(&pfd, 1, 1000);
                    if (ret < 0) { if (errno == EINTR) continue; break; }
                    if (ret == 0) {
                        write(out.fd(), blank_frame.data(), frame_size);
                        continue;
                    }
                    if (pfd.revents & POLLIN) {
                        char buf[4096];
                        ssize_t len = read(inotify_fd, buf, sizeof(buf));
                        for (char *p = buf; p < buf + len; ) {
                            auto* ev = (struct inotify_event*)p;
                             if (ev->mask & IN_OPEN)  open_count++;
                             if (ev->mask & IN_CLOSE)  open_count--;
                             p += sizeof(*ev) + ev->len;
                         }
                         if (open_count < 1) open_count = 1;
                     }
                 }
             } else {
                // Fallback: periodic polling via /proc
                while (running && !has_other_opener(out_path)) {
                    write(out.fd(), blank_frame.data(), frame_size);
                    usleep(1000000);
                }
            }
            if (!running) break;

            // --- Consumer detected — open camera ---
            fprintf(stderr, "[active] consumer detected, opening camera...\n");
            V4L2Device cam;
            bool cam_ok = false;
            do {
                // If camera path is unknown, scan for it
                std::string path = cam_path;
                if (path.empty()) {
                    // Re-scan for a webcam
                    glob_t cgl;
                    if (glob("/dev/video*", 0, nullptr, &cgl) == 0) {
                        for (size_t i = 0; i < cgl.gl_pathc; i++) {
                            if (cgl.gl_pathv[i] == out_path) continue;
                            V4L2Device probe;
                            if (probe.open(cgl.gl_pathv[i],
                                           V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
                                probe.hasCapture() &&
                                probe.hasStreaming() &&
                                probe.driver().find("loopback") ==
                                    std::string::npos) {
                                path = cgl.gl_pathv[i];
                                cam_path = path;
                                fprintf(stderr, "Real webcam found: %s (%s)\n",
                                        path.c_str(), probe.caps().card);
                                probe.close();
                                break;
                            }
                            probe.close();
                        }
                        globfree(&cgl);
                    }
                }
                if (path.empty()) {
                    fprintf(stderr, "No camera found\n");
                    break;
                }

                if (!cam.open(path.c_str(),
                              V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
                    fprintf(stderr, "Failed to open camera %s: %s\n",
                            path.c_str(), strerror(errno));
                    break;
                }
                uint32_t pf = V4L2_PIX_FMT_YUYV;
                uint32_t w = width, h = height;
                if (!cam.setFormat(pf, w, h) ||
                    pf != V4L2_PIX_FMT_YUYV) {
                    fprintf(stderr, "Camera doesn't support YUYV\n");
                    break;
                }
                // Update format if camera chose different dimensions
                uint32_t new_fs = cam.frameSize();
                if (w != width || h != height || new_fs != frame_size) {
                    fprintf(stderr, "Camera resolution changed to %ux%u\n",
                            w, h);
                    // Try to update output device format
                    uint32_t opf = pf, ow = w, oh = h;
                    if (out.setFormat(opf, ow, oh) &&
                        opf == V4L2_PIX_FMT_YUYV && ow == w && oh == h) {
                        width = w; height = h;
                        frame_size = new_fs;
                        rotated_frame.resize(frame_size);
                        blank_frame.resize(frame_size);
                        for (size_t i = 0; i < frame_size; i += 4) {
                            blank_frame[i]   = 16;
                            blank_frame[i+1] = 128;
                            blank_frame[i+2] = 16;
                            blank_frame[i+3] = 128;
                        }
                    }
                }
                if (!cam.initBuffers(4)) {
                    fprintf(stderr, "Failed to init capture buffers\n");
                    break;
                }
                if (!cam.startStreaming()) {
                    fprintf(stderr, "Failed to start capture\n");
                    break;
                }
                cam_ok = true;
            } while (false);
            if (!cam_ok) {
                cam.close();
                fprintf(stderr, "[active] camera unavailable, waiting...\n");
                time_t retry_at = time(nullptr) + 3;
                while (running && open_count > 1) {
                    if (use_inotify) {
                        struct pollfd pfd;
                        pfd.fd = inotify_fd;
                        pfd.events = POLLIN;
                        int ret = poll(&pfd, 1, 1000);
                        if (ret < 0) {
                            if (errno == EINTR) continue; break;
                        }
                        if (ret == 0) {
                            write(out.fd(), blank_frame.data(), frame_size);
                        }
                        if (pfd.revents & POLLIN) {
                            char buf[4096];
                            ssize_t len = read(inotify_fd, buf, sizeof(buf));
                            for (char *p = buf; p < buf + len; ) {
                                auto* ev = (struct inotify_event*)p;
                                if (ev->mask & IN_OPEN)  open_count++;
                                if (ev->mask & IN_CLOSE)  open_count--;
                                p += sizeof(*ev) + ev->len;
                            }
                            if (open_count < 1) open_count = 1;
                        }
                    } else {
                        write(out.fd(), blank_frame.data(), frame_size);
                        sleep(1);
                        if (!has_other_opener(out_path)) break;
                    }
                    if (time(nullptr) >= retry_at) break;
                }
                continue;
            }

            fprintf(stderr, "Camera streaming to %s."
                    " Press Ctrl+C to stop.\n", out_path.c_str());
            last_status = time(nullptr);
            uint64_t cap_count = 0, out_count = 0, out_skip = 0;

            while (running) {
                struct pollfd fds[2];
                int nfds = 0;
                fds[nfds].fd = cam.fd();
                fds[nfds].events = POLLIN;
                nfds++;
                if (use_inotify) {
                    fds[nfds].fd = inotify_fd;
                    fds[nfds].events = POLLIN;
                    nfds++;
                }

                int ret = poll(fds, nfds, 200);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    perror("poll");
                    break;
                }

                {
                    time_t now = time(nullptr);
                    if (now != last_status) {
                        fprintf(stderr,
                                "[active] cap=%lu out=%lu skip=%lu"
                                " openers=%d\n",
                                cap_count, out_count, out_skip,
                                open_count);
                        last_status = now;
                    }
                }

                // Process inotify events
                if (use_inotify && (fds[1].revents & POLLIN)) {
                    char buf[4096];
                    ssize_t len = read(inotify_fd, buf, sizeof(buf));
                    for (char *p = buf; p < buf + len; ) {
                        auto* ev = (struct inotify_event*)p;
                        if (ev->mask & IN_OPEN)  open_count++;
                        if (ev->mask & IN_CLOSE)  open_count--;
                        p += sizeof(*ev) + ev->len;
                    }
                    if (open_count < 1) open_count = 1;
                    if (open_count <= 1) {
                        fprintf(stderr,
                                "[active] consumer gone, closing camera\n");
                        break;
                    }
                }
                if (!use_inotify) {
                    // Fallback: check every ~5s
                    static int check_ctr = 0;
                    if (++check_ctr >= 25) {
                        check_ctr = 0;
                        if (!has_other_opener(out_path)) {
                            fprintf(stderr,
                                    "[active] consumer gone, closing camera\n");
                            break;
                        }
                    }
                }

                if (ret == 0) continue;

                // Camera has a new frame
                if (fds[0].revents & POLLIN) {
                    size_t idx, bytesused;
                    if (cam.dequeueBuffer(idx, bytesused)) {
                        if (idx >= cam.numBuffers()) {
                            fprintf(stderr,
                                    "Bad capture index %zu\n", idx);
                            break;
                        }
                        rotate_yuyv_180(
                            static_cast<const uint8_t*>(
                                cam.buffer(idx).start),
                            rotated_frame.data(), width, height);
                        cap_count++;
                        cam.enqueueBuffer(idx, bytesused);

                        ssize_t written = write(out.fd(),
                                                rotated_frame.data(),
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
                    fprintf(stderr, "Capture device error\n");
                    break;
                }
            }

            fprintf(stderr, "[active] closing camera...\n");
            cam.stopStreaming();
            cam.close();
        }

        fprintf(stderr, "Shutting down...\n");
    out_done:
        if (inotify_wd >= 0) inotify_rm_watch(inotify_fd, inotify_wd);
        if (inotify_fd >= 0) close(inotify_fd);
        out.close();
        (void)setup_ok;
    }

cleanup_device:
    if (created) {
        fprintf(stderr, "Removing virtual camera device %s...\n",
                out_path.c_str());
        delete_loopback_device(out_path);
    }
    return 0;
}
