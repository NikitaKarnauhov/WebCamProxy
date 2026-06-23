#pragma once
#include <cstdint>
#include <cstring>

// Rotate a YUYV frame.  src and dst must be non-overlapping.
// For 90/270: out_width = in_height, out_height = in_width.
void rotate_yuyv(const uint8_t* src, uint8_t* dst,
                 uint32_t in_width, uint32_t in_height, int angle);

// Get output dimensions for a given rotation angle.
void rotated_dims(uint32_t in_w, uint32_t in_h, int angle,
                  uint32_t& out_w, uint32_t& out_h);
