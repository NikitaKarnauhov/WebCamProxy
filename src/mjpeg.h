#pragma once
#include <cstdint>
#include <cstddef>

bool mjpeg_rotate(const uint8_t* jpeg_in, size_t jpeg_len,
                  uint8_t** jpeg_out, size_t* jpeg_out_len,
                  int angle);

// Strip APP1–APP15 markers from JPEG data (keep APP0/JFIF).
// Modifies data in-place; returns new size (≤ original).
size_t mjpeg_strip_app(uint8_t* jpeg_data, size_t jpeg_len);
