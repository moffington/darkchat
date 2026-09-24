#include "chat/core/image_policy.h"
#include <string.h>

ChatImagePlan chat_image_plan(const ChatImageFacts *facts, uint8_t *out_flags) {
    bool multi;
    if (out_flags) *out_flags = 0;
    if (!facts) return CHAT_IMAGE_REJECT_DIMENSIONS;
    if (facts->byte_length > (size_t)CHAT_ATTACHMENT_MAX_BYTES)
        return CHAT_IMAGE_REJECT_TOO_LARGE;
    if (facts->width > CHAT_ATTACHMENT_MAX_PIXELS ||
            facts->height > CHAT_ATTACHMENT_MAX_PIXELS)
        return CHAT_IMAGE_REJECT_DIMENSIONS;
    multi = facts->frame_count > 1;
    if (multi && out_flags) *out_flags = CHAT_PART_FLAG_FIRST_FRAME;
    if (facts->from_clipboard) return CHAT_IMAGE_NORMALIZE_PNG;
    switch (facts->format) {
    case CHAT_IMAGE_FORMAT_JPEG:
        if (multi ||
                (facts->exif_orientation != 0 && facts->exif_orientation != 1))
            return CHAT_IMAGE_NORMALIZE_JPEG;
        return CHAT_IMAGE_STORE_AS_IS;
    case CHAT_IMAGE_FORMAT_PNG:
        return multi ? CHAT_IMAGE_NORMALIZE_PNG : CHAT_IMAGE_STORE_AS_IS;
    case CHAT_IMAGE_FORMAT_WEBP:
    case CHAT_IMAGE_FORMAT_GIF:
        return multi ? CHAT_IMAGE_NORMALIZE_PNG : CHAT_IMAGE_STORE_AS_IS;
    case CHAT_IMAGE_FORMAT_OTHER:
    default:
        return CHAT_IMAGE_NORMALIZE_PNG;
    }
}

/* ---- passthrough container integrity ---- */

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_bytes(const unsigned char *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    size_t i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^
                (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* PNG: signature, chunk framing, per-chunk CRC, at least one IDAT, IEND last
   and exactly at the end. */
static bool png_intact(const unsigned char *bytes, size_t length) {
    static const unsigned char signature[8] =
        { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    size_t pos = 8;
    bool saw_idat = false;
    if (length < 8 || memcmp(bytes, signature, 8) != 0) return false;
    while (pos + 12 <= length) {
        uint32_t chunk = read_be32(bytes + pos);
        uint32_t stored_crc, actual_crc;
        const unsigned char *type = bytes + pos + 4;
        if (chunk > length - pos - 12) return false;
        stored_crc = read_be32(bytes + pos + 8 + chunk);
        actual_crc = crc32_bytes(type, (size_t)chunk + 4);
        if (stored_crc != actual_crc) return false;
        if (memcmp(type, "IDAT", 4) == 0) saw_idat = true;
        if (memcmp(type, "IEND", 4) == 0)
            return chunk == 0 && saw_idat && pos + 12 == length;
        pos += 12 + chunk;
    }
    return false;
}

static bool jpeg_intact(const unsigned char *bytes, size_t length) {
    return length >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
        bytes[length - 2] == 0xff && bytes[length - 1] == 0xd9;
}

static bool gif_intact(const unsigned char *bytes, size_t length) {
    static const char *versions[2] = { "GIF87a", "GIF89a" };
    int i;
    if (length < 7) return false;
    for (i = 0; i < 2; i++)
        if (memcmp(bytes, versions[i], 6) == 0)
            return bytes[length - 1] == 0x3b;
    return false;
}

static bool webp_intact(const unsigned char *bytes, size_t length) {
    if (length < 12) return false;
    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WEBP", 4) != 0)
        return false;
    return (size_t)read_le32(bytes + 4) + 8u == length;
}

bool chat_image_bytes_intact(const void *bytes, size_t length,
    ChatImageFormat format) {
    const unsigned char *data = (const unsigned char *)bytes;
    if (!data || length == 0) return false;
    switch (format) {
    case CHAT_IMAGE_FORMAT_PNG: return png_intact(data, length);
    case CHAT_IMAGE_FORMAT_JPEG: return jpeg_intact(data, length);
    case CHAT_IMAGE_FORMAT_GIF: return gif_intact(data, length);
    case CHAT_IMAGE_FORMAT_WEBP: return webp_intact(data, length);
    default: return true;
    }
}

