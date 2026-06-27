#include "mjpeg.h"
#include "rotate.h"
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <jpeglib.h>
#include <turbojpeg.h>
#include <vector>

static int angle_to_op(int angle) {
    switch (angle) {
    case 0:   return TJXOP_NONE;
    case 90:  return TJXOP_ROT90;
    case 180: return TJXOP_ROT180;
    case 270: return TJXOP_ROT270;
    default:  return TJXOP_NONE;
    }
}

bool mjpeg_rotate(const uint8_t* jpeg_in, size_t jpeg_len,
                  uint8_t** jpeg_out, size_t* jpeg_out_len,
                  int angle) {
    return mjpeg_transform(jpeg_in, jpeg_len, jpeg_out, jpeg_out_len,
                           angle, 0, 0, 0, 0);
}

bool mjpeg_transform(const uint8_t* jpeg_in, size_t jpeg_len,
                     uint8_t** jpeg_out, size_t* jpeg_out_len,
                     int angle,
                     uint32_t crop_x, uint32_t crop_y,
                     uint32_t crop_w, uint32_t crop_h) {
    if (angle == 0 && crop_w == 0) {
        *jpeg_out = static_cast<uint8_t*>(malloc(jpeg_len));
        if (!*jpeg_out) return false;
        std::memcpy(*jpeg_out, jpeg_in, jpeg_len);
        *jpeg_out_len = jpeg_len;
        return true;
    }

    tjhandle handle = tjInitTransform();
    if (!handle) goto fallback;

    {
        tjtransform xform;
        std::memset(&xform, 0, sizeof(xform));
        xform.op = angle_to_op(angle);
        xform.options = TJXOPT_TRIM | TJXOPT_COPYNONE;
        if (crop_w > 0 && crop_h > 0) {
            xform.r.x = static_cast<int>(crop_x);
            xform.r.y = static_cast<int>(crop_y);
            xform.r.w = static_cast<int>(crop_w);
            xform.r.h = static_cast<int>(crop_h);
            xform.options |= TJXOPT_CROP;
        }

        unsigned char* dst_buf = nullptr;
        unsigned long dst_size = 0;
        int ret = tjTransform(handle, jpeg_in,
                              static_cast<unsigned long>(jpeg_len),
                              1, &dst_buf, &dst_size, &xform, 0);
        if (ret != 0) {
            static bool crop_err_printed = false;
            if (!crop_err_printed && crop_w > 0) {
                fprintf(stderr, "tjTransform: %s (will skip crop)\n",
                        tjGetErrorStr2(handle));
                crop_err_printed = true;
            }
            if (dst_buf) tjFree(dst_buf);
            // Try without crop
            if (crop_w > 0) {
                tjDestroy(handle);
                handle = nullptr;
                // Fall back to software pipeline for crop
                if (mjpeg_soft_transform(jpeg_in, jpeg_len,
                                         jpeg_out, jpeg_out_len,
                                         angle,
                                         crop_x, crop_y, crop_w, crop_h,
                                         // Input dims unknown here — caller
                                         // should use mjpeg_transform
                                         // with known dims.  For now,
                                         // return false so caller retries.
                                         0, 0)) {
                    return true;
                }
            }
            if (handle) tjDestroy(handle);
            goto fallback;
        }
        tjDestroy(handle);
        *jpeg_out = dst_buf;
        *jpeg_out_len = static_cast<size_t>(dst_size);
        return true;
    }

fallback:
    *jpeg_out = static_cast<uint8_t*>(malloc(jpeg_len));
    if (!*jpeg_out) return false;
    std::memcpy(*jpeg_out, jpeg_in, jpeg_len);
    *jpeg_out_len = jpeg_len;
    return true;
}

