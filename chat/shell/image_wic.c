#include "chat/shell/image_wic.h"
#include "chat/core/image_policy.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- small helpers ---- */

static void copy_mime(char *dest, size_t cap, ChatImageFormat format) {
    const char *mime;
    size_t length;
    switch (format) {
    case CHAT_IMAGE_FORMAT_PNG: mime = "image/png"; break;
    case CHAT_IMAGE_FORMAT_JPEG: mime = "image/jpeg"; break;
    case CHAT_IMAGE_FORMAT_WEBP: mime = "image/webp"; break;
    case CHAT_IMAGE_FORMAT_GIF: mime = "image/gif"; break;
    default: mime = "application/octet-stream"; break;
    }
    if (!dest || cap == 0) return;
    length = strlen(mime);
    if (length >= cap) length = cap - 1;
    memcpy(dest, mime, length);
    dest[length] = 0;
}

static void set_display_name(wchar_t *dest, size_t cap, const wchar_t *path) {
    const wchar_t *name = path;
    const wchar_t *p;
    size_t length;
    if (!dest || cap == 0) return;
    dest[0] = 0;
    if (!path) return;
    for (p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    length = wcslen(name);
    if (length >= cap) length = cap - 1;
    wmemcpy(dest, name, length);
    dest[length] = 0;
}

static bool create_factory(IWICImagingFactory **out) {
    *out = NULL;
    return SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)out));
}

static ChatImageFormat container_format(IWICBitmapDecoder *decoder) {
    GUID guid;
    if (FAILED(decoder->lpVtbl->GetContainerFormat(decoder, &guid)))
        return CHAT_IMAGE_FORMAT_OTHER;
    if (IsEqualGUID(&guid, &GUID_ContainerFormatPng)) return CHAT_IMAGE_FORMAT_PNG;
    if (IsEqualGUID(&guid, &GUID_ContainerFormatJpeg)) return CHAT_IMAGE_FORMAT_JPEG;
    if (IsEqualGUID(&guid, &GUID_ContainerFormatGif)) return CHAT_IMAGE_FORMAT_GIF;
    if (IsEqualGUID(&guid, &GUID_ContainerFormatWebp)) return CHAT_IMAGE_FORMAT_WEBP;
    return CHAT_IMAGE_FORMAT_OTHER;
}

static unsigned read_orientation(IWICBitmapFrameDecode *frame) {
    IWICMetadataQueryReader *reader = NULL;
    PROPVARIANT value;
    unsigned orientation = 0;
    if (FAILED(frame->lpVtbl->GetMetadataQueryReader(frame, &reader)) || !reader)
        return 0;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->lpVtbl->GetMetadataByName(reader,
            L"System.Photo.Orientation", &value)) &&
            value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8)
        orientation = value.uiVal;
    PropVariantClear(&value);
    reader->lpVtbl->Release(reader);
    return orientation;
}

