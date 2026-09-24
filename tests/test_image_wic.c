/* Pure image-policy table plus headless Windows Imaging Component ingest.

   The WIC layer is exercised only for formats WIC can generate reliably on
   any supported Windows (PNG/JPEG/BMP/GIF); EXIF orientation and animation
   policy are covered through the pure decision table, not by crafting
   oriented or animated bytes at runtime. Built and run by `chat.bat test`. */
#include "chat/core/image_policy.h"
#include "chat/shell/image_wic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define REL(p) do { if (p) { ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* malloc wrap: counts live allocations so failure paths can be asserted
   leak-free. WIC allocates from its own heap, so only our buffers move. */
void *__real_malloc(size_t size);
void __real_free(void *pointer);
static long fail_mallocs;
static long live_allocs;
void *__wrap_malloc(size_t size) {
    if (fail_mallocs > 0) { --fail_mallocs; return NULL; }
    {
        void *result = __real_malloc(size);
        if (result) ++live_allocs;
        return result;
    }
}
void __wrap_free(void *pointer) {
    if (pointer) --live_allocs;
    __real_free(pointer);
}

/* ---- pure decision table ---- */

static int policy_checks(void) {
    ChatImageFacts facts;
    uint8_t flags = 0xAA;
    memset(&facts, 0, sizeof facts);
    facts.byte_length = CHAT_ATTACHMENT_MAX_BYTES;
    facts.format = CHAT_IMAGE_FORMAT_PNG;
    facts.width = 10; facts.height = 10; facts.frame_count = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);
    CHECK(flags == 0);

    facts.byte_length = (size_t)CHAT_ATTACHMENT_MAX_BYTES + 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_REJECT_TOO_LARGE);
    CHECK(flags == 0);
    /* Byte rejection outranks dimension rejection. */
    facts.width = CHAT_ATTACHMENT_MAX_PIXELS + 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_REJECT_TOO_LARGE);

    facts.byte_length = 10;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_REJECT_DIMENSIONS);
    facts.width = CHAT_ATTACHMENT_MAX_PIXELS;
    facts.height = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);
    facts.height = CHAT_ATTACHMENT_MAX_PIXELS + 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_REJECT_DIMENSIONS);

    /* JPEG: orientation 0/1 passthrough, otherwise normalize. */
    facts.width = 10; facts.height = 10;
    facts.format = CHAT_IMAGE_FORMAT_JPEG;
    facts.exif_orientation = 0;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);
    facts.exif_orientation = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);
    facts.exif_orientation = 6;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_JPEG);

    /* Multi-frame: flag set, normalize to PNG for every non-JPEG format. */
    facts.format = CHAT_IMAGE_FORMAT_GIF;
    facts.exif_orientation = 0;
    facts.frame_count = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);
    CHECK(flags == 0);
    facts.frame_count = 3;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_PNG);
    CHECK(flags == CHAT_PART_FLAG_FIRST_FRAME);

    facts.format = CHAT_IMAGE_FORMAT_WEBP;
    facts.frame_count = 2;
    flags = 0;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_PNG);
    CHECK(flags == CHAT_PART_FLAG_FIRST_FRAME);
    facts.frame_count = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_STORE_AS_IS);

    facts.format = CHAT_IMAGE_FORMAT_PNG;
    facts.frame_count = 2;
    flags = 0;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_PNG);
    CHECK(flags == CHAT_PART_FLAG_FIRST_FRAME);

    /* Decodable but not API-ready formats normalize to PNG. */
    facts.format = CHAT_IMAGE_FORMAT_OTHER;
    facts.frame_count = 1;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_PNG);

    /* Clipboard always normalizes, even for a passthrough-capable source. */
    facts.format = CHAT_IMAGE_FORMAT_PNG;
    facts.from_clipboard = true;
    CHECK(chat_image_plan(&facts, &flags) == CHAT_IMAGE_NORMALIZE_PNG);

    flags = 0xAA;
    CHECK(chat_image_plan(NULL, &flags) == CHAT_IMAGE_REJECT_DIMENSIONS);
    CHECK(flags == 0);

    /* Passthrough container integrity: minimal well-formed/truncated buffers. */
    {
        const unsigned char jpeg_ok[] = { 0xff, 0xd8, 0x00, 0x00, 0xff, 0xd9 };
        const unsigned char jpeg_bad[] = { 0xff, 0xd8, 0x00, 0x00, 0x00, 0x00 };
        unsigned char gif_ok[] = { 'G', 'I', 'F', '8', '9', 'a', 0x00, 0x3b };
        unsigned char gif_bad[] = { 'G', 'I', 'F', '8', '9', 'a', 0x00, 0x00 };
        unsigned char webp_ok[16];
        unsigned char webp_bad[16];
        const unsigned char png_bad[] = { 0x89, 'P', 'N', 'G' };
        memset(webp_ok, 0, sizeof webp_ok);
        memcpy(webp_ok, "RIFF", 4);
        memcpy(webp_ok + 8, "WEBP", 4);
        webp_ok[4] = (unsigned char)(sizeof webp_ok - 8); /* RIFF size, LE */
        memcpy(webp_bad, webp_ok, sizeof webp_ok);
        webp_bad[4] = 7;
        CHECK(chat_image_bytes_intact(jpeg_ok, sizeof jpeg_ok,
            CHAT_IMAGE_FORMAT_JPEG));
        CHECK(!chat_image_bytes_intact(jpeg_bad, sizeof jpeg_bad,
            CHAT_IMAGE_FORMAT_JPEG));
        CHECK(chat_image_bytes_intact(gif_ok, sizeof gif_ok,
            CHAT_IMAGE_FORMAT_GIF));
        CHECK(!chat_image_bytes_intact(gif_bad, sizeof gif_bad,
            CHAT_IMAGE_FORMAT_GIF));
        CHECK(chat_image_bytes_intact(webp_ok, sizeof webp_ok,
            CHAT_IMAGE_FORMAT_WEBP));
        CHECK(!chat_image_bytes_intact(webp_bad, sizeof webp_bad,
            CHAT_IMAGE_FORMAT_WEBP));
        CHECK(!chat_image_bytes_intact(png_bad, sizeof png_bad,
            CHAT_IMAGE_FORMAT_PNG));
        CHECK(!chat_image_bytes_intact(NULL, 0, CHAT_IMAGE_FORMAT_JPEG));
        CHECK(chat_image_bytes_intact(jpeg_ok, sizeof jpeg_ok,
            CHAT_IMAGE_FORMAT_OTHER));
    }
    return 0;
}

