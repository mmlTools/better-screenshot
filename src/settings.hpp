/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-data.h>

#include <QString>

namespace BetterScreenshot {

/* Image formats are stored by their lowercase file extension ("png", "jpg",
 * "webp"). Quality applies to lossy formats only and follows Qt's 0-100 scale. */
struct Settings {
	QString format = QStringLiteral("png");
	int quality = 95;

	bool saveLocal = true;
	bool useRecordingFolder = false;
	QString savePath;
	QString filenamePrefix = QStringLiteral("Screenshot");

	bool copyToClipboard = false;

	QString webhookUrl;
	QString webhookMessage;
	bool webhookUseCustomFormat = false;
	QString webhookFormat = QStringLiteral("jpg");

	/* Normalizes user supplied values (unknown formats, out of range quality,
	 * empty paths) so the rest of the plugin can rely on them. */
	void sanitize();

	bool wantsWebhook() const { return !webhookUrl.trimmed().isEmpty(); }
	bool hasAnyOutput() const { return saveLocal || copyToClipboard || wantsWebhook(); }
	QString effectiveWebhookFormat() const;

	static QString defaultSavePath();
	static QString normalizeFormat(const QString &format);
	static bool isLossy(const QString &format);
};

/* Settings live in the plugin's own config file
 * (<obs config>/plugin_config/better-screenshot/settings.json) so they are
 * shared by every scene collection and profile. */
class SettingsStore {
public:
	/* Returns false if no settings file exists yet. */
	static bool load(Settings &settings);
	static bool save(const Settings &settings);

	/* Versions before 1.3.0 stored the settings inside the scene collection.
	 * Reads them from that data (if present) so they can be migrated. */
	static bool importLegacy(obs_data_t *sceneCollectionData, Settings &settings);

	static void fromData(obs_data_t *data, Settings &settings);
	static void toData(const Settings &settings, obs_data_t *data);
};

} // namespace BetterScreenshot
