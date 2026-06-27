#pragma once
#include <cstdint>
#include <cstddef>

// Decode MJPEG to YUYV.  Returns true on success.
bool mjpeg_to_yuyv(const uint8_t* jpeg_in, size_t jpeg_len,
                   uint8_t* yuyv_out, uint32_t width, uint32_t height);

// Encode YUYV to MJPEG.  Returns JPEG size, 0 on failure.
size_t yuyv_to_mjpeg(const uint8_t* yuyv, uint8_t* jpeg_out,
                     size_t jpeg_cap, uint32_t width, uint32_t height,
                     int quality);

bool mjpeg_rotate(const uint8_t* jpeg_in, size_t jpeg_len,
                  uint8_t** jpeg_out, size_t* jpeg_out_len, int angle);

bool mjpeg_transform(const uint8_t* jpeg_in, size_t jpeg_len,
                     uint8_t** jpeg_out, size_t* jpeg_out_len,
                     int angle,
                     uint32_t crop_x, uint32_t crop_y,
                     uint32_t crop_w, uint32_t crop_h);

size_t mjpeg_strip_app(uint8_t* data, size_t len);

void compute_crop(uint32_t in_w, uint32_t in_h, int num, int den,
                  uint32_t& crop_x, uint32_t& crop_y,
                  uint32_t& crop_w, uint32_t& crop_h);

void map_crop_to_input(uint32_t ox, uint32_t oy, uint32_t ow, uint32_t oh,
                       uint32_t in_w, uint32_t in_h, int angle,
                       uint32_t& ix, uint32_t& iy,
                       uint32_t& iw, uint32_t& ih);

void yuyv_crop(uint8_t* data, uint32_t in_w, uint32_t in_h,
               uint32_t crop_x, uint32_t crop_y,
               uint32_t crop_w, uint32_t crop_h, uint8_t* dst);

// Software fallback: MJPEG → decode → rotate → crop → re-encode MJPEG
bool mjpeg_soft_transform(const uint8_t* jpeg_in, size_t jpeg_len,
                          uint8_t** jpeg_out, size_t* jpeg_out_len,
                          int angle,
                          uint32_t crop_x, uint32_t crop_y,
                          uint32_t crop_w, uint32_t crop_h,
                          uint32_t in_w, uint32_t in_h);