/* ---- WIC ingest ---- */

/* Decodes an in-memory image and returns its top-left pixel as BGRA, so a
   normalized output can be checked for preserved transparency. */
static bool first_pixel_bgra(IWICImagingFactory *factory,
    const unsigned char *data, size_t n, BYTE out[4]) {
    HGLOBAL memory;
    void *locked;
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    WICRect rect = { 0, 0, 1, 1 };
    bool ok = false;
    memory = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!memory) return false;
    locked = GlobalLock(memory);
    if (!locked) { GlobalFree(memory); return false; }
    memcpy(locked, data, n);
    GlobalUnlock(memory);
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return false;
    }
    if (FAILED(factory->lpVtbl->CreateDecoderFromStream(factory, stream, NULL,
            WICDecodeMetadataCacheOnDemand, &decoder)))
        goto done;
    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) goto done;
    if (FAILED(factory->lpVtbl->CreateFormatConverter(factory, &converter)))
        goto done;
    if (FAILED(converter->lpVtbl->Initialize(converter,
            (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
        goto done;
    if (SUCCEEDED(converter->lpVtbl->CopyPixels(converter, &rect, 4, 4, out)))
        ok = true;
done:
    if (converter) converter->lpVtbl->Release(converter);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    return ok;
}

static bool make_image(IWICImagingFactory *factory, const wchar_t *path,
    const GUID *container, UINT width, UINT height, const BYTE *bgra) {
    IWICBitmap *bitmap = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    bool ok = false;
    if (FAILED(factory->lpVtbl->CreateBitmapFromMemory(factory, width, height,
            &GUID_WICPixelFormat32bppBGRA, width * 4, width * height * 4,
            (BYTE *)bgra, &bitmap)))
        goto done;
    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream))) goto done;
    if (FAILED(stream->lpVtbl->InitializeFromFilename(stream, path,
            GENERIC_WRITE)))
        goto done;
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, container, NULL,
            &encoder)))
        goto done;
    if (FAILED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream,
            WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options)))
        goto done;
    if (FAILED(frame->lpVtbl->Initialize(frame, options))) goto done;
    if (FAILED(frame->lpVtbl->SetSize(frame, width, height))) goto done;
    if (FAILED(frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)bitmap,
            NULL)))
        goto done;
    if (FAILED(frame->lpVtbl->Commit(frame))) goto done;
    if (FAILED(encoder->lpVtbl->Commit(encoder))) goto done;
    ok = true;