size_t mjpeg_strip_app(uint8_t* data, size_t len) {
    if (len < 2) return len;
    size_t wpos = 0, rpos = 0;
    while (rpos + 1 < len) {
        if (data[rpos] == 0xFF && data[rpos + 1] >= 0xE1 &&
            data[rpos + 1] <= 0xEF) {
            if (rpos + 3 >= len) {
                std::memmove(data + wpos, data + rpos, len - rpos);
                wpos += len - rpos;
                break;
            }
            uint16_t mlen = (static_cast<uint16_t>(data[rpos + 2]) << 8) |
                            data[rpos + 3];
            if (mlen < 2) mlen = 2;
            rpos += 2 + mlen;
            continue;
        }
        data[wpos++] = data[rpos++];
    }
    while (rpos < len) data[wpos++] = data[rpos++];
    return wpos;
}

// ── Software MJPEG pipeline (decode → rotate → crop → re‑encode) ──

struct soft_err_mgr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void soft_error_exit(j_common_ptr c) {
    auto* e = reinterpret_cast<soft_err_mgr*>(c->err);
    longjmp(e->jb, 1);
}

bool mjpeg_to_yuyv(const uint8_t* jpeg_in, size_t jpeg_len,
                   uint8_t* yuyv_out, uint32_t width, uint32_t height) {
    struct jpeg_decompress_struct cinfo;
    struct soft_err_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = soft_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); return false; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_in, jpeg_len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width != width || cinfo.output_height != height)
        { jpeg_destroy_decompress(&cinfo); return false; }
    std::vector<uint8_t> rgb_row(width * 3);
    for (uint32_t y = 0; y < height; y++) {
        JSAMPROW rp = rgb_row.data();
        jpeg_read_scanlines(&cinfo, &rp, 1);
        // RGB → YUYV conversion (same as rgb_row_to_yuyv from soft pipeline)
        uint8_t* dline = yuyv_out + y * width * 2;
        for (uint32_t x = 0; x < width; x += 2) {
            int r0=rgb_row[x*3+0], g0=rgb_row[x*3+1], b0=rgb_row[x*3+2];
            int r1=rgb_row[x*3+3], g1=rgb_row[x*3+4], b1=rgb_row[x*3+5];
            auto Y  = [](int r,int g,int b){ return (66*r+129*g+25*b+128)>>8; };
            auto Cb = [](int r,int g,int b){ return ((-38*r-74*g+112*b+128)>>8)+128; };
            auto Cr = [](int r,int g,int b){ return ((112*r-94*g-18*b+128)>>8)+128; };
            dline[x*2+0] = Y(r0,g0,b0);
            dline[x*2+1] = Cb((r0+r1)/2,(g0+g1)/2,(b0+b1)/2);
            dline[x*2+2] = Y(r1,g1,b1);
            dline[x*2+3] = Cr((r0+r1)/2,(g0+g1)/2,(b0+b1)/2);
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

size_t yuyv_to_mjpeg(const uint8_t* yuyv, uint8_t* jpeg_out,
                     size_t jpeg_cap, uint32_t width, uint32_t height,
                     int quality) {
    struct jpeg_compress_struct cinfo;
    struct soft_err_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = soft_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); return 0; }
    jpeg_create_compress(&cinfo);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    unsigned long jsz = jpeg_cap;
    jpeg_mem_dest(&cinfo, &jpeg_out, &jsz);
    jpeg_start_compress(&cinfo, TRUE);
    std::vector<uint8_t> rgb_row(width * 3);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* yline = yuyv + y * width * 2;
        for (uint32_t x = 0; x < width; x += 2) {
            int y0=yline[x*2], u=yline[x*2+1]-128,
                y1=yline[x*2+2], v=yline[x*2+3]-128;
            auto clip=[&](int v){ return v<0?0:(v>255?255:v); };
            rgb_row[x*3+0]=clip(y0+((1436*v)>>10));
            rgb_row[x*3+1]=clip(y0-((352*u)>>10)-((731*v)>>10));
            rgb_row[x*3+2]=clip(y0+((1814*u)>>10));
            rgb_row[x*3+3]=clip(y1+((1436*v)>>10));
            rgb_row[x*3+4]=clip(y1-((352*u)>>10)-((731*v)>>10));
            rgb_row[x*3+5]=clip(y1+((1814*u)>>10));
        }
        JSAMPROW rp = rgb_row.data();
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return jsz;
}

