/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>
#include <obs.hpp>

#include <QImage>
#include <QObject>
#include <QString>

namespace BetterScreenshot {

/* Renders the program output (or a specific source) straight from the
 * graphics subsystem into a staging surface and hands the pixels to the
 * UI thread as a QImage.
 *
 * The work is spread over three consecutive video ticks on the graphics
 * thread (render, download, copy), which is the same approach the OBS
 * frontend uses for its own screenshots. No temporary file is written and
 * the UI thread is never blocked. */
class ScreenshotCapture : public QObject {
	Q_OBJECT

public:
	/* A null source captures the program output. */
	explicit ScreenshotCapture(obs_source_t *source = nullptr, QObject *parent = nullptr);
	~ScreenshotCapture() override;

	/* Must be called from the thread that owns this object (the UI thread).
	 * Exactly one of ready() / failed() is emitted on that thread afterwards. */
	void start();

signals:
	void ready(const QImage &image);
	void failed(const QString &reason);

private:
	enum class Stage { Render, Download, Copy, Finished };

	static void tick(void *param, float seconds);
	void processStage();
	bool render();
	void download();
	bool copy();
	void finish(bool success);

	OBSWeakSource weakSource;
	Stage stage = Stage::Render;
	bool tickRegistered = false;

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurf = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	QImage image;
	QString error;
};

} // namespace BetterScreenshot
