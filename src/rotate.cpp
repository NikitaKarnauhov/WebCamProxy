#include "rotate.h"
#include <vector>

void rotated_dims(uint32_t in_w, uint32_t in_h, int angle,
                  uint32_t& out_w, uint32_t& out_h) {
    if (angle == 90 || angle == 270) {
        out_w = in_h;
        out_h = in_w;
    } else {
        out_w = in_w;
        out_h = in_h;
    }
}

static void rotate_0(const uint8_t* src, uint8_t* dst,
                     uint32_t width, uint32_t height) {
    std::memcpy(dst, src, static_cast<size_t>(width) * height * 2);
}

static void rotate_180(const uint8_t* src, uint8_t* dst,
                       uint32_t width, uint32_t height) {
    uint32_t bpl = width * 2;
    uint32_t mps = width / 2;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* sline = src + (height - 1 - y) * bpl;
        uint8_t* dline = dst + y * bpl;
        for (uint32_t mx = 0; mx < mps; mx++) {
            const uint8_t* s = sline + (mps - 1 - mx) * 4;
            uint8_t* d = dline + mx * 4;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
}

// Extract Y, U, V per pixel from YUYV into planar arrays
static void unpack_yuyv(const uint8_t* src,
                        std::vector<uint8_t>& Y,
                        std::vector<uint8_t>& U,
                        std::vector<uint8_t>& V,
                        uint32_t width, uint32_t height) {
    Y.resize(static_cast<size_t>(width) * height);
    U.resize(static_cast<size_t>(width) * height);
    V.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* line = src + y * width * 2;
        for (uint32_t mx = 0; mx < width / 2; mx++) {
            const uint8_t* mp = line + mx * 4;
            uint32_t base = static_cast<uint32_t>(y) * width + 2 * mx;
            Y[base]     = mp[0];
            U[base] = U[base+1] = mp[1];
            Y[base + 1] = mp[2];
            V[base] = V[base+1] = mp[3];
        }
    }
}

// Pack planar YUV back to YUYV
static void pack_yuyv(uint8_t* dst,
                      const std::vector<uint8_t>& Y,
                      const std::vector<uint8_t>& U,
                      const std::vector<uint8_t>& V,
                      uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* line = dst + y * width * 2;
        for (uint32_t mx = 0; mx < width / 2; mx++) {
            uint32_t base = static_cast<uint32_t>(y) * width + 2 * mx;
            uint8_t* mp = line + mx * 4;
            mp[0] = Y[base];
            mp[1] = static_cast<uint8_t>(
                (static_cast<uint16_t>(U[base]) + U[base+1]) / 2);
            mp[2] = Y[base + 1];
            mp[3] = static_cast<uint8_t>(
                (static_cast<uint16_t>(V[base]) + V[base+1]) / 2);
        }
    }
}

// Rotate planar data
static void rotate_plane(const std::vector<uint8_t>& src,
                         std::vector<uint8_t>& dst,
                         uint32_t sw, uint32_t sh, int angle) {
    uint32_t dw = (angle == 90 || angle == 270) ? sh : sw;
    uint32_t dh = (angle == 90 || angle == 270) ? sw : sh;
    dst.resize(static_cast<size_t>(dw) * dh);

    for (uint32_t dy = 0; dy < dh; dy++) {
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t sx, sy;
            switch (angle) {
            case 0:
                sx = dx; sy = dy; break;
            case 90:
                sx = dy; sy = sh - 1 - dx; break;
            case 180:
                sx = sw - 1 - dx; sy = sh - 1 - dy; break;
            case 270:
                sx = sw - 1 - dy; sy = dx; break;
            default:
                sx = dx; sy = dy;
            }
            dst[static_cast<size_t>(dy) * dw + dx] =
                src[static_cast<size_t>(sy) * sw + sx];
        }
    }
}

static void rotate_packed(const uint8_t* src, uint8_t* dst,
                          uint32_t in_w, uint32_t in_h, int angle) {
    std::vector<uint8_t> Y, U, V, Yr, Ur, Vr;
    unpack_yuyv(src, Y, U, V, in_w, in_h);
    rotate_plane(Y, Yr, in_w, in_h, angle);
    rotate_plane(U, Ur, in_w, in_h, angle);
    rotate_plane(V, Vr, in_w, in_h, angle);
    uint32_t out_w = (angle == 90 || angle == 270) ? in_h : in_w;
    uint32_t out_h = (angle == 90 || angle == 270) ? in_w : in_h;
    pack_yuyv(dst, Yr, Ur, Vr, out_w, out_h);
}

void rotate_yuyv(const uint8_t* src, uint8_t* dst,
                 uint32_t in_width, uint32_t in_height, int angle) {
    switch (angle) {
    case 0:
        rotate_0(src, dst, in_width, in_height);
        break;
    case 180:
        rotate_180(src, dst, in_width, in_height);
        break;
    default:
        rotate_packed(src, dst, in_width, in_height, angle);
        break;
    }
}