static WICBitmapTransformOptions orientation_transform(unsigned orientation) {
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 |
        WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 |
        WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

/* Encodes `source` into a freshly malloc'd buffer. PNG keeps alpha
   (32bppBGRA); JPEG is 24bppBGR at quality 0.85 and carries no alpha. */
static ChatImageIngestResult encode_source(IWICImagingFactory *factory,
    IWICBitmapSource *source, bool jpeg, unsigned char **out_bytes,
    size_t *out_n) {
    const GUID *container = jpeg ? &GUID_ContainerFormatJpeg
                                 : &GUID_ContainerFormatPng;
    WICPixelFormatGUID pixel_format = jpeg ? GUID_WICPixelFormat24bppBGR
                                           : GUID_WICPixelFormat32bppBGRA;
    IStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    HGLOBAL global = NULL;
    unsigned char *bytes = NULL;
    void *locked;
    STATSTG stat;
    UINT width = 0, height = 0;
    ChatImageIngestResult result = IMAGE_INGEST_IO;

    *out_bytes = NULL;
    *out_n = 0;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
        return IMAGE_INGEST_OOM;
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, container, NULL,
            &encoder)))
        goto done;
    if (FAILED(encoder->lpVtbl->Initialize(encoder, stream,
            WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options)))
        goto done;
    if (jpeg && options) {
        PROPBAG2 property;
        VARIANT value;
        memset(&property, 0, sizeof property);
        property.pstrName = (LPOLESTR)L"ImageQuality";
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.85f;
        /* Not every encoder advertises the quality option; ignore failure. */
        options->lpVtbl->Write(options, 1, &property, &value);
        VariantClear(&value);
    }
    if (FAILED(frame->lpVtbl->Initialize(frame, options)))
        goto done;
    if (FAILED(source->lpVtbl->GetSize(source, &width, &height)))
        goto done;
    if (FAILED(frame->lpVtbl->SetSize(frame, width, height)))
        goto done;
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &pixel_format)))
        goto done;
    if (FAILED(frame->lpVtbl->WriteSource(frame, source, NULL)))
        goto done;
    if (FAILED(frame->lpVtbl->Commit(frame)))
        goto done;
    if (FAILED(encoder->lpVtbl->Commit(encoder)))
        goto done;
    memset(&stat, 0, sizeof stat);
    if (FAILED(stream->lpVtbl->Stat(stream, &stat, STATFLAG_NONAME)))
        goto done;
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 0x7fffffff)
        goto done;
    bytes = (unsigned char *)malloc((size_t)stat.cbSize.QuadPart);
    if (!bytes) { result = IMAGE_INGEST_OOM; goto done; }
    if (FAILED(GetHGlobalFromStream(stream, &global)) || !global) {
        free(bytes);
        bytes = NULL;
        goto done;
    }
    locked = GlobalLock(global);
    if (!locked) {
        free(bytes);
        bytes = NULL;
        goto done;
    }
    memcpy(bytes, locked, (size_t)stat.cbSize.QuadPart);
    GlobalUnlock(global);
    *out_bytes = bytes;
    *out_n = (size_t)stat.cbSize.QuadPart;
    bytes = NULL;
    result = IMAGE_INGEST_OK;

done:
    free(bytes);
    if (options) options->lpVtbl->Release(options);
    if (frame) frame->lpVtbl->Release(frame);
    if (encoder) encoder->lpVtbl->Release(encoder);
    if (stream) stream->lpVtbl->Release(stream);
    return result;
}

/* Forces a full pixel decode so a file whose header/metadata parses but whose
   pixel payload is truncated or damaged is rejected instead of being stored
   as a passthrough. WIC separates frame properties (GetFrame/GetSize) from
   pixel access (CopyPixels), so converting and copying every pixel is the
   only real validation. */
static bool pixels_decode(IWICImagingFactory *factory,
    IWICBitmapSource *source) {
    IWICFormatConverter *converter = NULL;
    UINT width = 0, height = 0;
    size_t stride, size;
    unsigned char *buffer;
    bool ok = false;

    if (FAILED(factory->lpVtbl->CreateFormatConverter(factory, &converter)))
        return false;
    if (FAILED(converter->lpVtbl->Initialize(converter, source,
            &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom)))
        goto done;
    if (FAILED(converter->lpVtbl->GetSize(converter, &width, &height)))
        goto done;
    stride = (size_t)width * 4u;
    size = stride * (size_t)height;
    if (size == 0 || size > 0x7fffffffu) goto done;
    buffer = (unsigned char *)malloc(size);
    if (!buffer) goto done;
    if (SUCCEEDED(converter->lpVtbl->CopyPixels(converter, NULL,
            (UINT)stride, (UINT)size, buffer)))
        ok = true;
    free(buffer);

done:
    if (converter) converter->lpVtbl->Release(converter);
    return ok;
}