done:
    REL(options); REL(frame); REL(encoder); REL(stream); REL(bitmap);
    return ok;
}

static bool write_bytes(const wchar_t *path, const void *data, size_t n) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool ok;
    if (file == INVALID_HANDLE_VALUE) return false;
    ok = WriteFile(file, data, (DWORD)n, &written, NULL) && written == n;
    CloseHandle(file);
    return ok;
}

/* Copies `source`, flipping the first byte of the first IDAT chunk data so
   the pixel payload is damaged while the PNG header/metadata stay valid. */
static bool corrupt_png_idat(const wchar_t *source, const wchar_t *dest) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *buffer;
    DWORD got = 0;
    size_t total, pos;
    bool ok = false;
    file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }
    total = (size_t)size.QuadPart;
    buffer = (unsigned char *)malloc(total);
    if (!buffer) { CloseHandle(file); return false; }
    if (!ReadFile(file, buffer, (DWORD)total, &got, NULL) || got != total) {
        free(buffer);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    pos = 8; /* PNG signature */
    while (pos + 12 <= total) {
        DWORD length = ((DWORD)buffer[pos] << 24) | ((DWORD)buffer[pos + 1] << 16) |
            ((DWORD)buffer[pos + 2] << 8) | (DWORD)buffer[pos + 3];
        if ((size_t)length > total - pos - 12) break;
        if (!memcmp(buffer + pos + 4, "IDAT", 4) && length > 0) {
            buffer[pos + 8] ^= 0xFF;
            ok = write_bytes(dest, buffer, total);
            break;
        }
        pos += 12 + (size_t)length;
    }
    free(buffer);
    return ok;
}

static bool copy_prefix(const wchar_t *source, const wchar_t *dest, size_t n) {
    HANDLE file;
    BYTE buffer[64];
    DWORD got = 0;
    if (n > sizeof buffer) return false;
    file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!ReadFile(file, buffer, (DWORD)n, &got, NULL) || got != n) {
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return write_bytes(dest, buffer, n);
}

