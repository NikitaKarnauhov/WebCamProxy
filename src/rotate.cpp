#include "rotate.h"

void rotate_yuyv_180(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    uint32_t bytes_per_line = width * 2;
    uint32_t macropixels_per_line = width / 2;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = height - 1 - y;
        const uint8_t* src_line = src + src_y * bytes_per_line;
        uint8_t* dst_line = dst + y * bytes_per_line;

        for (uint32_t mx = 0; mx < macropixels_per_line; mx++) {
            uint32_t src_mx = macropixels_per_line - 1 - mx;
            const uint8_t* src_mp = src_line + src_mx * 4;
            uint8_t* dst_mp = dst_line + mx * 4;

            dst_mp[0] = src_mp[2];
            dst_mp[1] = src_mp[1];
            dst_mp[2] = src_mp[0];
            dst_mp[3] = src_mp[3];
        }
    }
}