/* Applies the EXIF orientation transform (JPEG only) and the normalize-edge
   downscale, then encodes. `source` is borrowed. */
static ChatImageIngestResult transform_and_encode(IWICImagingFactory *factory,
    IWICBitmapSource *source, unsigned orientation, bool jpeg,
    ChatImageIngest *out) {
    IWICBitmapFlipRotator *rotator = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *current = source;
    UINT width = 0, height = 0;
    ChatImageIngestResult result;

    if (FAILED(current->lpVtbl->GetSize(current, &width, &height)))
        return IMAGE_INGEST_UNSUPPORTED;
    if (orientation > 1 && orientation <= 8) {
        WICBitmapTransformOptions options = orientation_transform(orientation);
        if (options != WICBitmapTransformRotate0) {
            /* A required transform: fail closed rather than return OK with an
               unrotated image that claims to be normalized. */
            if (FAILED(factory->lpVtbl->CreateBitmapFlipRotator(factory,
                    &rotator)) ||
                    FAILED(rotator->lpVtbl->Initialize(rotator, current,
                        options))) {
                result = IMAGE_INGEST_OOM;
                goto done;
            }
            current = (IWICBitmapSource *)rotator;
            if (FAILED(current->lpVtbl->GetSize(current, &width, &height))) {
                result = IMAGE_INGEST_UNSUPPORTED;
                goto done;
            }
        }
    }
    {
        uint32_t long_edge = width > height ? width : height;
        if (long_edge > CHAT_ATTACHMENT_NORMALIZE_EDGE) {
            double scale = (double)CHAT_ATTACHMENT_NORMALIZE_EDGE /
                (double)long_edge;
            UINT new_width = (UINT)(width * scale + 0.5);
            UINT new_height = (UINT)(height * scale + 0.5);
            if (new_width < 1) new_width = 1;
            if (new_height < 1) new_height = 1;
            /* A required downscale: fail closed rather than store an
               oversized normalized image. */
            if (FAILED(factory->lpVtbl->CreateBitmapScaler(factory, &scaler)) ||
                    FAILED(scaler->lpVtbl->Initialize(scaler, current,
                        new_width, new_height, WICBitmapInterpolationModeFant))) {
                result = IMAGE_INGEST_OOM;
                goto done;
            }
            current = (IWICBitmapSource *)scaler;
            width = new_width;
            height = new_height;
        }
    }
    result = encode_source(factory, current, jpeg, &out->bytes, &out->length);
    if (result == IMAGE_INGEST_OK) {
        out->meta.pixel_width = width;
        out->meta.pixel_height = height;
        copy_mime(out->meta.mime, sizeof out->meta.mime,
            jpeg ? CHAT_IMAGE_FORMAT_JPEG : CHAT_IMAGE_FORMAT_PNG);
    }

done:
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (rotator) rotator->lpVtbl->Release(rotator);
    return result;
}

/* ---- file source ---- */

static bool read_source_file(const wchar_t *path, unsigned char **out,
    size_t *out_n, ChatImageIngestResult *result) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *data;
    DWORD got = 0;

    *out = NULL;
    *out_n = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { *result = IMAGE_INGEST_IO; return false; }
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        *result = IMAGE_INGEST_IO;
        return false;
    }
    /* Byte cap first: an over-cap file is rejected without being read. */
    if ((uint64_t)size.QuadPart > (uint64_t)CHAT_ATTACHMENT_MAX_BYTES) {
        CloseHandle(file);
        *result = IMAGE_INGEST_TOO_LARGE;
        return false;
    }
    if (size.QuadPart == 0) {
        CloseHandle(file);
        *result = IMAGE_INGEST_EMPTY;
        return false;
    }
    data = (unsigned char *)malloc((size_t)size.QuadPart);
    if (!data) {
        CloseHandle(file);
        *result = IMAGE_INGEST_OOM;
        return false;
    }
    if (!ReadFile(file, data, (DWORD)size.QuadPart, &got, NULL) ||
            got != (DWORD)size.QuadPart) {
        free(data);
        CloseHandle(file);
        *result = IMAGE_INGEST_IO;
        return false;
    }
    CloseHandle(file);
    *out = data;
    *out_n = (size_t)size.QuadPart;
    return true;
}

