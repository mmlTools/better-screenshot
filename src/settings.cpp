/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "settings.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/util.hpp>

#include "plugin-support.h"
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <algorithm>

namespace BetterScreenshot {

namespace {

constexpr const char *kSettingsFile = "settings.json";
constexpr const char *kLegacyRoot = "better_screenshot";

constexpr const char *kFormat = "format";
constexpr const char *kQuality = "quality";
constexpr const char *kSaveLocal = "save_local";
constexpr const char *kUseRecordingFolder = "use_recording_folder";
constexpr const char *kSavePath = "save_path";
constexpr const char *kFilenamePrefix = "filename_prefix";
constexpr const char *kCopyToClipboard = "copy_to_clipboard";
constexpr const char *kWebhookUrl = "webhook_url";
constexpr const char *kWebhookMessage = "webhook_message";
constexpr const char *kWebhookUseCustomFormat = "webhook_use_custom_format";
constexpr const char *kWebhookFormat = "webhook_format";

QString settingsFilePath()
{
	BPtr<char> path = obs_module_config_path(kSettingsFile);
	return path ? QString::fromUtf8(path.Get()) : QString();
}

} // namespace

QString Settings::defaultSavePath()
{
	QString path = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (path.isEmpty())
		path = QDir::homePath();

	return QDir(path).filePath(QStringLiteral("OBS Better Screenshot"));
}

QString Settings::normalizeFormat(const QString &format)
{
	const QString f = format.trimmed().toLower();

	if (f == QLatin1String("jpg") || f == QLatin1String("jpeg"))
		return QStringLiteral("jpg");
	if (f == QLatin1String("webp"))
		return QStringLiteral("webp");

	return QStringLiteral("png");
}

bool Settings::isLossy(const QString &format)
{
	const QString f = normalizeFormat(format);
	return f == QLatin1String("jpg") || f == QLatin1String("webp");
}

QString Settings::effectiveWebhookFormat() const
{
	return normalizeFormat(webhookUseCustomFormat ? webhookFormat : format);
}

void Settings::sanitize()
{
	format = normalizeFormat(format);
	webhookFormat = normalizeFormat(webhookFormat);
	quality = std::clamp(quality, 1, 100);

	savePath = savePath.trimmed();
	if (savePath.isEmpty())
		savePath = defaultSavePath();

	filenamePrefix = filenamePrefix.trimmed();
	if (filenamePrefix.isEmpty())
		filenamePrefix = QStringLiteral("Screenshot");

	webhookUrl = webhookUrl.trimmed();
}

void SettingsStore::fromData(obs_data_t *data, Settings &settings)
{
	obs_data_set_default_string(data, kFormat, "png");
	obs_data_set_default_int(data, kQuality, 95);
	obs_data_set_default_bool(data, kSaveLocal, true);
	obs_data_set_default_bool(data, kUseRecordingFolder, false);
	obs_data_set_default_string(data, kSavePath, "");
	obs_data_set_default_string(data, kFilenamePrefix, "Screenshot");
	obs_data_set_default_bool(data, kCopyToClipboard, false);
	obs_data_set_default_string(data, kWebhookUrl, "");
	obs_data_set_default_string(data, kWebhookMessage, "");
	obs_data_set_default_bool(data, kWebhookUseCustomFormat, false);
	obs_data_set_default_string(data, kWebhookFormat, "jpg");

	settings.format = QString::fromUtf8(obs_data_get_string(data, kFormat));
	settings.quality = static_cast<int>(obs_data_get_int(data, kQuality));
	settings.saveLocal = obs_data_get_bool(data, kSaveLocal);
	settings.useRecordingFolder = obs_data_get_bool(data, kUseRecordingFolder);
	settings.savePath = QString::fromUtf8(obs_data_get_string(data, kSavePath));
	settings.filenamePrefix = QString::fromUtf8(obs_data_get_string(data, kFilenamePrefix));
	settings.copyToClipboard = obs_data_get_bool(data, kCopyToClipboard);
	settings.webhookUrl = QString::fromUtf8(obs_data_get_string(data, kWebhookUrl));
	settings.webhookMessage = QString::fromUtf8(obs_data_get_string(data, kWebhookMessage));
	settings.webhookUseCustomFormat = obs_data_get_bool(data, kWebhookUseCustomFormat);
	settings.webhookFormat = QString::fromUtf8(obs_data_get_string(data, kWebhookFormat));

	settings.sanitize();
}

void SettingsStore::toData(const Settings &settings, obs_data_t *data)
{
	obs_data_set_string(data, kFormat, settings.format.toUtf8().constData());
	obs_data_set_int(data, kQuality, settings.quality);
	obs_data_set_bool(data, kSaveLocal, settings.saveLocal);
	obs_data_set_bool(data, kUseRecordingFolder, settings.useRecordingFolder);
	obs_data_set_string(data, kSavePath, settings.savePath.toUtf8().constData());
	obs_data_set_string(data, kFilenamePrefix, settings.filenamePrefix.toUtf8().constData());
	obs_data_set_bool(data, kCopyToClipboard, settings.copyToClipboard);
	obs_data_set_string(data, kWebhookUrl, settings.webhookUrl.toUtf8().constData());
	obs_data_set_string(data, kWebhookMessage, settings.webhookMessage.toUtf8().constData());
	obs_data_set_bool(data, kWebhookUseCustomFormat, settings.webhookUseCustomFormat);
	obs_data_set_string(data, kWebhookFormat, settings.webhookFormat.toUtf8().constData());
}

bool SettingsStore::load(Settings &settings)
{
	const QString path = settingsFilePath();
	if (path.isEmpty() || !QFile::exists(path))
		return false;

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(path.toUtf8().constData(), "bak");
	if (!data) {
		obs_log(LOG_WARNING, "Could not read settings file '%s'", path.toUtf8().constData());
		return false;
	}

	fromData(data, settings);
	return true;
}

bool SettingsStore::save(const Settings &settings)
{
	const QString path = settingsFilePath();
	if (path.isEmpty())
		return false;

	BPtr<char> dir = obs_module_config_path("");
	if (dir && os_mkdirs(dir.Get()) == MKDIR_ERROR) {
		obs_log(LOG_WARNING, "Could not create settings directory '%s'", dir.Get());
		return false;
	}

	OBSDataAutoRelease data = obs_data_create();
	toData(settings, data);

	if (!obs_data_save_json_safe(data, path.toUtf8().constData(), "tmp", "bak")) {
		obs_log(LOG_WARNING, "Could not write settings file '%s'", path.toUtf8().constData());
		return false;
	}

	return true;
}

bool SettingsStore::importLegacy(obs_data_t *sceneCollectionData, Settings &settings)
{
	if (!sceneCollectionData || !obs_data_has_user_value(sceneCollectionData, kLegacyRoot))
		return false;

	OBSDataAutoRelease legacy = obs_data_get_obj(sceneCollectionData, kLegacyRoot);
	if (!legacy)
		return false;

	fromData(legacy, settings);
	obs_log(LOG_INFO, "Imported settings from the scene collection (pre-1.3.0 storage)");
	return true;
}

} // namespace BetterScreenshot
