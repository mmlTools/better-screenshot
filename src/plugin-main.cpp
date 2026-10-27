/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "screenshot-service.hpp"
#include "settings-dialog.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/config-file.h>

#include "plugin-support.h"

#include <QWidget>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

using namespace BetterScreenshot;

namespace {

constexpr const char *kCaptureHotkeyName = "better_screenshot.capture";

std::unique_ptr<ScreenshotService> service;
obs_hotkey_id captureHotkeyId = OBS_INVALID_HOTKEY_ID;
bool settingsStored = false;

void captureHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && service)
		service->requestCapture();
}

/* OBS persists bindings of frontend hotkeys to the profile (basic.ini,
 * [Hotkeys] section) when they are changed in the Hotkeys settings page,
 * but it only restores its own hotkeys on startup. Restore ours the same
 * way OBS does it in OBSBasic::CreateHotkeys(). */
void loadHotkeyBindings()
{
	if (captureHotkeyId == OBS_INVALID_HOTKEY_ID)
		return;

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	const char *json = config_get_string(config, "Hotkeys", kCaptureHotkeyName);
	if (!json || !*json)
		return;

	OBSDataAutoRelease data = obs_data_create_from_json(json);
	if (!data)
		return;

	OBSDataArrayAutoRelease bindings = obs_data_get_array(data, "bindings");
	obs_hotkey_load(captureHotkeyId, bindings);
}

/* Safety net for bindings changed outside the Hotkeys settings page. */
void saveHotkeyBindings()
{
	if (captureHotkeyId == OBS_INVALID_HOTKEY_ID)
		return;

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	OBSDataArrayAutoRelease bindings = obs_hotkey_save(captureHotkeyId);
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "bindings", bindings);

	config_set_string(config, "Hotkeys", kCaptureHotkeyName, obs_data_get_json(data));
	config_save_safe(config, "tmp", nullptr);
}

/* Settings used to be stored in the scene collection. If no settings file
 * exists yet, take them from the first scene collection that loads. */
void sceneCollectionCallback(obs_data_t *saveData, bool saving, void *)
{
	if (saving || settingsStored || !service)
		return;

	Settings settings;
	if (!SettingsStore::importLegacy(saveData, settings))
		return;

	service->setSettings(settings);
	settingsStored = SettingsStore::save(settings);
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		loadHotkeyBindings();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		saveHotkeyBindings();
		break;
	default:
		break;
	}
}

void showSettingsDialog(void *)
{
	if (!service)
		return;

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	SettingsDialog dialog(service->settings(), parent);

	if (dialog.exec() != QDialog::Accepted)
		return;

	const Settings settings = dialog.result();
	service->setSettings(settings);
	settingsStored = SettingsStore::save(settings) || settingsStored;
}

} // namespace

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Better Screenshot";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Screenshots of the program output with format, quality and folder control, clipboard copy and "
	       "Discord webhook upload.";
}

bool obs_module_load(void)
{
	service = std::make_unique<ScreenshotService>();

	Settings settings;
	settingsStored = SettingsStore::load(settings);
	service->setSettings(settings);

	obs_frontend_add_tools_menu_item(obs_module_text("PluginName"), showSettingsDialog, nullptr);
	obs_frontend_add_save_callback(sceneCollectionCallback, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);

	captureHotkeyId = obs_hotkey_register_frontend(kCaptureHotkeyName, obs_module_text("Hotkey.Description"),
						       captureHotkey, nullptr);
	if (captureHotkeyId == OBS_INVALID_HOTKEY_ID)
		obs_log(LOG_WARNING, "Failed to register the capture hotkey");

	/* The profile may not be available this early on a cold start;
	 * OBS_FRONTEND_EVENT_FINISHED_LOADING retries once it is. */
	loadHotkeyBindings();

	const char *modulePath = obs_get_module_file_name(obs_current_module());
	obs_log(LOG_INFO, "plugin loaded successfully (version %s, OBS %s, Qt %s) from %s", PLUGIN_VERSION,
		obs_get_version_string(), qVersion(), modulePath ? modulePath : "(unknown path)");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(sceneCollectionCallback, nullptr);
	obs_frontend_remove_event_callback(frontendEvent, nullptr);

	if (captureHotkeyId != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(captureHotkeyId);
		captureHotkeyId = OBS_INVALID_HOTKEY_ID;
	}

	service.reset();

	obs_log(LOG_INFO, "plugin unloaded");
}
