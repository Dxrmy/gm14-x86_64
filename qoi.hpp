#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>

namespace gm14 {

struct QoiDecoded {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

inline bool decode_gm_qoi(const uint8_t* bytes, size_t size, QoiDecoded& out) {
    if (size < 12) return false;
    if (bytes[0] != 'f' || bytes[1] != 'i' || bytes[2] != 'o' || bytes[3] != 'q')
        return false;

    int width = bytes[4] | (bytes[5] << 8);
    int height = bytes[6] | (bytes[7] << 8);
    int length = bytes[8] | (bytes[9] << 8) | (bytes[10] << 16) | (bytes[11] << 24);

    if (width <= 0 || height <= 0) return false;
    if (12 + (size_t)length > size) length = (int)(size - 12);

    const uint8_t* pixelData = bytes + 12;
    int pos = 0;
    int run = 0;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint8_t index[64 * 4] = {0};

    out.width = width;
    out.height = height;
    out.rgba.resize((size_t)width * height * 4);
    size_t rawDataLength = out.rgba.size();
    uint8_t* rawData = out.rgba.data();

    const uint8_t QOI_INDEX = 0x00;
    const uint8_t QOI_RUN_8 = 0x40;
    const uint8_t QOI_RUN_16 = 0x60;
    const uint8_t QOI_DIFF_8 = 0x80;
    const uint8_t QOI_DIFF_16 = 0xc0;
    const uint8_t QOI_DIFF_24 = 0xe0;
    const uint8_t QOI_COLOR = 0xf0;
    const uint8_t QOI_MASK_2 = 0xc0;
    const uint8_t QOI_MASK_3 = 0xe0;
    const uint8_t QOI_MASK_4 = 0xf0;

    for (size_t rawDataPos = 0; rawDataPos < rawDataLength; rawDataPos += 4) {
        if (run > 0) {
            run--;
        } else if (pos < length) {
            int b1 = pixelData[pos++];
            if ((b1 & QOI_MASK_2) == QOI_INDEX) {
                int indexPos = (b1 ^ QOI_INDEX) << 2;
                r = index[indexPos];
                g = index[indexPos + 1];
                b = index[indexPos + 2];
                a = index[indexPos + 3];
            } else if ((b1 & QOI_MASK_3) == QOI_RUN_8) {
                run = b1 & 0x1f;
            } else if ((b1 & QOI_MASK_3) == QOI_RUN_16) {
                int b2 = (pos < length) ? pixelData[pos++] : 0;
                run = (((b1 & 0x1f) << 8) | b2) + 32;
            } else if ((b1 & QOI_MASK_2) == QOI_DIFF_8) {
                r += (uint8_t)(((int32_t)(b1 & 48) << 26 >> 30) & 0xff);
                g += (uint8_t)(((int32_t)(b1 & 12) << 28 >> 22 >> 8) & 0xff);
                b += (uint8_t)(((int32_t)(b1 & 3) << 30 >> 14 >> 16) & 0xff);
            } else if ((b1 & QOI_MASK_3) == QOI_DIFF_16) {
                int b2 = (pos < length) ? pixelData[pos++] : 0;
                int merged = (b1 << 8) | b2;
                r += (uint8_t)(((int32_t)(merged & 7936) << 19 >> 27) & 0xff);
                g += (uint8_t)(((int32_t)(merged & 240) << 24 >> 20 >> 8) & 0xff);
                b += (uint8_t)(((int32_t)(merged & 15) << 28 >> 12 >> 16) & 0xff);
            } else if ((b1 & QOI_MASK_4) == QOI_DIFF_24) {
                int b2 = (pos < length) ? pixelData[pos++] : 0;
                int b3 = (pos < length) ? pixelData[pos++] : 0;
                int merged = (b1 << 16) | (b2 << 8) | b3;
                r += (uint8_t)(((int32_t)(merged & 1015808) << 12 >> 27) & 0xff);
                g += (uint8_t)(((int32_t)(merged & 31744) << 17 >> 19 >> 8) & 0xff);
                b += (uint8_t)(((int32_t)(merged & 992) << 22 >> 11 >> 16) & 0xff);
                a += (uint8_t)(((int32_t)(merged & 31) << 27 >> 3 >> 24) & 0xff);
            } else if ((b1 & QOI_MASK_4) == QOI_COLOR) {
                if (b1 & 8) r = (pos < length) ? pixelData[pos++] : 0;
                if (b1 & 4) g = (pos < length) ? pixelData[pos++] : 0;
                if (b1 & 2) b = (pos < length) ? pixelData[pos++] : 0;
                if (b1 & 1) a = (pos < length) ? pixelData[pos++] : 0;
            }

            int indexPos2 = ((r ^ g ^ b ^ a) & 63) << 2;
            index[indexPos2] = r;
            index[indexPos2 + 1] = g;
            index[indexPos2 + 2] = b;
            index[indexPos2 + 3] = a;
        }

        rawData[rawDataPos + 0] = r;
        rawData[rawDataPos + 1] = g;
        rawData[rawDataPos + 2] = b;
        rawData[rawDataPos + 3] = a;
    }
    return true;
}

} // namespace gm14