static IWICBitmapDecoder *decoder_from_memory(IWICImagingFactory *factory,
    const unsigned char *data, size_t length, IStream **out_stream) {
    HGLOBAL block;
    void *locked;
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;

    *out_stream = NULL;
    block = GlobalAlloc(GMEM_MOVEABLE, length);
    if (!block) return NULL;
    locked = GlobalLock(block);
    if (!locked) { GlobalFree(block); return NULL; }
    memcpy(locked, data, length);
    GlobalUnlock(block);
    if (FAILED(CreateStreamOnHGlobal(block, TRUE, &stream))) {
        GlobalFree(block);
        return NULL;
    }
    if (FAILED(factory->lpVtbl->CreateDecoderFromStream(factory, stream, NULL,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
        stream->lpVtbl->Release(stream);
        return NULL;
    }
    *out_stream = stream;
    return decoder;
}

ChatImageIngestResult image_ingest_file(const wchar_t *path,
    ChatImageIngest *out) {
    ChatImageIngestResult result = IMAGE_INGEST_IO;
    unsigned char *source_bytes = NULL;
    size_t source_length = 0;
    IWICImagingFactory *factory = NULL;
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    ChatImageFacts facts;
    ChatImagePlan plan;
    uint8_t flags = 0;
    UINT frame_count = 0, width = 0, height = 0;
    ChatImageFormat format;
    unsigned orientation = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!path || !out) return IMAGE_INGEST_IO;
    if (!read_source_file(path, &source_bytes, &source_length, &result))
        return result;
    if (!create_factory(&factory)) { result = IMAGE_INGEST_OOM; goto cleanup; }
    decoder = decoder_from_memory(factory, source_bytes, source_length,
        &stream);
    if (!decoder) { result = IMAGE_INGEST_UNSUPPORTED; goto cleanup; }
    if (FAILED(decoder->lpVtbl->GetFrameCount(decoder, &frame_count)) ||
            frame_count == 0 ||
            FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) ||
            FAILED(frame->lpVtbl->GetSize(frame, &width, &height))) {
        result = IMAGE_INGEST_UNSUPPORTED;
        goto cleanup;
    }
    format = container_format(decoder);
    if (format == CHAT_IMAGE_FORMAT_JPEG)
        orientation = read_orientation(frame);
    memset(&facts, 0, sizeof facts);
    facts.byte_length = source_length;
    facts.format = format;
    facts.width = width;
    facts.height = height;
    facts.frame_count = frame_count;
    facts.exif_orientation = orientation;
    facts.from_clipboard = false;
    plan = chat_image_plan(&facts, &flags);
    if (plan == CHAT_IMAGE_REJECT_TOO_LARGE) {
        result = IMAGE_INGEST_TOO_LARGE;
        goto cleanup;
    }
    if (plan == CHAT_IMAGE_REJECT_DIMENSIONS) {
        result = IMAGE_INGEST_DIMENSIONS;
        goto cleanup;
    }
    if (plan == CHAT_IMAGE_STORE_AS_IS) {
        /* Passthrough requires both container integrity (WIC is lenient about
           damaged payloads) and a successful full pixel decode. */
        if (!chat_image_bytes_intact(source_bytes, source_length, format) ||
                !pixels_decode(factory, (IWICBitmapSource *)frame)) {
            result = IMAGE_INGEST_UNSUPPORTED;
            goto cleanup;
        }
        out->bytes = source_bytes; /* ownership transfers to the caller */
        source_bytes = NULL;
        out->length = source_length;
        out->meta.pixel_width = width;
        out->meta.pixel_height = height;
        copy_mime(out->meta.mime, sizeof out->meta.mime, format);
        out->flags = flags;
        set_display_name(out->meta.display_name,
            sizeof out->meta.display_name / sizeof out->meta.display_name[0],
            path);
        result = IMAGE_INGEST_OK;
        goto cleanup;
    }
    result = transform_and_encode(factory, (IWICBitmapSource *)frame,
        orientation, plan == CHAT_IMAGE_NORMALIZE_JPEG, out);
    if (result == IMAGE_INGEST_OK) {
        out->flags = flags;
        set_display_name(out->meta.display_name,
            sizeof out->meta.display_name / sizeof out->meta.display_name[0],
            path);
    } else {
        free(out->bytes);
        memset(out, 0, sizeof *out);
    }

cleanup:
    free(source_bytes);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    if (factory) factory->lpVtbl->Release(factory);
    return result;
}

