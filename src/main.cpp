#include "v4l2.h"
#include "rotate.h"
#include "mjpeg.h"

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

static std::string opt_source;   // --source
static std::string opt_name = "WebCamProxy";  // --name
static int opt_rotate = 180;     // --rotate
static int opt_sharpness = -1;           // -1 = not set
static int opt_backlight = -1;
static int opt_focus_abs = -1;
static bool opt_focus_auto = true;       // default: auto
static std::string opt_frame_size;       // --input-frame-size WxH
static std::string opt_input_fmt;        // --input-format auto|mjpg

static void sig_handler(int) { running = 0; }

static void set_best_fps(int fd, uint32_t width, uint32_t height,
                         uint32_t pixelformat) {
    // Enumerate frame intervals, pick the fastest (smallest interval)
    struct v4l2_frmivalenum fie;
    std::memset(&fie, 0, sizeof(fie));
    fie.pixel_format = pixelformat;
    fie.width = width;
    fie.height = height;

    uint32_t best_num = 0, best_den = 1;
    for (fie.index = 0;
         ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie) >= 0;
         fie.index++) {
        if (fie.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            float fps = static_cast<float>(fie.discrete.denominator) /
                        fie.discrete.numerator;
            float best = (best_den > 0) ?
                static_cast<float>(best_den) / best_num : 0;
            if (fps > best) {
                best_num = fie.discrete.numerator;
                best_den = fie.discrete.denominator;
            }
        } else if (fie.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            // Use the minimum interval (= maximum fps)
            best_num = fie.stepwise.min.numerator;
            best_den = fie.stepwise.min.denominator;
            break;
        }
    }

    if (best_num > 0) {
        struct v4l2_streamparm parm;
        std::memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = best_num;
        parm.parm.capture.timeperframe.denominator = best_den;
        if (ioctl(fd, VIDIOC_S_PARM, &parm) == 0) {
            float fps = static_cast<float>(best_den) / best_num;
            fprintf(stderr, "Frame rate: %.2f fps\n", fps);
        }
    }
}

