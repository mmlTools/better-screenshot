/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "screenshot-service.hpp"
#include "screenshot-capture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/util.hpp>

#include "plugin-support.h"
#include <util/config-file.h>
#include <util/platform.h>

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>
#include <QWidget>

#include <cstring>

namespace BetterScreenshot {

namespace {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

/* The folder OBS records into, resolved the same way the built-in
 * screenshot feature does it. Empty if the profile is not available. */
QString recordingFolder()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return {};

	const char *mode = config_get_string(config, "Output", "Mode");
	const char *path = nullptr;

	if (mode && strcmp(mode, "Advanced") == 0) {
		const char *type = config_get_string(config, "AdvOut", "RecType");
		path = (type && strcmp(type, "Standard") != 0) ? config_get_string(config, "AdvOut", "FFFilePath")
							       : config_get_string(config, "AdvOut", "RecFilePath");
	} else {
		path = config_get_string(config, "SimpleOutput", "FilePath");
	}

	return path ? QString::fromUtf8(path) : QString();
}

/* Builds "<prefix> <OBS filename formatting>.<ext>" with the formatting
 * configured in the profile, exactly like the built-in screenshot, and makes
 * the name unique unless the profile allows overwriting. */
QString buildFileName(const QString &prefix, const QString &ext, const QDir &dir)
{
	config_t *config = obs_frontend_get_profile_config();

	const char *formatting = config ? config_get_string(config, "Output", "FilenameFormatting") : nullptr;
	const bool noSpace = config && config_get_bool(config, "SimpleOutput", "FileNameWithoutSpace");
	const bool overwrite = config && config_get_bool(config, "Output", "OverwriteIfExists");

	QString format = prefix;
	if (!format.isEmpty() && !format.endsWith(QLatin1Char(' ')))
		format += QLatin1Char(' ');
	format += (formatting && *formatting) ? QString::fromUtf8(formatting)
					      : QStringLiteral("%CCYY-%MM-%DD %hh-%mm-%ss");

	BPtr<char> generated =
		os_generate_formatted_filename(ext.toUtf8().constData(), !noSpace, format.toUtf8().constData());
	const QString base = QString::fromUtf8(generated.Get());

	if (overwrite || !dir.exists(base))
		return base;

	const QFileInfo info(base);
	const QString stem = info.completeBaseName();
	const QString suffix = info.suffix();
	const QString separator = noSpace ? QStringLiteral("_") : QStringLiteral(" ");

	for (int i = 1; i < 10000; ++i) {
		const QString candidate = QStringLiteral("%1%2(%3).%4").arg(stem, separator).arg(i).arg(suffix);
		if (!dir.exists(candidate))
			return candidate;
	}

	return base;
}

bool encodeImage(const QImage &image, const QString &format, int quality, QByteArray &out, QString &error)
{
	QBuffer buffer(&out);
	if (!buffer.open(QIODevice::WriteOnly)) {
		error = T("Error.EncodeFailed").arg(buffer.errorString());
		return false;
	}

	QImageWriter writer(&buffer, format.toUtf8());
	if (Settings::isLossy(format))
		writer.setQuality(quality);

	if (!writer.write(image)) {
		error = T("Error.EncodeFailed").arg(writer.errorString());
		return false;
	}

	return true;
}

/* Qt only looks for its TLS backend plugin in its library paths. OBS does
 * not ship one on Windows, so the plugin bundles qschannelbackend.dll in a
 * "tls" folder next to its own DLL and registers that folder once. */
void configureQtTlsPluginPath()
{
	static bool configured = false;
	if (configured)
		return;
	configured = true;

	const char *modulePath = obs_get_module_file_name(obs_current_module());
	if (!modulePath || !*modulePath)
		return;

	const QString moduleDir = QFileInfo(QString::fromUtf8(modulePath)).absolutePath();
	if (QFileInfo::exists(QDir(moduleDir).filePath(QStringLiteral("tls")))) {
		QCoreApplication::addLibraryPath(moduleDir);
		obs_log(LOG_INFO, "Registered Qt plugin path '%s'", moduleDir.toUtf8().constData());
	}
}

} // namespace