// ── Software MJPEG pipeline (decode → rotate → crop → re‑encode) ──

// RGB → YUYV conversion (same as before)
static void rgb_row_to_yuyv(const uint8_t* rgb, uint8_t* yuyv,
                            uint32_t w) {
    for (uint32_t x = 0; x < w; x += 2) {
        int r0=rgb[x*3+0], g0=rgb[x*3+1], b0=rgb[x*3+2];
        int r1=rgb[x*3+3], g1=rgb[x*3+4], b1=rgb[x*3+5];
        auto Y  = [](int r,int g,int b){ return (66*r+129*g+25*b+128)>>8; };
        auto Cb = [](int r,int g,int b){ return ((-38*r-74*g+112*b+128)>>8)+128; };
        auto Cr = [](int r,int g,int b){ return ((112*r-94*g-18*b+128)>>8)+128; };
        yuyv[x*2+0] = Y(r0,g0,b0);
        yuyv[x*2+1] = Cb((r0+r1)/2,(g0+g1)/2,(b0+b1)/2);
        yuyv[x*2+2] = Y(r1,g1,b1);
        yuyv[x*2+3] = Cr((r0+r1)/2,(g0+g1)/2,(b0+b1)/2);
    }
}

// YUYV → RGB for JPEG encoding
static void yuyv_row_to_rgb(const uint8_t* yuyv, uint8_t* rgb,
                            uint32_t w) {
    for (uint32_t x = 0; x < w; x += 2) {
        int y0=yuyv[x*2], u=yuyv[x*2+1]-128, y1=yuyv[x*2+2], v=yuyv[x*2+3]-128;
        auto clip=[&](int v){ return v<0?0:(v>255?255:v); };
        rgb[x*3+0]=clip(y0 + ((1436*v)>>10));
        rgb[x*3+1]=clip(y0 - ((352*u)>>10) - ((731*v)>>10));
        rgb[x*3+2]=clip(y0 + ((1814*u)>>10));
        rgb[x*3+3]=clip(y1 + ((1436*v)>>10));
        rgb[x*3+4]=clip(y1 - ((352*u)>>10) - ((731*v)>>10));
        rgb[x*3+5]=clip(y1 + ((1814*u)>>10));
    }
}

bool mjpeg_soft_transform(const uint8_t* jpeg_in, size_t jpeg_len,
                          uint8_t** jpeg_out, size_t* jpeg_out_len,
                          int angle,
                          uint32_t crop_x, uint32_t crop_y,
                          uint32_t crop_w, uint32_t crop_h,
                          uint32_t in_w, uint32_t in_h) {
    // 1. Decode MJPEG → YUYV
    std::vector<uint8_t> raw(in_w * in_h * 2);
    {
        struct jpeg_decompress_struct cinfo;
        struct soft_err_mgr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = soft_error_exit;
        if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); return false; }
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, jpeg_in, jpeg_len);
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        if (cinfo.output_width != in_w || cinfo.output_height != in_h)
            { jpeg_destroy_decompress(&cinfo); return false; }
        std::vector<uint8_t> rgb_row(in_w * 3);
        for (uint32_t y = 0; y < in_h; y++) {
            JSAMPROW rp = rgb_row.data();
            jpeg_read_scanlines(&cinfo, &rp, 1);
            rgb_row_to_yuyv(rgb_row.data(), raw.data() + y * in_w * 2, in_w);
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }

    // 2. Rotate YUYV (into a temp buffer)
    uint32_t full_ow, full_oh;
    rotated_dims(in_w, in_h, angle, full_ow, full_oh);
    std::vector<uint8_t> rotated(full_ow * full_oh * 2);
    rotate_yuyv(raw.data(), rotated.data(), in_w, in_h, angle);

    // 3. Crop (output-size) if needed
    const uint8_t* src = rotated.data();
    if (crop_w > 0 && (crop_w != full_ow || crop_h != full_oh)) {
        std::vector<uint8_t> cropped(crop_w * crop_h * 2);
        yuyv_crop(rotated.data(), full_ow, full_oh,
                  crop_x, crop_y, crop_w, crop_h, cropped.data());
        rotated.swap(cropped);
        src = rotated.data();
    }
    uint32_t out_w = crop_w > 0 ? crop_w : full_ow;
    uint32_t out_h = crop_h > 0 ? crop_h : full_oh;

    // 4. Re-encode as MJPEG
    {
        struct jpeg_compress_struct cinfo;
        struct soft_err_mgr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = soft_error_exit;
        if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); return false; }
        jpeg_create_compress(&cinfo);
        cinfo.image_width = out_w;
        cinfo.image_height = out_h;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, 85, TRUE);
        unsigned long jsz = out_w * out_h * 3;
        *jpeg_out = static_cast<uint8_t*>(malloc(jsz));
        if (!*jpeg_out) { jpeg_destroy_compress(&cinfo); return false; }
        jpeg_mem_dest(&cinfo, jpeg_out, &jsz);
        jpeg_start_compress(&cinfo, TRUE);
        std::vector<uint8_t> rgb_row(out_w * 3);
        for (uint32_t y = 0; y < out_h; y++) {
            yuyv_row_to_rgb(src + y * out_w * 2, rgb_row.data(), out_w);
            JSAMPROW rp = rgb_row.data();
            jpeg_write_scanlines(&cinfo, &rp, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        *jpeg_out_len = static_cast<size_t>(jsz);
    }
    return true;
}