static int wic_checks(IWICImagingFactory *factory, const wchar_t *dir) {
    BYTE pixels[4 * 4 * 4];
    wchar_t path_png[512], path_jpg[512], path_bmp[512], path_gif[512];
    wchar_t path_trunc[512], path_lying[512], path_corrupt[512];
    ChatImageIngest ingest;
    size_t i;

    for (i = 0; i < sizeof pixels; i++) pixels[i] = (BYTE)(i * 5 + 1);
    swprintf(path_png, 512, L"%ls\\tiny.png", dir);
    swprintf(path_jpg, 512, L"%ls\\tiny.jpg", dir);
    swprintf(path_bmp, 512, L"%ls\\tiny.bmp", dir);
    swprintf(path_gif, 512, L"%ls\\tiny.gif", dir);
    swprintf(path_trunc, 512, L"%ls\\truncated.png", dir);
    swprintf(path_lying, 512, L"%ls\\notreally.png", dir);
    swprintf(path_corrupt, 512, L"%ls\\corrupt.png", dir);

    CHECK(make_image(factory, path_png, &GUID_ContainerFormatPng, 4, 4, pixels));
    CHECK(make_image(factory, path_jpg, &GUID_ContainerFormatJpeg, 4, 4, pixels));
    CHECK(make_image(factory, path_bmp, &GUID_ContainerFormatBmp, 4, 4, pixels));
    CHECK(make_image(factory, path_gif, &GUID_ContainerFormatGif, 4, 4, pixels));

    /* PNG passthrough: source bytes stored unchanged, PNG mime. */
    CHECK(image_ingest_file(path_png, &ingest) == IMAGE_INGEST_OK);
    CHECK(!strcmp(ingest.meta.mime, "image/png"));
    CHECK(ingest.meta.pixel_width == 4 && ingest.meta.pixel_height == 4);
    CHECK(ingest.flags == 0 && ingest.length > 8);
    CHECK(ingest.bytes && ingest.bytes[0] == 0x89 && ingest.bytes[1] == 'P');
    CHECK(!wcscmp(ingest.meta.display_name, L"tiny.png"));
    CHECK(ingest.meta.attachment_id == 0);
    image_ingest_dispose(&ingest);
    CHECK(ingest.bytes == NULL && ingest.length == 0);

    /* JPEG passthrough. */
    CHECK(image_ingest_file(path_jpg, &ingest) == IMAGE_INGEST_OK);
    CHECK(!strcmp(ingest.meta.mime, "image/jpeg"));
    CHECK(ingest.meta.pixel_width == 4 && ingest.meta.pixel_height == 4);
    image_ingest_dispose(&ingest);

    /* BMP is not API-ready: normalize to PNG at the same size (<= 2048). */
    CHECK(image_ingest_file(path_bmp, &ingest) == IMAGE_INGEST_OK);
    CHECK(!strcmp(ingest.meta.mime, "image/png"));
    CHECK(ingest.bytes && ingest.bytes[0] == 0x89 && ingest.bytes[1] == 'P');
    CHECK(ingest.meta.pixel_width == 4 && ingest.meta.pixel_height == 4);
    image_ingest_dispose(&ingest);

    /* Single-frame GIF passthrough (codec-tolerant: skip if no GIF support). */
    {
        ChatImageIngestResult r = image_ingest_file(path_gif, &ingest);
        if (r == IMAGE_INGEST_OK) {
            CHECK(!strcmp(ingest.meta.mime, "image/gif"));
            image_ingest_dispose(&ingest);
        } else {
            CHECK(r == IMAGE_INGEST_UNSUPPORTED);
        }
    }

    /* Truncated and mislabeled files are unsupported, never partial. */
    CHECK(copy_prefix(path_png, path_trunc, 20));
    CHECK(image_ingest_file(path_trunc, &ingest) == IMAGE_INGEST_UNSUPPORTED);
    CHECK(ingest.bytes == NULL && ingest.length == 0);
    CHECK(write_bytes(path_lying, "not an image at all", 19));
    CHECK(image_ingest_file(path_lying, &ingest) == IMAGE_INGEST_UNSUPPORTED);
    CHECK(ingest.bytes == NULL && ingest.length == 0);

    /* A PNG whose header parses but whose pixel payload is damaged must not
       be accepted as a passthrough (frame metadata alone is not validation). */
    CHECK(corrupt_png_idat(path_png, path_corrupt));
    CHECK(image_ingest_file(path_corrupt, &ingest) == IMAGE_INGEST_UNSUPPORTED);
    CHECK(ingest.bytes == NULL && ingest.length == 0);
    DeleteFileW(path_corrupt);

    /* Empty file. */
    CHECK(write_bytes(path_lying, "", 0));
    CHECK(image_ingest_file(path_lying, &ingest) == IMAGE_INGEST_EMPTY);

    /* A pathless call is a clean IO failure, not a crash. */
    CHECK(image_ingest_file(NULL, &ingest) == IMAGE_INGEST_IO);
    CHECK(image_ingest_file(L"build\\image-test-missing-file.png", &ingest)
        == IMAGE_INGEST_IO);

    /* The byte cap runs before decode: an over-cap file whose bytes are not
       an image reports TOO_LARGE, not UNSUPPORTED. */
    {
        size_t big_n = (size_t)CHAT_ATTACHMENT_MAX_BYTES + 1;
        unsigned char *big = (unsigned char *)malloc(big_n);
        wchar_t path_big[512];
        CHECK(big != NULL);
        memset(big, 'x', big_n);
        swprintf(path_big, 512, L"%ls\\over-cap.bin", dir);
        CHECK(write_bytes(path_big, big, big_n));
        free(big);
        CHECK(image_ingest_file(path_big, &ingest) == IMAGE_INGEST_TOO_LARGE);
        CHECK(ingest.bytes == NULL && ingest.length == 0);
        DeleteFileW(path_big);
    }

    /* Synthetic CF_DIB: 4x2 top-down 32bpp BI_RGB always normalizes to PNG. */
    {
        struct {
            BITMAPINFOHEADER header;
            BYTE bits[4 * 2 * 4];
        } dib;
        memset(&dib, 0, sizeof dib);
        dib.header.biSize = sizeof(BITMAPINFOHEADER);
        dib.header.biWidth = 4;
        dib.header.biHeight = -2; /* top-down */
        dib.header.biPlanes = 1;
        dib.header.biBitCount = 32;
        dib.header.biCompression = BI_RGB;
        for (i = 0; i < sizeof dib.bits; i++)
            dib.bits[i] = (BYTE)(i * 11 + 3);
        CHECK(image_ingest_dib(&dib, sizeof dib, &ingest) == IMAGE_INGEST_OK);
        CHECK(!strcmp(ingest.meta.mime, "image/png"));
        CHECK(ingest.meta.pixel_width == 4 && ingest.meta.pixel_height == 2);
        CHECK(ingest.flags == 0 && ingest.bytes && ingest.length > 8);
        CHECK(ingest.bytes[0] == 0x89 && ingest.bytes[1] == 'P');
        CHECK(ingest.meta.display_name[0] == 0);
        image_ingest_dispose(&ingest);

        /* Bottom-up DIB still produces the same normalized size. */
        dib.header.biHeight = 2;
        CHECK(image_ingest_dib(&dib, sizeof dib, &ingest) == IMAGE_INGEST_OK);
        CHECK(ingest.meta.pixel_width == 4 && ingest.meta.pixel_height == 2);
        image_ingest_dispose(&ingest);

        /* Undersized buffers and unsupported layouts are rejected. */
        CHECK(image_ingest_dib(&dib, sizeof(BITMAPINFOHEADER) - 1, &ingest)
            == IMAGE_INGEST_UNSUPPORTED);
        dib.header.biBitCount = 4;
        CHECK(image_ingest_dib(&dib, sizeof dib, &ingest)
            == IMAGE_INGEST_UNSUPPORTED);
    }

    /* BI_BITFIELDS: the color masks after a 40-byte header must be skipped,
       not read as pixels. Standard masks over identical pixels must produce
       the same PNG as the BI_RGB form; non-standard masks are rejected. */
    {
        ChatImageIngest rgb, bitfields;
        unsigned char pixel_bytes[4 * 2 * 4];
        struct {
            BITMAPINFOHEADER header;
            BYTE bits[4 * 2 * 4];
        } rgb_dib;
        struct {
            BITMAPINFOHEADER header;
            DWORD masks[3];
            BYTE bits[4 * 2 * 4];
        } bf_dib;
        for (i = 0; i < sizeof pixel_bytes; i++)
            pixel_bytes[i] = (BYTE)(i * 9 + 2);

        memset(&rgb_dib, 0, sizeof rgb_dib);
        rgb_dib.header.biSize = sizeof(BITMAPINFOHEADER);
        rgb_dib.header.biWidth = 4;
        rgb_dib.header.biHeight = -2;
        rgb_dib.header.biPlanes = 1;
        rgb_dib.header.biBitCount = 32;
        rgb_dib.header.biCompression = BI_RGB;
        memcpy(rgb_dib.bits, pixel_bytes, sizeof pixel_bytes);

        memset(&bf_dib, 0, sizeof bf_dib);
        bf_dib.header.biSize = sizeof(BITMAPINFOHEADER);
        bf_dib.header.biWidth = 4;
        bf_dib.header.biHeight = -2;
        bf_dib.header.biPlanes = 1;
        bf_dib.header.biBitCount = 32;
        bf_dib.header.biCompression = BI_BITFIELDS;
        bf_dib.masks[0] = 0x00ff0000u;
        bf_dib.masks[1] = 0x0000ff00u;
        bf_dib.masks[2] = 0x000000ffu;
        memcpy(bf_dib.bits, pixel_bytes, sizeof pixel_bytes);

        CHECK(image_ingest_dib(&rgb_dib, sizeof rgb_dib, &rgb)
            == IMAGE_INGEST_OK);
        CHECK(image_ingest_dib(&bf_dib, sizeof bf_dib, &bitfields)
            == IMAGE_INGEST_OK);
        CHECK(rgb.length > 0 && rgb.length == bitfields.length);
        CHECK(!memcmp(rgb.bytes, bitfields.bytes, rgb.length));
        image_ingest_dispose(&rgb);
        image_ingest_dispose(&bitfields);

        bf_dib.masks[1] = 0x00ff0000u; /* non-standard channel order */
        CHECK(image_ingest_dib(&bf_dib, sizeof bf_dib, &ingest)
            == IMAGE_INGEST_UNSUPPORTED);
        CHECK(ingest.bytes == NULL && ingest.length == 0);

        /* A BI_BITFIELDS header that ends before its mask block must not be
           read past the buffer. */
        {
            BITMAPINFOHEADER short_header;
            memset(&short_header, 0, sizeof short_header);
            short_header.biSize = sizeof(BITMAPINFOHEADER);
            short_header.biWidth = 4;
            short_header.biHeight = -2;
            short_header.biPlanes = 1;
            short_header.biBitCount = 32;
            short_header.biCompression = BI_BITFIELDS;
            CHECK(image_ingest_dib(&short_header, sizeof short_header, &ingest)
                == IMAGE_INGEST_UNSUPPORTED);
        }

        /* biHeight == INT32_MIN must not overflow the magnitude computation;
           its magnitude is far over the pixel cap, so it is rejected. */
        {
            BITMAPINFOHEADER extreme;
            memset(&extreme, 0, sizeof extreme);
            extreme.biSize = sizeof(BITMAPINFOHEADER);
            extreme.biWidth = 4;
            extreme.biHeight = INT32_MIN;
            extreme.biPlanes = 1;
            extreme.biBitCount = 32;
            extreme.biCompression = BI_RGB;
            CHECK(image_ingest_dib(&extreme, sizeof extreme, &ingest)
                == IMAGE_INGEST_DIMENSIONS);
        }
    }

    /* A bottom-up DIB is flipped so it matches the equivalent top-down DIB;
       a silently skipped flip would encode different bytes. */
    {
        ChatImageIngest down, up;
        unsigned char top_row[4 * 4], bottom_row[4 * 4];
        struct {
            BITMAPINFOHEADER header;
            BYTE bits[4 * 2 * 4];
        } top_down_dib, bottom_up_dib;
        for (i = 0; i < sizeof top_row; i++) {
            top_row[i] = (BYTE)(0x10 + i);
            bottom_row[i] = (BYTE)(0x80 + i);
        }
        memset(&top_down_dib, 0, sizeof top_down_dib);
        top_down_dib.header.biSize = sizeof(BITMAPINFOHEADER);
        top_down_dib.header.biWidth = 4;
        top_down_dib.header.biHeight = -2;
        top_down_dib.header.biPlanes = 1;
        top_down_dib.header.biBitCount = 32;
        top_down_dib.header.biCompression = BI_RGB;
        memcpy(top_down_dib.bits, top_row, sizeof top_row);
        memcpy(top_down_dib.bits + sizeof top_row, bottom_row,
            sizeof bottom_row);
        memset(&bottom_up_dib, 0, sizeof bottom_up_dib);
        bottom_up_dib.header.biSize = sizeof(BITMAPINFOHEADER);
        bottom_up_dib.header.biWidth = 4;
        bottom_up_dib.header.biHeight = 2;
        bottom_up_dib.header.biPlanes = 1;
        bottom_up_dib.header.biBitCount = 32;
        bottom_up_dib.header.biCompression = BI_RGB;
        memcpy(bottom_up_dib.bits, bottom_row, sizeof bottom_row);
        memcpy(bottom_up_dib.bits + sizeof bottom_row, top_row,
            sizeof top_row);
        CHECK(image_ingest_dib(&top_down_dib, sizeof top_down_dib, &down)
            == IMAGE_INGEST_OK);
        CHECK(image_ingest_dib(&bottom_up_dib, sizeof bottom_up_dib, &up)
            == IMAGE_INGEST_OK);
        CHECK(down.length > 0 && down.length == up.length);
        CHECK(!memcmp(down.bytes, up.bytes, down.length));
        image_ingest_dispose(&down);
        image_ingest_dispose(&up);
    }

    /* V4/V5 BI_BITFIELDS carries its masks inside the header; the pixel
       offset is biSize, not biSize + 12, and an alpha mask preserves
       transparency through normalization. */
    {
        ChatImageIngest v5_ingest;
        BYTE pixel[4];
        struct {
            BITMAPV5HEADER header;
            BYTE bits[4 * 4];
        } v5;
        memset(&v5, 0, sizeof v5);
        v5.header.bV5Size = sizeof(BITMAPV5HEADER);
        v5.header.bV5Width = 4;
        v5.header.bV5Height = -1;
        v5.header.bV5Planes = 1;
        v5.header.bV5BitCount = 32;
        v5.header.bV5Compression = BI_BITFIELDS;
        v5.header.bV5RedMask = 0x00ff0000u;
        v5.header.bV5GreenMask = 0x0000ff00u;
        v5.header.bV5BlueMask = 0x000000ffu;
        v5.header.bV5AlphaMask = 0xff000000u;
        /* Four pixels, BGRA memory order; the first is fully transparent. */
        v5.bits[0] = 0x11; v5.bits[1] = 0x22; v5.bits[2] = 0x33; v5.bits[3] = 0x00;
        v5.bits[4] = 0x44; v5.bits[5] = 0x55; v5.bits[6] = 0x66; v5.bits[7] = 0xFF;
        v5.bits[8] = 0x77; v5.bits[9] = 0x88; v5.bits[10] = 0x99; v5.bits[11] = 0xFF;
        v5.bits[12] = 0xAA; v5.bits[13] = 0xBB; v5.bits[14] = 0xCC; v5.bits[15] = 0xFF;
        CHECK(image_ingest_dib(&v5, sizeof v5, &v5_ingest) == IMAGE_INGEST_OK);
        CHECK(!strcmp(v5_ingest.meta.mime, "image/png") &&
            v5_ingest.meta.pixel_width == 4 &&
            v5_ingest.meta.pixel_height == 1);
        CHECK(first_pixel_bgra(factory, v5_ingest.bytes, v5_ingest.length,
            pixel));
        CHECK(pixel[3] == 0x00); /* transparency survived normalization */
        image_ingest_dispose(&v5_ingest);
    }
    return 0;
}