ScreenshotService::ScreenshotService(QObject *parent) : QObject(parent)
{
	pool.setMaxThreadCount(1);
}

ScreenshotService::~ScreenshotService()
{
	pool.waitForDone();
}

void ScreenshotService::setSettings(const Settings &settings)
{
	settings_ = settings;
	settings_.sanitize();
}

void ScreenshotService::requestCapture()
{
	++pending;
	startNextCapture();
}

void ScreenshotService::startNextCapture()
{
	if (busy || pending <= 0)
		return;

	--pending;
	busy = true;

	auto *capture = new ScreenshotCapture(nullptr, this);
	connect(capture, &ScreenshotCapture::ready, this, &ScreenshotService::onCaptureReady);
	connect(capture, &ScreenshotCapture::failed, this, &ScreenshotService::onCaptureFailed);
	connect(capture, &ScreenshotCapture::ready, capture, &QObject::deleteLater);
	connect(capture, &ScreenshotCapture::failed, capture, &QObject::deleteLater);
	capture->start();
}

void ScreenshotService::onCaptureFailed(const QString &reason)
{
	notifyFailure(T("Error.CaptureFailed").arg(reason));

	busy = false;
	startNextCapture();
}

void ScreenshotService::onCaptureReady(const QImage &image)
{
	auto job = std::make_shared<Job>();
	job->settings = settings_;
	job->image = image;

	if (!job->settings.hasAnyOutput()) {
		job->error = T("Error.NothingToDo");
		complete(job);
		return;
	}

	if (job->settings.saveLocal && !prepareOutputPath(*job)) {
		complete(job);
		return;
	}

	if (job->settings.copyToClipboard) {
		QGuiApplication::clipboard()->setImage(image);
		job->actions << T("Status.CopiedToClipboard");
	}

	QPointer<ScreenshotService> self(this);
	pool.start([self, job]() {
		encode(job);
		if (self) {
			QMetaObject::invokeMethod(self, [self, job]() { self->onEncoded(job); }, Qt::QueuedConnection);
		}
	});
}

bool ScreenshotService::prepareOutputPath(Job &job)
{
	const QString folder = job.settings.useRecordingFolder ? recordingFolder() : job.settings.savePath;
	if (folder.trimmed().isEmpty()) {
		job.error = job.settings.useRecordingFolder ? T("Error.RecordingFolderUnavailable")
							    : T("Warn.ChooseFolder");
		return false;
	}

	QDir dir(folder);
	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
		job.error = T("Warn.CouldNotCreateFolder").arg(QDir::toNativeSeparators(dir.absolutePath()));
		return false;
	}

	job.filePath = dir.filePath(buildFileName(job.settings.filenamePrefix, job.settings.format, dir));
	return true;
}

/* Worker thread: only touches the job. */
void ScreenshotService::encode(const JobPtr &job)
{
	const Settings &s = job->settings;

	if (s.saveLocal) {
		if (!encodeImage(job->image, s.format, s.quality, job->fileBytes, job->error))
			return;

		QSaveFile file(job->filePath);
		if (!file.open(QIODevice::WriteOnly) || file.write(job->fileBytes) != job->fileBytes.size() ||
		    !file.commit()) {
			job->error = T("Error.LocalSaveFailed").arg(file.errorString());
			return;
		}
	}

	if (s.wantsWebhook()) {
		const QString webhookFormat = s.effectiveWebhookFormat();

		if (s.saveLocal && webhookFormat == s.format) {
			job->webhookBytes = job->fileBytes;
		} else if (!encodeImage(job->image, webhookFormat, s.quality, job->webhookBytes, job->error)) {
			return;
		}

		const QString stem = job->filePath.isEmpty() ? QStringLiteral("screenshot")
							     : QFileInfo(job->filePath).completeBaseName();
		job->webhookFileName = QStringLiteral("%1.%2").arg(stem, webhookFormat);
	}

	/* The pixels are no longer needed; release them before the job travels back. */
	job->image = QImage();
}

