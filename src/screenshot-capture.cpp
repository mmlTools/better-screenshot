/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "screenshot-capture.hpp"

#include <obs-module.h>

#include "plugin-support.h"

#include <cstring>

namespace BetterScreenshot {

ScreenshotCapture::ScreenshotCapture(obs_source_t *source, QObject *parent)
	: QObject(parent),
	  weakSource(OBSGetWeakRef(source))
{
}

ScreenshotCapture::~ScreenshotCapture()
{
	if (tickRegistered)
		obs_remove_tick_callback(tick, this);

	if (texrender || stagesurf) {
		obs_enter_graphics();
		gs_stagesurface_destroy(stagesurf);
		gs_texrender_destroy(texrender);
		obs_leave_graphics();
	}
}

void ScreenshotCapture::start()
{
	if (tickRegistered || stage != Stage::Render)
		return;

	tickRegistered = true;
	obs_add_tick_callback(tick, this);
}

void ScreenshotCapture::tick(void *param, float)
{
	auto *self = static_cast<ScreenshotCapture *>(param);
	if (self->stage == Stage::Finished)
		return;

	obs_enter_graphics();
	self->processStage();
	obs_leave_graphics();
}

void ScreenshotCapture::processStage()
{
	switch (stage) {
	case Stage::Render:
		if (!render()) {
			finish(false);
			return;
		}
		stage = Stage::Download;
		break;

	case Stage::Download:
		download();
		stage = Stage::Copy;
		break;

	case Stage::Copy:
		finish(copy());
		break;

	case Stage::Finished:
		break;
	}
}

bool ScreenshotCapture::render()
{
	OBSSourceAutoRelease source = OBSGetStrongRef(weakSource);

	if (source) {
		width = obs_source_get_width(source);
		height = obs_source_get_height(source);
	} else {
		obs_video_info ovi;
		obs_get_video_info(&ovi);
		width = ovi.base_width;
		height = ovi.base_height;
	}

	if (!width || !height) {
		error = QString::fromUtf8(obs_module_text("Error.InvalidSize"));
		return false;
	}

	/* Always tonemap to SDR: the encoders we hand the image to are 8-bit. */
	const enum gs_color_space space = GS_CS_SRGB;
	const enum gs_color_format format = gs_get_format_from_space(space);

	texrender = gs_texrender_create(format, GS_ZS_NONE);
	stagesurf = gs_stagesurface_create(width, height, format);

	if (!texrender || !stagesurf) {
		error = QString::fromUtf8(obs_module_text("Error.GraphicsAlloc"));
		return false;
	}

	if (!gs_texrender_begin_with_color_space(texrender, width, height, space)) {
		error = QString::fromUtf8(obs_module_text("Error.RenderFailed"));
		return false;
	}

	vec4 zero;
	vec4_zero(&zero);

	gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, float(width), 0.0f, float(height), -100.0f, 100.0f);
	gs_set_viewport(0, 0, int(width), int(height));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (source) {
		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);
	} else {
		obs_render_main_texture();
	}

	gs_blend_state_pop();

	gs_projection_pop();
	gs_viewport_pop();

	gs_texrender_end(texrender);
	return true;
}

void ScreenshotCapture::download()
{
	gs_stage_texture(stagesurf, gs_texrender_get_texture(texrender));
}

bool ScreenshotCapture::copy()
{
	uint8_t *data = nullptr;
	uint32_t linesize = 0;

	if (!gs_stagesurface_map(stagesurf, &data, &linesize)) {
		error = QString::fromUtf8(obs_module_text("Error.MapFailed"));
		return false;
	}

	image = QImage(int(width), int(height), QImage::Format_RGBX8888);

	const size_t rowBytes = size_t(image.bytesPerLine());
	for (uint32_t y = 0; y < height; ++y)
		std::memcpy(image.scanLine(int(y)), data + size_t(y) * linesize, rowBytes);

	gs_stagesurface_unmap(stagesurf);
	return true;
}

/* Runs on the graphics thread; the signal is delivered on the UI thread. */
void ScreenshotCapture::finish(bool success)
{
	stage = Stage::Finished;
	obs_remove_tick_callback(tick, this);
	tickRegistered = false;

	if (success) {
		QImage result = std::move(image);
		QMetaObject::invokeMethod(this, [this, result]() { emit ready(result); }, Qt::QueuedConnection);
	} else {
		const QString reason = error;
		QMetaObject::invokeMethod(this, [this, reason]() { emit failed(reason); }, Qt::QueuedConnection);
	}
}

} // namespace BetterScreenshot