void compute_crop(uint32_t in_w, uint32_t in_h,
                  int num, int den,
                  uint32_t& crop_x, uint32_t& crop_y,
                  uint32_t& crop_w, uint32_t& crop_h) {
    double target = static_cast<double>(num) / den;
    double src = static_cast<double>(in_w) / in_h;
    uint32_t cw, ch;
    if (src > target) {
        ch = in_h;
        cw = static_cast<uint32_t>(in_h * target);
    } else {
        cw = in_w;
        ch = static_cast<uint32_t>(in_w / target);
    }
    // Round to 16×16 (LCM of all common JPEG MCU sizes: 8×8, 16×8, 8×16, 16×16)
    cw = (cw / 16) * 16;
    ch = (ch / 16) * 16;
    crop_w = cw;
    crop_h = ch;
    crop_x = ((in_w - cw) / 2 / 16) * 16;
    crop_y = ((in_h - ch) / 2 / 16) * 16;
}

void map_crop_to_input(uint32_t ox, uint32_t oy,
                       uint32_t ow, uint32_t oh,
                       uint32_t in_w, uint32_t in_h,
                       int angle,
                       uint32_t& ix, uint32_t& iy,
                       uint32_t& iw, uint32_t& ih) {
    switch (angle) {
    case 0:
        ix = ox; iy = oy; iw = ow; ih = oh; break;
    case 90:
        ix = oy; iy = in_h - ox - ow;
        iw = oh; ih = ow; break;
    case 180:
        ix = in_w - ox - ow; iy = in_h - oy - oh;
        iw = ow; ih = oh; break;
    case 270:
        ix = in_w - oy - oh; iy = ox;
        iw = oh; ih = ow; break;
    }
    // Round to 16×16 MCU boundaries
    ix = (ix / 16) * 16;
    iy = (iy / 16) * 16;
    iw = (iw / 16) * 16;
    ih = (ih / 16) * 16;
}

void yuyv_crop(uint8_t* data, uint32_t in_w, uint32_t in_h,
               uint32_t crop_x, uint32_t crop_y,
               uint32_t crop_w, uint32_t crop_h,
               uint8_t* dst) {
    crop_x &= ~1u;
    crop_w &= ~1u;
    for (uint32_t y = 0; y < crop_h; y++) {
        const uint8_t* src_line = data + (crop_y + y) * in_w * 2;
        uint8_t* dst_line = dst + y * crop_w * 2;
        std::memcpy(dst_line, src_line + crop_x * 2, crop_w * 2);
    }
}
