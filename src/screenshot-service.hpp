/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "settings.hpp"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <memory>

class QNetworkAccessManager;

namespace BetterScreenshot {

/* Owns the capture queue and turns captured frames into files, clipboard
 * content and webhook uploads.
 *
 * Captures are serialized (one at a time) so rapid hotkey presses cannot
 * exhaust GPU staging memory. Encoding runs on a worker thread, the webhook
 * upload is asynchronous, and nothing in here blocks the UI thread. All
 * public methods must be called from the UI thread. */
class ScreenshotService : public QObject {
	Q_OBJECT

public:
	explicit ScreenshotService(QObject *parent = nullptr);
	~ScreenshotService() override;

	const Settings &settings() const { return settings_; }
	void setSettings(const Settings &settings);

	/* Queues a capture of the program output. */
	void requestCapture();

private:
	struct Job {
		Settings settings;
		QImage image;

		QString filePath;
		QByteArray fileBytes;

		QByteArray webhookBytes;
		QString webhookFileName;

		QStringList actions;
		QString error;
	};
	using JobPtr = std::shared_ptr<Job>;

	void startNextCapture();
	void onCaptureReady(const QImage &image);
	void onCaptureFailed(const QString &reason);

	bool prepareOutputPath(Job &job);
	static void encode(const JobPtr &job);
	void onEncoded(const JobPtr &job);
	void upload(const JobPtr &job);
	void complete(const JobPtr &job);

	void notifyFailure(const QString &message);

	Settings settings_;
	QThreadPool pool;
	QNetworkAccessManager *network = nullptr;

	int pending = 0;
	bool busy = false;
};

} // namespace BetterScreenshot
