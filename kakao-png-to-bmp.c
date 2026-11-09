#include "kakao-png-to-bmp.h"
#include <png.h>
#include <stdint.h>
#include <string.h>

#define MAX_DIMENSION 16384U
#define MAX_PIXELS 64000000U
#define MAX_DECODED_BYTES (256U * 1024U * 1024U)

struct image {
    const unsigned char *input;
    size_t remaining;
    GCancellable *cancel;
    png_bytep bmp;
    png_bytepp rows;
    png_uint_32 width, height;
    int channels, depth;
    size_t stride;
    char *error;
};

static void read_bytes(png_structp png, png_bytep dest, png_size_t size)
{
    struct image *image = png_get_io_ptr(png);
    if (g_cancellable_is_cancelled(image->cancel) || size > image->remaining)
        png_error(png, "PNG input ended or was cancelled");
    memcpy(dest, image->input, size);
    image->input += size;
    image->remaining -= size;
}

static void decode_error(png_structp png, png_const_charp message)
{
    struct image *image = png_get_error_ptr(png);
    image->error = g_strdup(message);
    png_longjmp(png, 1);
}

static void ignore_warning(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

static int read_png(struct image *image)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, image,
                                             decode_error, ignore_warning);
    if (!png)
        return 0;
    png_infop info = png_create_info_struct(png);
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }

    png_set_read_fn(png, image, read_bytes);
    png_set_user_limits(png, MAX_DIMENSION, MAX_DIMENSION);
    png_read_info(png, info);
    image->width = png_get_image_width(png, info);
    image->height = png_get_image_height(png, info);
    if ((uint64_t)image->width * image->height > MAX_PIXELS)
        png_error(png, "image exceeds pixel limit");

    int color = png_get_color_type(png, info);
    int depth = png_get_bit_depth(png, info);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    int transparent_color = png_get_valid(png, info, PNG_INFO_tRNS);
    if (transparent_color)
        png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (depth == 16 && !(color & PNG_COLOR_MASK_ALPHA) && !transparent_color)
        png_set_scale_16(png);
    png_set_bgr(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    image->channels = png_get_channels(png, info);
    image->depth = png_get_bit_depth(png, info);
    size_t row_bytes = png_get_rowbytes(png, info);
    image->stride = (row_bytes + 3U) & ~(size_t)3U;
    if (image->stride > MAX_DECODED_BYTES / image->height)
        png_error(png, "decoded image exceeds memory limit");
    image->bmp = g_try_malloc(54 + image->stride * image->height);
    image->rows = g_try_new(png_bytep, image->height);
    if (!image->bmp || !image->rows)
        png_error(png, "image allocation failed");
    /* PNG rows arrive top-down, but a Windows 3.x BMP stores them bottom-up. */
    for (png_uint_32 y = 0; y < image->height; y++)
        image->rows[y] = image->bmp + 54 + image->stride * (image->height - 1 - y);
    png_read_image(png, image->rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    return 1;
}

static void put_le32(unsigned char *dest, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        dest[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t get_be16(const unsigned char *source)
{
    return (uint32_t)source[0] * 256U + source[1];
}

static void blend_white(png_bytep dest, const png_byte *row, png_uint_32 width, int depth)
{
    /* Blend before reducing the channel depth, as ImageMagick does. */
    if (depth == 16) {
        for (png_uint_32 x = 0; x < width; x++) {
            uint32_t alpha = get_be16(row + 8 * x + 6);
            uint32_t white = (65535U - alpha) * 65535U;
            for (int channel = 0; channel < 3; channel++) {
                uint32_t value = get_be16(row + 8 * x + 2 * channel);
                uint32_t blended = (value * alpha + white + 32767U) / 65535U;
                dest[3 * x + channel] = (blended + 128U) / 257U;
            }
        }
        return;
    }
    for (png_uint_32 x = 0; x < width; x++) {
        unsigned alpha = row[4 * x + 3];
        unsigned white = (255 - alpha) * 255;
        for (int channel = 0; channel < 3; channel++)
            dest[3 * x + channel] = (row[4 * x + channel] * alpha + white + 127) / 255;
    }
}

static GBytes *make_bmp(struct image *image)
{
    const size_t row_bytes = image->width * 3U;
    const size_t padding = (4 - row_bytes % 4) % 4;
    const uint32_t size = (row_bytes + padding) * image->height;
    unsigned char *header = image->bmp;
    memset(header, 0, 54);
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, 54 + size);
    put_le32(header + 10, 54);
    put_le32(header + 14, 40);
    put_le32(header + 18, image->width);
    put_le32(header + 22, image->height);
    header[26] = 1;
    header[28] = 24;
    put_le32(header + 34, size);
    for (png_uint_32 y = 0; y < image->height; y++) {
        png_bytep dest = image->bmp + 54 + (row_bytes + padding) * y;
        if (image->channels == 4) {
            const png_byte *source = image->bmp + 54 + image->stride * y;
            blend_white(dest, source, image->width, image->depth);
        }
        memset(dest + row_bytes, 0, padding);
    }
    return g_bytes_new_take(g_steal_pointer(&image->bmp), 54 + size);
}

GBytes *kakao_png_to_bmp(GBytes *png, GCancellable *cancel, GError **error)
{
    struct image image = {.cancel = cancel};
    image.input = g_bytes_get_data(png, &image.remaining);
    GBytes *bmp = NULL;
    if (read_png(&image)) {
        if (!g_cancellable_set_error_if_cancelled(cancel, error))
            bmp = make_bmp(&image);
    } else if (!g_cancellable_set_error_if_cancelled(cancel, error)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s",
                    image.error ? image.error : "Could not allocate PNG decoder");
    }
    g_free(image.error);
    g_free(image.rows);
    g_free(image.bmp);
    return bmp;
}