/* ---- packed clipboard DIB ---- */

ChatImageIngestResult image_ingest_dib(const void *dib, size_t dib_size,
    ChatImageIngest *out) {
    ChatImageIngestResult result = IMAGE_INGEST_UNSUPPORTED;
    const BITMAPINFOHEADER *header;
    const unsigned char *bits;
    uint32_t width, height;
    bool top_down;
    size_t offset, stride, needed, mask_bytes;
    WICPixelFormatGUID pixel_format;
    IWICImagingFactory *factory = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapFlipRotator *flip = NULL;
    IWICBitmapSource *source = NULL;

    if (out) memset(out, 0, sizeof *out);
    if (!dib || !out || dib_size < sizeof(BITMAPINFOHEADER))
        return IMAGE_INGEST_UNSUPPORTED;
    header = (const BITMAPINFOHEADER *)dib;
    if (header->biSize < sizeof(BITMAPINFOHEADER) ||
            (size_t)header->biSize > dib_size || header->biPlanes != 1 ||
            header->biWidth <= 0 || header->biHeight == 0)
        return IMAGE_INGEST_UNSUPPORTED;
    width = (uint32_t)header->biWidth;
    {
        /* Sign-extend before negating: INT32_MIN would overflow a LONG. */
        int64_t magnitude = header->biHeight;
        top_down = magnitude < 0;
        if (magnitude < 0) magnitude = -magnitude;
        /* Caps before any allocation: CreateBitmapFromMemory would otherwise
           honor an attacker-sized header. */
        if (width > CHAT_ATTACHMENT_MAX_PIXELS ||
                magnitude > (int64_t)CHAT_ATTACHMENT_MAX_PIXELS)
            return IMAGE_INGEST_DIMENSIONS;
        height = (uint32_t)magnitude;
    }
    if (header->biBitCount == 24 && header->biCompression == BI_RGB) {
        pixel_format = GUID_WICPixelFormat24bppBGR;
        stride = (((size_t)width * 24u + 31u) / 32u) * 4u;
        offset = (size_t)header->biSize;
        mask_bytes = 0;
    } else if (header->biBitCount == 32 &&
            (header->biCompression == BI_RGB ||
             header->biCompression == BI_BITFIELDS)) {
        bool has_alpha = false;
        stride = (size_t)width * 4u;
        mask_bytes = 0;
        if (header->biCompression == BI_BITFIELDS) {
            /* Channel order is declared by masks, never assumed: a 40-byte
               header carries three masks immediately after it, while a V4/V5
               header carries them inside biSize. Only the standard 32bpp BGRA
               layout is accepted; any other masks would be misread. */
            DWORD red, green, blue, alpha = 0;
            if (header->biSize == sizeof(BITMAPINFOHEADER)) {
                DWORD masks[3];
                if (dib_size < sizeof(BITMAPINFOHEADER) + sizeof masks)
                    return IMAGE_INGEST_UNSUPPORTED;
                memcpy(masks, (const unsigned char *)dib +
                    sizeof(BITMAPINFOHEADER), sizeof masks);
                red = masks[0]; green = masks[1]; blue = masks[2];
                mask_bytes = sizeof masks;
            } else if (header->biSize >= sizeof(BITMAPV4HEADER) &&
                    dib_size >= sizeof(BITMAPV4HEADER)) {
                const BITMAPV4HEADER *v4 = (const BITMAPV4HEADER *)dib;
                red = v4->bV4RedMask;
                green = v4->bV4GreenMask;
                blue = v4->bV4BlueMask;
                alpha = v4->bV4AlphaMask;
            } else {
                return IMAGE_INGEST_UNSUPPORTED;
            }
            if (red != 0x00ff0000u || green != 0x0000ff00u ||
                    blue != 0x000000ffu ||
                    (alpha != 0u && alpha != 0xff000000u))
                return IMAGE_INGEST_UNSUPPORTED;
            has_alpha = alpha != 0u;
        }
        /* Preserve transparency only when the source declares an alpha mask;
           a BI_RGB fourth byte is undefined padding. */
        pixel_format = has_alpha ? GUID_WICPixelFormat32bppBGRA
                                 : GUID_WICPixelFormat32bppBGR;
        offset = (size_t)header->biSize + mask_bytes;
    } else {
        return IMAGE_INGEST_UNSUPPORTED;
    }
    if (offset > dib_size) return IMAGE_INGEST_UNSUPPORTED;
    needed = offset + stride * height;
    if (needed > dib_size) return IMAGE_INGEST_UNSUPPORTED;
    bits = (const unsigned char *)dib + offset;

    if (!create_factory(&factory)) return IMAGE_INGEST_OOM;
    if (FAILED(factory->lpVtbl->CreateBitmapFromMemory(factory, width, height,
            &pixel_format, (UINT)stride, (UINT)(stride * height),
            (BYTE *)bits, &bitmap))) {
        result = IMAGE_INGEST_UNSUPPORTED;
        goto cleanup;
    }
    source = (IWICBitmapSource *)bitmap;
    if (!top_down) {
        /* A bottom-up DIB must be flipped; a setup failure fails closed. */
        if (FAILED(factory->lpVtbl->CreateBitmapFlipRotator(factory, &flip)) ||
                FAILED(flip->lpVtbl->Initialize(flip, source,
                    WICBitmapTransformFlipVertical))) {
            result = IMAGE_INGEST_OOM;
            goto cleanup;
        }
        source = (IWICBitmapSource *)flip;
    }
    result = transform_and_encode(factory, source, 0, false, out);
    if (result != IMAGE_INGEST_OK) {
        free(out->bytes);
        memset(out, 0, sizeof *out);
    }

cleanup:
    if (flip) flip->lpVtbl->Release(flip);
    if (bitmap) bitmap->lpVtbl->Release(bitmap);
    if (factory) factory->lpVtbl->Release(factory);
    return result;
}

/* ---- lifecycle and messages ---- */

void image_ingest_dispose(ChatImageIngest *out) {
    if (!out) return;
    free(out->bytes);
    memset(out, 0, sizeof *out);
}

const wchar_t *image_ingest_error_text(ChatImageIngestResult result) {
    switch (result) {
    case IMAGE_INGEST_OK: return L"";
    case IMAGE_INGEST_IO: return L"The image could not be read.";
    case IMAGE_INGEST_TOO_LARGE: return L"Image is larger than 20 MB";
    case IMAGE_INGEST_DIMENSIONS: return L"Image dimensions are too large";
    case IMAGE_INGEST_UNSUPPORTED:
        return L"That file is not a supported image.";
    case IMAGE_INGEST_OOM: return L"Not enough memory to process the image.";
    case IMAGE_INGEST_EMPTY: return L"The image is empty.";
    default: return L"The image could not be read.";
    }
}