static bool remove_dir_tree(const wchar_t *dir) {
    wchar_t pattern[512];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    swprintf(pattern, 512, L"%ls\\*", dir);
    find = FindFirstFileW(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[512];
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
                continue;
            swprintf(full, 512, L"%ls\\%ls", dir, entry.cFileName);
            DeleteFileW(full);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    return RemoveDirectoryW(dir) != 0;
}

int main(void) {
    wchar_t dir[512];
    IWICImagingFactory *factory = NULL;
    long before;

    if (policy_checks()) return 1;
    CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)));
    CHECK(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory)));
    swprintf(dir, 512, L"build\\image-test-%lu",
        (unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(wic_checks(factory, dir) == 0);

    /* An allocation failure on the source read is a clean OOM, leak-free. */
    {
        wchar_t path[512];
        ChatImageIngest ingest;
        swprintf(path, 512, L"%ls\\tiny.png", dir);
        before = live_allocs;
        fail_mallocs = 1;
        CHECK(image_ingest_file(path, &ingest) == IMAGE_INGEST_OOM);
        CHECK(ingest.bytes == NULL && ingest.length == 0);
        CHECK(live_allocs == before);
    }
    /* A successful ingest plus dispose balances our allocations. */
    {
        wchar_t path[512];
        ChatImageIngest ingest;
        swprintf(path, 512, L"%ls\\tiny.png", dir);
        before = live_allocs;
        CHECK(image_ingest_file(path, &ingest) == IMAGE_INGEST_OK);
        image_ingest_dispose(&ingest);
        CHECK(live_allocs == before);
    }

    REL(factory);
    CoUninitialize();
    CHECK(remove_dir_tree(dir));
    puts("Image policy table and WIC ingest checks passed");
    return 0;
}