static void apply_controls(int fd) {
    auto set_ctrl = [fd](uint32_t id, int val, const char* name) {
        v4l2_control ctrl;
        std::memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = id;
        ctrl.value = val;
        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "Warning: could not set %s: %s\n",
                    name, strerror(errno));
    };

    if (opt_sharpness >= 0)
        set_ctrl(V4L2_CID_SHARPNESS, opt_sharpness, "sharpness");
    if (opt_backlight >= 0)
        set_ctrl(V4L2_CID_BACKLIGHT_COMPENSATION, opt_backlight,
                 "backlight_compensation");
    if (opt_focus_abs >= 0) {
        set_ctrl(V4L2_CID_FOCUS_AUTO, 0, "focus_auto");
        set_ctrl(V4L2_CID_FOCUS_ABSOLUTE, opt_focus_abs, "focus_absolute");
    } else if (!opt_focus_auto) {
        set_ctrl(V4L2_CID_FOCUS_AUTO, 0, "focus_auto");
    }
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
        " v4l2loopback-ctl add -n " + opt_name +
        " -x 1 -b 4 -w 1920 -h 1920 2>/dev/null";
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
        {"source",                required_argument, nullptr, 's'},
        {"name",                  required_argument, nullptr, 'n'},
        {"rotate",                required_argument, nullptr, 'r'},
        {"sharpness",             required_argument, nullptr, 'S'},
        {"backlight-compensation",required_argument, nullptr, 'B'},
        {"focus",                 required_argument, nullptr, 'F'},
        {"input-frame-size",      required_argument, nullptr, 'W'},
        {"input-format",          required_argument, nullptr, 'I'},
        {"help",                  no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "s:n:r:S:B:F:W:I:h",
                            long_opts, nullptr)) != -1) {
        switch (c) {
        case 's': opt_source = optarg; break;
        case 'n': opt_name   = optarg; break;
        case 'r':
            opt_rotate = atoi(optarg);
            if (opt_rotate != 0 && opt_rotate != 90 &&
                opt_rotate != 180 && opt_rotate != 270) {
                fprintf(stderr, "Invalid --rotate: %s"
                        " (must be 0, 90, 180, or 270)\n", optarg);
                return 1;
            }
            break;
        case 'S': opt_sharpness = atoi(optarg); break;
        case 'B': opt_backlight = atoi(optarg); break;
        case 'F':
            if (strcmp(optarg, "auto") == 0) {
                opt_focus_auto = true;
            } else {
                opt_focus_auto = false;
                opt_focus_abs = atoi(optarg);
            }
            break;
        case 'W': opt_frame_size = optarg; break;
        case 'I':
            opt_input_fmt = optarg;
            if (opt_input_fmt != "auto" && opt_input_fmt != "mjpg") {
                fprintf(stderr, "Invalid --input-format: %s"
                        " (must be 'auto' or 'mjpg')\n", optarg);
                return 1;
            }
            break;
        case 'h':
            fprintf(stderr,
                "Usage: %s [OPTIONS]\n"
                "  --source DEV     real webcam path or card name"
                " (default: auto)\n"
                "  --name   NAME    virtual camera name"
                " (default: WebCamProxy)\n"
                "  --rotate ANGLE   0, 90, 180 (default), or 270\n"
                "  --sharpness N    set sharpness"
                " (camera-dependent)\n"
                "  --backlight-compensation N  set backlight comp.\n"
                "  --focus auto|N   auto-focus or manual value\n"
                "  --input-frame-size WxH  capture resolution"
                " (default: 640x480)\n"
                "  --input-format auto|mjpg  capture format"
                " (default: auto=YUYV)\n",
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

    // Default camera format; negotiated on first consumer connect.
    uint32_t cam_width = 640, cam_height = 480;
    if (!opt_frame_size.empty()) {
        uint32_t rw, rh;
        if (sscanf(opt_frame_size.c_str(), "%ux%u", &rw, &rh) == 2 &&
            rw > 0 && rh > 0) {
            cam_width = rw; cam_height = rh;
            fprintf(stderr, "Requested input size: %ux%u\n", rw, rh);
        } else {
            fprintf(stderr, "Invalid --input-frame-size '%s',"
                    " using default\n", opt_frame_size.c_str());
        }
    }
    uint32_t pixfmt = (opt_input_fmt == "mjpg") ?
        V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;

    // Output dimensions depend on rotation angle
    uint32_t out_width, out_height;
    rotated_dims(cam_width, cam_height, opt_rotate, out_width, out_height);
    uint32_t out_frame_size = out_width * out_height * 2;
    fprintf(stderr, "Output: YUYV %ux%u rotate=%d (%u bytes/frame)\n",
            out_width, out_height, opt_rotate, out_frame_size);

    // --- Everything below uses these initialized variables ---
    {
        V4L2Device out;
        bool cam_is_mjpg = (pixfmt == V4L2_PIX_FMT_MJPEG);
        bool out_is_mjpg = cam_is_mjpg;
        std::vector<uint8_t> rotated_frame(out_frame_size);
        std::vector<uint8_t> blank_frame(out_frame_size, 0);
        for (size_t i = 0; i < out_frame_size; i += 4) {
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
            uint32_t opf = pixfmt, ow = out_width, oh = out_height;
            if (!out.setFormat(opf, ow, oh) ||
                opf != pixfmt || ow != out_width || oh != out_height) {
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
        // Streaming I/O — output buffers must exist for capture to work
        if (!out.initBuffers(4)) {
            fprintf(stderr, "Failed to init output buffers\n");
            goto out_done;
        }
        for (size_t i = 0; i < out.numBuffers(); i++) {
            if (out_is_mjpg)
                std::memset(out.buffer(i).start, 0,
                            out.buffer(i).length);
            else
                std::memcpy(out.buffer(i).start, blank_frame.data(),
                            out_frame_size);
        }
        if (!out.startStreaming(out_is_mjpg ? 0 : out_frame_size)) {
            fprintf(stderr, "Failed to start output streaming\n");
            goto out_done;
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
                time_t last_blank = 0;
                while (running && open_count <= 1) {
                    struct pollfd pfd;
                    pfd.fd = inotify_fd;
                    pfd.events = POLLIN;
                    int ret = poll(&pfd, 1, 2000);
                    if (ret < 0) {
                        if (errno == EINTR) continue; break;
                    }
                    time_t now = time(nullptr);
                    if (ret == 0 || now - last_blank >= 3) {
                        if (!out_is_mjpg) {
                            size_t oidx, unused;
                            if (out.dequeueBuffer(oidx, unused)) {
                                std::memcpy(out.buffer(oidx).start,
                                            blank_frame.data(),
                                            out_frame_size);
                                out.enqueueBuffer(oidx, out_frame_size);
                            }
                        }
                        last_blank = now;
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
                while (running && !has_other_opener(out_path)) {
                    if (!out_is_mjpg)
                        write(out.fd(), blank_frame.data(), out_frame_size);
                    sleep(2);
                }
            }
            if (!running) break;

            // --- Consumer detected — open camera ---
            fprintf(stderr, "[active] consumer detected, opening camera...\n");
            V4L2Device cam;
            bool cam_ok = false;

            // Find camera path if unknown
            std::string path = cam_path;
            if (path.empty()) {
                glob_t cgl;
                if (glob("/dev/video*", 0, nullptr, &cgl) == 0) {
                    for (size_t i = 0; i < cgl.gl_pathc; i++) {
                        if (cgl.gl_pathv[i] == out_path) continue;
                        V4L2Device probe;
                        if (probe.open(cgl.gl_pathv[i],
                                       V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
                            probe.hasCapture() && probe.hasStreaming() &&
                            probe.driver().find("loopback") ==
                                std::string::npos) {
                            path = cgl.gl_pathv[i];
                            cam_path = path;
                            fprintf(stderr, "Real webcam: %s (%s)\n",
                                    path.c_str(), probe.caps().card);
                            probe.close();
                            break;
                        }
                        probe.close();
                    }
                    globfree(&cgl);
                }
            }
            if (path.empty()) { fprintf(stderr, "No camera\n"); continue; }

            if (!cam.open(path.c_str(),
                          V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
                fprintf(stderr, "Failed to open camera: %s\n",
                        strerror(errno));
                continue;
            }
            usleep(300000);

            // Try to set format; retry on EBUSY keeping the fd open.
            // KTalk may be probing the real camera simultaneously;
            // once it releases, S_FMT will succeed.
            // Parse requested frame size
            uint32_t req_w = cam_width, req_h = cam_height;
            if (!opt_frame_size.empty()) {
                if (sscanf(opt_frame_size.c_str(), "%ux%u",
                           &req_w, &req_h) != 2) {
                    fprintf(stderr, "Invalid --input-frame-size: %s"
                            " (expected WxH)\n",
                            opt_frame_size.c_str());
                    req_w = cam_width; req_h = cam_height;
                } else {
                    fprintf(stderr, "Requested input size: %ux%u\n",
                            req_w, req_h);
                }
            }

            for (int retry = 0; running; retry++) {
                uint32_t pf = (opt_input_fmt == "mjpg") ?
                    V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
                uint32_t w = req_w, h = req_h;
                if (retry == 0) {
                    uint32_t gpf, gw, gh;
                    if (cam.getFormat(gpf, gw, gh) &&
                        gpf == pf && gw == req_w && gh == req_h) {
                        w = gw; h = gh;
                        goto format_ok;
                    }
                }
                if (cam.setFormat(pf, w, h)) {
format_ok:
                    uint32_t cam_fs = cam.frameSize();
                    if (cam_fs == 0) cam_fs = w * h * 2;  // fallback
                    bool is_mjpg = (pf == V4L2_PIX_FMT_MJPEG);
                    uint32_t out_fs;
                    if (is_mjpg && opt_rotate == 0) {
                        out_width = w;
                        out_height = h;
                        out_frame_size = out_fs = cam_fs;
                        fprintf(stderr, "Format: MJPG %ux%u (max %u"
                                " bytes/frame)\n", w, h, cam_fs);
                    } else if (is_mjpg) {
                        // MJPEG → decode → rotate → re-encode MJPG
                        rotated_dims(w, h, opt_rotate,
                                     out_width, out_height);
                        out_fs = out_width * out_height * 2;
                        out_frame_size = out_fs;
                        fprintf(stderr, "Format: MJPG %ux%u → MJPG"
                                " %ux%u rotate=%d\n",
                                w, h, out_width, out_height, opt_rotate);
                    } else if (pf == V4L2_PIX_FMT_YUYV) {
                        rotated_dims(w, h, opt_rotate,
                                     out_width, out_height);
                        out_fs = out_width * out_height * 2;
                        out_frame_size = out_fs;
                    } else {
                        fprintf(stderr, "Unsupported format %c%c%c%c\n",
                                static_cast<char>(pf & 0xFF),
                                static_cast<char>((pf >> 8) & 0xFF),
                                static_cast<char>((pf >> 16) & 0xFF),
                                static_cast<char>((pf >> 24) & 0xFF));
                        break;
                    }
                    if (w != cam_width || h != cam_height ||
                        out_fs != out_frame_size ||
                        pf != pixfmt) {
                        cam_width = w; cam_height = h;
                        cam_is_mjpg = (pf == V4L2_PIX_FMT_MJPEG);
                        out_is_mjpg = cam_is_mjpg;
                        pixfmt = out_is_mjpg ? pf : V4L2_PIX_FMT_YUYV;

                        // Only update output if format actually differs
                        bool fmt_changed = true;
                        {
                            uint32_t cpf, cw, ch;
                            if (out.getFormat(cpf, cw, ch) &&
                                cpf == pixfmt &&
                                cw == out_width && ch == out_height) {
                                fmt_changed = false;
                            }
                        }
                        if (fmt_changed) {
                        uint32_t opf = pixfmt, ow = out_width, oh = out_height;
                        // Temporarily allow format change
                        {
                            v4l2_control ctrl;
                            std::memset(&ctrl, 0, sizeof(ctrl));
                            ctrl.id = 0x0098f900;
                            ctrl.value = 0;
                            ioctl(out.fd(), VIDIOC_S_CTRL, &ctrl);
                        }
                        if (!out.setFormat(opf, ow, oh) || opf != pixfmt ||
                            ow != out_width || oh != out_height) {
                            fprintf(stderr,
                                    "Output rejected format"
                                    " (wanted %c%c%c%c %ux%u,"
                                    " got %c%c%c%c %ux%u)\n",
                                    static_cast<char>(pixfmt & 0xFF),
                                    static_cast<char>((pixfmt >> 8) & 0xFF),
                                    static_cast<char>((pixfmt >> 16) & 0xFF),
                                    static_cast<char>((pixfmt >> 24) & 0xFF),
                                    out_width, out_height,
                                    static_cast<char>(opf & 0xFF),
                                    static_cast<char>((opf >> 8) & 0xFF),
                                    static_cast<char>((opf >> 16) & 0xFF),
                                    static_cast<char>((opf >> 24) & 0xFF),
                                    ow, oh);
                            break;
                        }
                        // Re-lock format
                        {
                            v4l2_control ctrl;
                            std::memset(&ctrl, 0, sizeof(ctrl));
                            ctrl.id = 0x0098f900;
                            ctrl.value = 1;
                            ioctl(out.fd(), VIDIOC_S_CTRL, &ctrl);
                        }
                        out.stopStreaming();
                        if (!out.initBuffers(4)) {
                            fprintf(stderr,
                                    "Failed to reinit output buffers\n");
                            break;
                        }
        if (!out.startStreaming(pixfmt == V4L2_PIX_FMT_MJPEG ?
                                0 : out_frame_size)) {
                            fprintf(stderr,
                                    "Failed to restart output\n");
                            break;
                        }
                        rotated_frame.resize(out_frame_size);
                        blank_frame.resize(out_frame_size);
                        for (size_t i = 0; i < out_frame_size; i += 4) {
                            blank_frame[i]   = 16;
                            blank_frame[i+1] = 128;
                            blank_frame[i+2] = 16;
                            blank_frame[i+3] = 128;
                        }
                        }
                    }
                    set_best_fps(cam.fd(), w, h, pf);
                    if (!cam.initBuffers(4)) {
                        fprintf(stderr,
                                "Failed to init capture buffers\n");
                        break;
                    }
                    if (!cam.startStreaming()) {
                        fprintf(stderr,
                                "Failed to start capture\n");
                        break;
                    }
                    cam_ok = true;
                    apply_controls(cam.fd());
                    break;
                }

                // Format failed — close the fd so KTalk can finish
                // probing, then reopen and retry.
                if (retry == 0)
                    fprintf(stderr,
                            "[active] camera busy (errno=%s),"
                            " waiting...\n", strerror(errno));
                cam.close();
                sleep(3);
                if (!cam.open(path.c_str(),
                              V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
                    fprintf(stderr, "Failed to reopen camera: %s\n",
                            strerror(errno));
                    break;
                }
                usleep(300000);
                if (retry > 0 && retry % 5 == 0)
                    fprintf(stderr,
                            "[active] still waiting after %d retries...\n",
                            retry);
            }
            if (!cam_ok) {
                cam.close();
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
                        if (cam_is_mjpg) {
                            if (opt_rotate == 0) {
                                std::memcpy(rotated_frame.data(),
                                            cam.buffer(idx).start,
                                            bytesused);
                            } else {
                                uint8_t* rjpeg = nullptr;
                                size_t rjpeg_len = 0;
                                if (!mjpeg_rotate(
                                        static_cast<const uint8_t*>(
                                            cam.buffer(idx).start),
                                        bytesused,
                                        &rjpeg, &rjpeg_len,
                                        opt_rotate) || !rjpeg) {
                                    cam.enqueueBuffer(idx, bytesused);
                                    continue;
                                }
                                size_t n = rjpeg_len;
                                if (n > out_frame_size) n = out_frame_size;
                                std::memcpy(rotated_frame.data(), rjpeg, n);
                                bytesused = n;
                                free(rjpeg);
                            }
                        } else {
                            rotate_yuyv(
                                static_cast<const uint8_t*>(
                                    cam.buffer(idx).start),
                                rotated_frame.data(),
                                cam_width, cam_height, opt_rotate);
                        }
                        cap_count++;
                        cam.enqueueBuffer(idx, bytesused);

                        size_t oidx, unused;
                        if (out.dequeueBuffer(oidx, unused)) {
                            size_t n = out_is_mjpg ?
                                static_cast<size_t>(bytesused) :
                                out_frame_size;
                            std::memcpy(out.buffer(oidx).start,
                                        rotated_frame.data(), n);
                            out.enqueueBuffer(oidx, n);
                            out_count++;
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
            usleep(300000);  // let hardware finish stopping
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
