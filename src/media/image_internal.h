#ifndef BONGO_CAT_IMAGE_INTERNAL_H
#define BONGO_CAT_IMAGE_INTERNAL_H

#include "bongo_cat/image.h"

BongoCatResult bongo_cat_image_decode_pixels(const char *path,
    BongoCatImage *image, BongoCatError *error);
BongoCatResult bongo_cat_image_decode_pixels_responsive(const char *path,
    BongoCatImage *image, BongoCatImageProgress progress, void *userdata,
    BongoCatError *error);
void bongo_cat_image_make_alpha_mask_progress(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask, BongoCatImageProgress progress,
    void *userdata);
/* Input pixels must use premultiplied RGBA. */
bool bongo_cat_image_upload_mipmaps(const BongoCatImage *image);
bool bongo_cat_image_generate_mipmaps(void);
/* Model uploads premultiply the decoded pixels in place, exactly once.
   Existing textures are only supported for straight-alpha image updates. */
unsigned int bongo_cat_image_upload_texture(BongoCatImage *image,
    unsigned int existing, bool model, BongoCatError *error);
unsigned int bongo_cat_image_begin_model_texture(int width, int height,
    bool mipmaps, BongoCatError *error);
typedef struct BongoCatImageUploadBuffer {
    unsigned int texture, framebuffer;
    int width, height;
} BongoCatImageUploadBuffer;
void bongo_cat_image_release_upload_buffer(BongoCatImageUploadBuffer *buffer);
bool bongo_cat_image_upload_model_rows(unsigned int texture,
    BongoCatImage *rows, int y, BongoCatImageUploadBuffer *buffer,
    BongoCatError *error);
bool bongo_cat_image_finish_model_texture(unsigned int texture,
    BongoCatError *error);
void bongo_cat_image_alpha_mask_rows(const BongoCatImage *rows, int height,
    int y, BongoCatImageAlphaMask *mask);

/* Row callbacks run on the calling thread; decoding uses one bounded buffer.
   Returning false cancels decoding before producing another strip. */
typedef bool (*BongoCatImageRows)(void *userdata, BongoCatImage *rows,
    int height, int y);
/* Fast path for PNG8 RGB/RGBA without interlacing or tRNS. Other encodings
   return false so the caller can use its general decoder. */
bool bongo_cat_image_decode_png_rows(const char *path,
    BongoCatImageRows consume, void *consumer,
    BongoCatImageProgress progress, void *userdata);
#ifdef _WIN32
bool bongo_cat_image_decode_wic_rows(const char *path,
    BongoCatImageRows consume, void *consumer,
    BongoCatImageProgress progress, void *userdata);
bool bongo_cat_image_decode_wic_responsive(const char *path,
    BongoCatImage *image, int max_width, int max_height,
    BongoCatImageProgress progress, void *userdata);
#endif

#endif
