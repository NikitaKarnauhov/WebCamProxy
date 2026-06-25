#include "mjpeg.h"
#include <cstring>
#include <cstdlib>
#include <turbojpeg.h>

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
    if (angle == 0) {
        *jpeg_out = static_cast<uint8_t*>(malloc(jpeg_len));
        if (!*jpeg_out) return false;
        std::memcpy(*jpeg_out, jpeg_in, jpeg_len);
        *jpeg_out_len = jpeg_len;
        return true;
    }

    tjhandle handle = tjInitTransform();
    if (!handle) {
        *jpeg_out = static_cast<uint8_t*>(malloc(jpeg_len));
        if (!*jpeg_out) return false;
        std::memcpy(*jpeg_out, jpeg_in, jpeg_len);
        *jpeg_out_len = jpeg_len;
        return true;
    }

    tjtransform xform;
    std::memset(&xform, 0, sizeof(xform));
    xform.op = angle_to_op(angle);
    xform.options = TJXOPT_TRIM;

    unsigned char* dst_buf = nullptr;
    unsigned long dst_size = 0;

    int ret = tjTransform(handle, jpeg_in,
                          static_cast<unsigned long>(jpeg_len),
                          1, &dst_buf, &dst_size, &xform, 0);
    tjDestroy(handle);

    if (ret != 0 || !dst_buf) {
        *jpeg_out = static_cast<uint8_t*>(malloc(jpeg_len));
        if (!*jpeg_out) return false;
        std::memcpy(*jpeg_out, jpeg_in, jpeg_len);
        *jpeg_out_len = jpeg_len;
        return true;
    }

    *jpeg_out = dst_buf;
    *jpeg_out_len = static_cast<size_t>(dst_size);
    return true;
}
