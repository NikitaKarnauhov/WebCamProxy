#pragma once
#include <cstdint>
#include <cstddef>

// Lossless JPEG rotation using TurboJPEG (block-level, no decode/re-encode).
// jpeg_in / jpeg_len: input JPEG data
// jpeg_out: receives malloc'd output buffer (caller must free())
// jpeg_out_len: receives output size
// angle: 0 (passthrough), 90, 180, or 270
bool mjpeg_rotate(const uint8_t* jpeg_in, size_t jpeg_len,
                  uint8_t** jpeg_out, size_t* jpeg_out_len,
                  int angle);
