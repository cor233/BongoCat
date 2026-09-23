#include "image_internal.h"
#include "bongo_cat/gl_api.h"

#include <SDL3/SDL.h>
#include <string.h>

typedef struct ImageProgressStage {
    BongoCatImageProgress progress;
    void *userdata;
    float start, span;
} ImageProgressStage;

static void report_progress(void *userdata, float progress) {
    ImageProgressStage *stage = userdata;
    if (stage && stage->progress)
        stage->progress(stage->userdata, stage->start + stage->span * progress);
}

static bool texture_fits(int width, int height, int limit,
    const char *path, BongoCatError *error) {
    if (width <= limit && height <= limit) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Live2D texture %dx%d exceeds the GPU limit of %d pixels; "
        "cannot preserve the original detail: %s", width, height, limit, path);
    return false;
}

typedef struct ModelRowUpload {
    GLuint texture;
    BongoCatImageUploadBuffer buffer;
    int width, height, limit;
    const char *path;
    BongoCatImageAlphaMask *alpha;
    BongoCatError *error;
    bool upload_failed;
    bool mipmaps;
} ModelRowUpload;

typedef bool (*RowDecoder)(const char *, BongoCatImageRows, void *,
    BongoCatImageProgress, void *);

static bool upload_rows(void *userdata, BongoCatImage *rows, int height, int y) {
    ModelRowUpload *upload = userdata;
    if (!y) {
        upload->width = rows->width;
        upload->height = height;
        if (texture_fits(rows->width, height, upload->limit,
            upload->path, upload->error))
            upload->texture = bongo_cat_image_begin_model_texture(
                rows->width, height, upload->mipmaps, upload->error);
        if (!upload->texture) upload->upload_failed = true;
    }
    if (upload->upload_failed) return false;
    bongo_cat_image_alpha_mask_rows(rows, height, y, upload->alpha);
    upload->upload_failed = !bongo_cat_image_upload_model_rows(
        upload->texture, rows, y, &upload->buffer, upload->error);
    return !upload->upload_failed;
}

static GLuint stream_model(const char *path, int limit, int *width, int *height,
    BongoCatImageAlphaMask *alpha, ImageProgressStage *stage,
    RowDecoder decode, bool *upload_failed, BongoCatError *error) {
    ModelRowUpload upload = {.limit = limit, .path = path,
        .alpha = alpha, .error = error, .mipmaps = true};
    bool decoded = decode(path, upload_rows, &upload,
        stage->progress ? report_progress : NULL, stage);
    bongo_cat_image_release_upload_buffer(&upload.buffer);
    BongoCatError mip_error = {0};
    if (decoded && !bongo_cat_image_finish_model_texture(upload.texture, &mip_error)) {
        /* Discard partial mip storage, then decode the original strips again.
           No full-size CPU backup is needed for the linear fallback. */
        glDeleteTextures(1, &upload.texture);
        upload.texture = 0;
        upload.mipmaps = false;
        bongo_cat_gl_clear_errors();
        stage->start += stage->span;
        stage->span = (1.0f - stage->start) * .25f;
        decoded = decode(path, upload_rows, &upload,
            stage->progress ? report_progress : NULL, stage);
        bongo_cat_image_release_upload_buffer(&upload.buffer);
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
            "Live2D mipmap upload unavailable; retrying original %dx%d pixels",
            upload.width, upload.height);
    }
    *upload_failed = upload.upload_failed;
    if (!decoded) {
        if (upload.texture) glDeleteTextures(1, &upload.texture);
        if (alpha) memset(alpha, 0, sizeof(*alpha));
        return 0;
    }
    if (width) *width = upload.width;
    if (height) *height = upload.height;
    SDL_Log("Live2D texture preserved at %dx%d (streamed): %s",
        upload.width, upload.height, path);
    if (stage->progress) stage->progress(stage->userdata, 1.0f);
    return upload.texture;
}

unsigned int bongo_cat_image_texture_model(const char *path, bool direct_decode,
    int *width, int *height, BongoCatImageAlphaMask *alpha,
    BongoCatImageProgress progress, void *userdata, BongoCatError *error) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (alpha) memset(alpha, 0, sizeof(*alpha));
    if (!path || !path[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "A Live2D texture path is required");
        return 0;
    }
    if (!SDL_GL_GetCurrentContext() || !bongo_cat_gl_clear_errors()) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot load a Live2D texture without a usable OpenGL context");
        return 0;
    }
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (glGetError() != GL_NO_ERROR || limit < 1) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot query the GPU texture size limit");
        return 0;
    }
    int source_width = 0, source_height = 0;
    if (bongo_cat_image_info(path, &source_width, &source_height) &&
        !texture_fits(source_width, source_height, limit, path, error)) return 0;

    BongoCatImage image = {0};
    ImageProgressStage stage = {progress, userdata, 0.0f, .30f};
    BongoCatImageProgress staged = progress ? report_progress : NULL;
    // A small window does not imply that the parts of a large atlas are small.
    if (!direct_decode) {
        stage.span = .65f;
        bool upload_failed = false;
        GLuint texture = stream_model(path, limit, width, height, alpha,
            &stage, bongo_cat_image_decode_png_rows, &upload_failed, error);
        if (texture || upload_failed) return texture;
#ifdef _WIN32
        stage = (ImageProgressStage){progress, userdata, .80f, .10f};
        texture = stream_model(path, limit, width, height, alpha,
            &stage, bongo_cat_image_decode_wic_rows, &upload_failed, error);
        if (texture || upload_failed) return texture;
#endif
        // Unsupported encodings retain the existing general decoder fallback.
        stage = (ImageProgressStage){progress, userdata, .95f, .01f};
    }
    if (bongo_cat_image_decode_pixels_responsive(path, &image,
        staged, &stage, error) != BONGO_CAT_OK) return 0;
    if (!texture_fits(image.width, image.height, limit, path, error)) {
        bongo_cat_image_free(&image);
        return 0;
    }
    float decoded_progress = stage.start + stage.span;
    if (progress) progress(userdata, decoded_progress);
    stage = (ImageProgressStage){progress, userdata, decoded_progress,
        (1.0f - decoded_progress) * .5f};
    bongo_cat_image_make_alpha_mask_progress(&image, alpha, staged, &stage);
    GLuint texture = bongo_cat_image_upload_texture(&image, 0, true, error);
    if (texture) {
        if (width) *width = image.width;
        if (height) *height = image.height;
        SDL_Log("Live2D texture preserved at %dx%d: %s", image.width, image.height, path);
    } else {
        if (alpha) memset(alpha, 0, sizeof(*alpha));
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Live2D texture failed: %s (%s)",
            path, error ? error->message : "upload failed");
    }
    bongo_cat_image_free(&image);
    if (progress) progress(userdata, 1.0f);
    return texture;
}
