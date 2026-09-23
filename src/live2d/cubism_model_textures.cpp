#include "cubism_model.hpp"
extern "C" {
#include "bongo_cat/sha256.h"
}

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cstring>

namespace bongo_cat {

struct ModelTexture {
    GLuint id = 0;
    SDL_GLContext context = nullptr;
    bool direct = false;
    char digest[65]{};
    BongoCatImageAlphaMask alpha{};
    ~ModelTexture() { if (id) glDeleteTextures(1, &id); }
};

// Only live model owners retain texture storage. Weak entries allow identical
// atlases in different model/mode directories to share it during handoff.
static std::vector<std::weak_ptr<ModelTexture>> live_textures;

struct TextureHashJob {
    const char *path;
    char *digest;
    BongoCatResult result = BONGO_CAT_ERROR_IO;
};

static int SDLCALL hash_worker(void *userdata) {
    auto &job = *static_cast<TextureHashJob *>(userdata);
    job.result = bongo_cat_sha256_file(job.path, job.digest, nullptr);
    return (int)job.result;
}

static bool hash_texture(const std::string &path, char digest[65],
    BongoCatImageProgress progress, void *userdata) {
    TextureHashJob job{path.c_str(), digest};
    SDL_Thread *worker = progress ? SDL_CreateThread(hash_worker,
        BONGO_CAT_SLUG "-texture-hash", &job) : nullptr;
    if (worker) {
        const auto wait = [](SDL_Thread *thread) { SDL_WaitThread(thread, nullptr); };
        std::unique_ptr<SDL_Thread, decltype(wait)> joined(worker, wait);
        while (SDL_GetThreadState(worker) == SDL_THREAD_ALIVE) {
            progress(userdata, 0.0f);
            SDL_Delay(2);
        }
    } else hash_worker(&job);
    return job.result == BONGO_CAT_OK;
}

static bool same_source(const std::string &path, const SDL_PathInfo &before) {
    SDL_PathInfo after{};
    return SDL_GetPathInfo(path.c_str(), &after) && after.type == SDL_PATHTYPE_FILE &&
        before.size == after.size && before.modify_time == after.modify_time &&
        before.create_time == after.create_time;
}

static bool full_sampling(GLuint texture) {
    GLint previous = 0, filter = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
    // A temporary allocation failure must not make later loads inherit the
    // original-size linear fallback when a full mip chain could succeed.
    return filter == GL_LINEAR_MIPMAP_LINEAR;
}

static std::shared_ptr<ModelTexture> acquire_texture(const std::string &path,
    bool direct, BongoCatImageProgress progress, void *userdata,
    BongoCatError *error) {
    live_textures.erase(std::remove_if(live_textures.begin(), live_textures.end(),
        [](const auto &entry) { return entry.expired(); }), live_textures.end());
    SDL_GLContext context = SDL_GL_GetCurrentContext();
    SDL_PathInfo before{};
    char digest[65]{};
    // Hash the bytes on every acquisition: edits and same-name replacements
    // must never reuse stale pixels, even when their size/timestamp matches.
    bool reusable = context && SDL_GetPathInfo(path.c_str(), &before) &&
        before.type == SDL_PATHTYPE_FILE &&
        hash_texture(path, digest, progress, userdata) &&
        same_source(path, before);
    if (reusable) {
        for (const auto &entry : live_textures) {
            auto texture = entry.lock();
            if (texture && texture->context == context && texture->direct == direct &&
                std::strcmp(texture->digest, digest) == 0) {
                SDL_Log("Live2D texture shared: %s", path.c_str());
                return texture;
            }
        }
    }
    // Separate the object from its weak control block so expired registry
    // entries cannot retain the 16 KiB alpha mask after the last owner leaves.
    std::shared_ptr<ModelTexture> texture(new ModelTexture);
    texture->context = context;
    texture->direct = direct;
    texture->id = bongo_cat_image_texture_model(path.c_str(), direct,
        nullptr, nullptr, &texture->alpha, progress, userdata, error);
    if (!texture->id) return {};
    if (reusable && same_source(path, before) && full_sampling(texture->id)) {
        std::memcpy(texture->digest, digest, sizeof(digest));
        live_textures.emplace_back(texture);
    }
    return texture;
}

struct TextureProgressContext {
    BongoCatLive2DLoadProgress callback;
    void *userdata;
    float start;
    float span;
};

static void texture_progress(void *userdata, float progress) {
    auto *context = static_cast<TextureProgressContext *>(userdata);
    if (context && context->callback)
        context->callback(context->userdata,
            context->start + context->span * progress);
}

void NativeModel::bind_textures() {
    auto *renderer = GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer) return;
    for (size_t i = 0; i < textures_.size(); ++i)
        if (textures_[i])
            renderer->BindTexture((Csm::csmInt32)i, textures_[i]->id);
    renderer->IsPremultipliedAlpha(true);
}

void NativeModel::release_textures() {
    textures_.clear();
    triangle_alpha_.clear();
}

const BongoCatImageAlphaMask *NativeModel::texture_alpha(int index) const {
    return index >= 0 && (size_t)index < textures_.size() && textures_[(size_t)index]
        ? &textures_[(size_t)index]->alpha : nullptr;
}

bool NativeModel::load_textures(BongoCatError *error,
    BongoCatLive2DLoadProgress progress, void *userdata) {
    release_textures();
    int count = setting_->GetTextureCount();
    textures_.assign((size_t)count, nullptr);
    TextureProgressContext texture_context = {progress, userdata, .50f,
        .45f / (float)(count > 0 ? count : 1)};
    for (int i = 0; i < count; ++i) {
        texture_context.start = .50f + .45f * (float)i /
            (float)(count > 0 ? count : 1);
        textures_[(size_t)i] = acquire_texture(
            path(setting_->GetTextureFileName(i)), direct_textures_,
            progress ? texture_progress : nullptr, &texture_context, error);
        if (!textures_[(size_t)i]) {
            release_textures();
            return false;
        }
        if (progress) progress(userdata, .50f + .45f * (float)(i + 1) /
            (float)(count > 0 ? count : 1));
    }
    prepare_expression_frame();
    release_renderer();
    if (!create_renderer(error)) {
        release_textures();
        return false;
    }
    renderer_width_ = width_;
    renderer_height_ = height_;
    return true;
}

} // namespace bongo_cat
