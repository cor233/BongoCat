#include "bongo_cat/model.h"
#if defined(CSM_TARGET_WIN_GL) || defined(CSM_TARGET_LINUX_GL)
#include <GL/glew.h>
#endif
#include "cubism_runtime.hpp"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>
#include <exception>
#include <new>

extern "C" BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *runtime,
    const char *directory, const char *setting, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error) {
    if (!runtime) return BONGO_CAT_ERROR_ARGUMENT;
    bongo_cat::NativeModel *previous = runtime->model;
    bongo_cat::NativeModel *model = nullptr;
    try {
        model = new(std::nothrow) bongo_cat::NativeModel();
        if (!model) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                "Cannot allocate Live2D model");
            return BONGO_CAT_ERROR_MEMORY;
        }
        if (render_options) model->set_render_options(*render_options);
        if (!model->load(directory, setting, preset, progress, userdata, error)) {
            delete model;
            return error ? error->code : BONGO_CAT_ERROR_CUBISM;
        }
        /* Keep the previous renderer alive until the replacement is complete. */
        model->reshape(runtime->width, runtime->height);
        if (!model->load_textures(error, progress, userdata)) {
            BongoCatResult result = error ? error->code : BONGO_CAT_ERROR_CUBISM;
            delete model;
            return result;
        }
        GLenum ready_error = glGetError();
        SDL_Log("[runtime] Live2D resource handoff: stage=replacement-ready "
            "new_textures=%zu previous_textures=%zu current_window=%p "
            "current_context=%p gl_error=0x%x",
            model->texture_count(), previous ? previous->texture_count() : 0,
            (void *)SDL_GL_GetCurrentWindow(),
            (void *)SDL_GL_GetCurrentContext(), (unsigned)ready_error);
        if (progress) progress(userdata, 1.0f);
        runtime->model = model;
        const size_t released_textures = previous ? previous->texture_count() : 0;
        // Loading and drawing use the same GL context. GL deletion preserves
        // already submitted draws; future frames only use the replacement.
        // Release now even when a hidden/minimized window never draws again.
        if (previous) {
            delete previous;
            // Submit any queued work so driver-side releases can finish even
            // when this switch is followed by no draws or buffer swaps.
            glFlush();
        }
        SDL_Log("[runtime] Live2D resource handoff: stage=previous-released "
            "texture_refs=%zu", released_textures);
        GLenum retired_error = glGetError();
        SDL_Log("[runtime] Live2D resource handoff: stage=complete "
            "new_textures=%zu current_window=%p current_context=%p gl_error=0x%x",
            model->texture_count(),
            (void *)SDL_GL_GetCurrentWindow(),
            (void *)SDL_GL_GetCurrentContext(), (unsigned)retired_error);
        return BONGO_CAT_OK;
    } catch (const std::bad_alloc &) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Out of memory while loading the Live2D model");
    } catch (const std::exception &exception) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Live2D model load failed: %s", exception.what());
    } catch (...) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Live2D model load failed with an unknown exception");
    }
    delete model;
    return error ? error->code : BONGO_CAT_ERROR_CUBISM;
}