void ScreenshotService::onEncoded(const JobPtr &job)
{
	if (!job->error.isEmpty()) {
		complete(job);
		return;
	}

	if (job->settings.saveLocal)
		job->actions << T("Status.SavedTo").arg(QDir::toNativeSeparators(job->filePath));

	if (job->settings.wantsWebhook())
		upload(job);
	else
		complete(job);
}

void ScreenshotService::upload(const JobPtr &job)
{
	const QUrl url(job->settings.webhookUrl);
	if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
		job->error = T("Error.WebhookUrlInvalid");
		complete(job);
		return;
	}

	if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) {
		job->error = T("Error.WebhookHttpsRequired");
		complete(job);
		return;
	}

	configureQtTlsPluginPath();

	if (!QSslSocket::supportsSsl()) {
		const QStringList backends = QSslSocket::availableBackends();
		job->error = T("Error.TlsUnavailable")
				     .arg(backends.isEmpty() ? QStringLiteral("(none)")
							     : backends.join(QStringLiteral(", ")));
		complete(job);
		return;
	}

	if (!network)
		network = new QNetworkAccessManager(this);

	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Better Screenshot OBS Plugin"));
	request.setTransferTimeout(30000);

	QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
	sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
	request.setSslConfiguration(sslConfig);

	auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

	QJsonObject payload;
	payload[QStringLiteral("content")] = job->settings.webhookMessage;

	QHttpPart payloadPart;
	payloadPart.setHeader(QNetworkRequest::ContentDispositionHeader,
			      QStringLiteral("form-data; name=\"payload_json\""));
	payloadPart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	payloadPart.setBody(QJsonDocument(payload).toJson(QJsonDocument::Compact));
	multiPart->append(payloadPart);

	QHttpPart filePart;
	filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
	filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
			   QStringLiteral("form-data; name=\"file[0]\"; filename=\"%1\"").arg(job->webhookFileName));
	filePart.setBody(job->webhookBytes);
	multiPart->append(filePart);

	QNetworkReply *reply = network->post(request, multiPart);
	multiPart->setParent(reply);

	auto sslErrors = std::make_shared<QStringList>();
	connect(reply, &QNetworkReply::sslErrors, this, [sslErrors](const QList<QSslError> &errors) {
		for (const QSslError &err : errors)
			*sslErrors << err.errorString();
	});

	connect(reply, &QNetworkReply::finished, this, [this, job, reply, sslErrors]() {
		reply->deleteLater();

		const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		const int status = statusVar.isValid() ? statusVar.toInt() : 0;

		if (reply->error() != QNetworkReply::NoError || status >= 300) {
			QString error = reply->errorString();
			if (error.isEmpty() && status > 0)
				error = T("Error.WebhookHttpStatus").arg(status);
			if (!sslErrors->isEmpty())
				error += QStringLiteral(" SSL: %1").arg(sslErrors->join(QStringLiteral(" | ")));

			const QByteArray body = reply->readAll();
			if (!body.isEmpty())
				error += QStringLiteral(" Response: %1").arg(QString::fromUtf8(body));

			job->error = T("Error.WebhookUploadFailed").arg(error);
		} else {
			job->actions << T("Status.SentToDiscord").arg(job->settings.effectiveWebhookFormat().toUpper());
		}

		complete(job);
	});
}

void ScreenshotService::complete(const JobPtr &job)
{
	if (!job->error.isEmpty()) {
		/* Report what did work before the failure, e.g. a saved file. */
		QString message = job->error;
		if (!job->actions.isEmpty())
			message +=
				QStringLiteral("\n") + T("Status.Screenshot").arg(job->actions.join(T("Status.Join")));

		notifyFailure(message);
	} else {
		obs_log(LOG_INFO, "%s",
			T("Status.Screenshot").arg(job->actions.join(T("Status.Join"))).toUtf8().constData());
	}

	busy = false;
	startNextCapture();
}

void ScreenshotService::notifyFailure(const QString &message)
{
	obs_log(LOG_WARNING, "%s", message.toUtf8().constData());

	auto *box = new QMessageBox(QMessageBox::Warning, T("PluginName"), message, QMessageBox::Ok, mainWindow());
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->setModal(false);
	box->show();
}

} // namespace BetterScreenshot
