# WebCamProxy

Virtual V4L2 camera that proxies a real webcam with rotation, cropping, and
format conversion.  The virtual camera appears to applications (browsers,
KTalk, Cheese, ffplay) as a regular `/dev/video` device.

**This code was generated with DeepSeek.**

## Requirements

- Linux with V4L2
- `v4l2loopback` kernel module (`akmod-v4l2loopback` on Fedora)
- `libjpeg-turbo` and `turbojpeg` (`turbojpeg-devel` on Fedora)
- `yaml-cpp` (`yaml-cpp-devel` on Fedora)
- CMake 3.16+, Ninja, C++17 compiler

## Quick Start

```bash
# One-time: load the kernel module
sudo modprobe v4l2loopback

# Build
cmake -B build -G Ninja
ninja -C build

# Run (auto-detects real webcam and loopback device)
./build/webcamproxy

# Test with ffplay
ffplay -f v4l2 -pixel_format yuyv422 -video_size 640x480 /dev/video3
```

## Usage

```
webcamproxy [OPTIONS]

  --source DEV           real webcam path or card name (default: auto-detect)
  --name   NAME          virtual camera name (default: WebCamProxy)
  --rotate ANGLE         0, 90, 180 (default), or 270
  --input-frame-size WxH capture resolution (default: 640x480)
  --input-format auto|mjpg  capture format (default: auto=YUYV)
  --output-aspect-ratio W:H  crop output to aspect ratio (e.g. 16:9, 4:3)

  --sharpness N           camera sharpness (0-255)
  --brightness auto|N     brightness multiplier; auto=default
  --contrast auto|N       contrast multiplier; auto=default
  --saturation auto|N     saturation multiplier; auto=default
  --backlight-compensation N
  --white-balance auto|N  auto white balance or color temperature (K)
  --focus auto|N          auto-focus or manual value
  --exposure-compensation N  exposure multiplier (0 = aperture-priority AE)

  --config PATH           config file path
  --help
```

## Configuration

Optional YAML config file, searched in order:

1. `--config PATH` (if given)
2. `$XDG_CONFIG_HOME/WebCamProxy/config.yml`
3. `$HOME/.config/WebCamProxy/config.yml`
4. `/etc/WebCamProxy/config.yml`

CLI options override config values. The config file is watched with inotify;
changes take effect after the current client disconnects.

Example (`config.example.yml`):

```yaml
rotate: 180
#source: /dev/video2
#input_frame_size: 1280x720
#input_format: mjpg
#output_aspect_ratio: 16:9
#brightness: auto
```

## Install

User-local:
```bash
cmake -B build -G Ninja -DCMAKE_INSTALL_PREFIX=$HOME/.local
ninja -C build
cmake --install build
systemctl --user enable webcamproxy
systemctl --user start webcamproxy
```

System-wide:
```bash
cmake -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr
ninja -C build
sudo cmake --install build
sudo systemctl enable webcamproxy
sudo systemctl start webcamproxy
```

## Passwordless sudo

To avoid entering your password for device management:

```bash
sudo visudo -f /etc/sudoers.d/webcamproxy
```

Add:
```
%wheel ALL=(ALL) NOPASSWD: /usr/sbin/modprobe v4l2loopback*, /usr/bin/v4l2loopback-ctl *
```

## License

MIT — see [LICENSE](LICENSE).
