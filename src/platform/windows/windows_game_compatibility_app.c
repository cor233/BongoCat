#include "windows_game_compatibility.h"
#include "bongo_cat/app.h"
#include <SDL3/SDL_log.h>

bool bongo_cat_windows_game_compatibility_startup(BongoCatApp *app,
    bool *restarting, BongoCatError *error) {
    *restarting = false;
    if (app->smoke || app->secondary_pet || !app->settings.app.game_compatibility)
        return true;
    bool administrator = false;
    if (!bongo_cat_windows_game_compatibility_elevated(&administrator, error))
        return false;
    if (administrator) return true;
    if (!bongo_cat_windows_game_compatibility_launch(true, error)) return false;
    *restarting = true;
    return true;
}

bool bongo_cat_windows_game_compatibility_set(BongoCatApp *app,
    bool enabled, BongoCatError *error) {
    if (!app || app->secondary_pet || app->smoke || app->settings_store_blocked) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Game compatibility settings are not writable in this instance");
        return false;
    }
    bool administrator = false;
    if (!bongo_cat_windows_game_compatibility_elevated(&administrator, error))
        return false;
    BongoCatApplicationPreferences previous = app->settings.app;
    /* Ordinary login launches also honor the saved mode at application startup.
       Disabling must additionally remove any existing elevated login task. */
    app->settings.app.game_compatibility = enabled;
    if (!enabled) app->settings.app.autostart_admin = false;
    if (bongo_cat_settings_save(app->settings_path, &app->settings, error) != BONGO_CAT_OK) {
        app->settings.app = previous;
        return false;
    }
    bool needs_restart = enabled || administrator;
    if (needs_restart && !bongo_cat_windows_game_compatibility_launch(enabled, error)) {
        app->settings.app = previous;
        BongoCatError rollback = {0};
        (void)bongo_cat_settings_save(app->settings_path, &app->settings, &rollback);
        return false;
    }
    if (!enabled && previous.autostart && previous.autostart_admin &&
        bongo_cat_platform_set_autostart(true, false, error) != BONGO_CAT_OK) {
        app->settings.app = previous;
        BongoCatError rollback = {0};
        (void)bongo_cat_settings_save(app->settings_path, &app->settings, &rollback);
        bongo_cat_windows_game_compatibility_cancel();
        return false;
    }
    if (needs_restart) app->running = false;
    return true;
}
