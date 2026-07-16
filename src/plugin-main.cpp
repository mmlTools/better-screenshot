/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include "plugin-support.h"

#include <util/platform.h>
#include <util/bmem.h>

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

struct BetterScreenshotSettings {
	QString format = "png";
	bool saveLocal = true;
	bool deleteOriginalAfterSave = false;
	QString savePath;
	QString webhookUrl;
	QString webhookMessage;
	bool webhookUseCustomFormat = false;
	QString webhookFormat = "jpg";
};

static BetterScreenshotSettings g_settings;
static QPointer<QNetworkAccessManager> g_network;
static QPointer<QObject> g_asyncContext;

static bool g_screenshotCompleted = false;
static QString g_lastScreenshotPath;
static bool g_captureInProgress = false;
static bool g_captureScheduled = false;
static int g_pendingCaptures = 0;

static obs_hotkey_id g_captureHotkeyId = OBS_INVALID_HOTKEY_ID;

static const char *CONFIG_ROOT = "better_screenshot";
static const char *KEY_FORMAT = "format";
static const char *KEY_SAVE_LOCAL = "save_local";
static const char *KEY_DELETE_ORIGINAL_AFTER_SAVE = "delete_original_after_save";
static const char *KEY_SAVE_PATH = "save_path";
static const char *KEY_WEBHOOK_URL = "webhook_url";
static const char *KEY_WEBHOOK_MESSAGE = "webhook_message";
static const char *KEY_WEBHOOK_USE_CUSTOM_FORMAT = "webhook_use_custom_format";
static const char *KEY_WEBHOOK_FORMAT = "webhook_format";
static const char *KEY_CAPTURE_HOTKEY = "capture_hotkey";

static void configure_qt_tls_plugin_paths();

static QWidget *get_main_window_widget()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

static QString default_save_path()
{
	QString path = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (path.isEmpty())
		path = QDir::homePath();

	return QDir(path).filePath("OBS Better Screenshot");
}

static bool format_supported(const QString &fmt)
{
	const auto formats = QImageWriter::supportedImageFormats();
	const QByteArray wanted = fmt.trimmed().toLower().toUtf8();

	for (const QByteArray &item : formats) {
		if (item.toLower() == wanted)
			return true;
	}

	return false;
}

static QString normalized_extension(const QString &fmt)
{
	const QString f = fmt.trimmed().toLower();

	if (f == "jpg" || f == "jpeg")
		return "jpg";
	if (f == "png")
		return "png";
	if (f == "webp")
		return "webp";

	return "png";
}

static QString build_output_file_path()
{
	QString dir = g_settings.savePath.trimmed();
	if (dir.isEmpty())
		dir = default_save_path();

	const QString ext = normalized_extension(g_settings.format);
	const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
	const QString filename = QString("BetterScreenshot_%1.%2").arg(timestamp, ext);

	return QDir(dir).filePath(filename);
}

static bool encode_image(const QImage &image, const QString &format, QByteArray &encoded, QString &error)
{
	QBuffer buffer(&encoded);
	if (!buffer.open(QIODevice::WriteOnly)) {
		error = "Could not open memory buffer for image encoding.";
		return false;
	}

	QImageWriter writer(&buffer, format.toUtf8());
	writer.setQuality(95);

	if (!writer.write(image)) {
		error = writer.errorString().isEmpty() ? "Image encoding failed." : writer.errorString();
		return false;
	}

	return true;
}

static bool ensure_output_folder_exists(QString &error)
{
	QString dirPath = g_settings.savePath.trimmed();
	if (dirPath.isEmpty())
		dirPath = default_save_path();

	QDir dir(dirPath);
	if (dir.exists())
		return true;

	if (!dir.mkpath(".")) {
		error = QString("Could not create folder: %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
		return false;
	}

	return true;
}

static bool save_image_locally(const QImage &image, const QString &filePath, const QString &format, QString &error)
{
	QFileInfo info(filePath);
	QDir dir = info.dir();

	if (!dir.exists() && !dir.mkpath(".")) {
		error = QString("Could not create folder: %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
		return false;
	}

	QImageWriter writer(filePath, format.toUtf8());
	writer.setQuality(95);

	if (!writer.write(image)) {
		error = writer.errorString().isEmpty() ? "Failed to save image locally." : writer.errorString();
		return false;
	}

	return true;
}

static bool validate_original_screenshot_cleanup(const QString &originalPath, const QString &newFilePath,
						 QString &originalClean, QString &error)
{
	if (originalPath.trimmed().isEmpty()) {
		error = "Original OBS screenshot path is empty.";
		return false;
	}

	originalClean = QFileInfo(originalPath).absoluteFilePath();
	const QString newClean = newFilePath.trimmed().isEmpty() ? QString()
								 : QFileInfo(newFilePath).absoluteFilePath();

	if (!newClean.isEmpty() && originalClean.compare(newClean, Qt::CaseInsensitive) == 0) {
		error = "Refusing to delete the output file because it matches the original screenshot path.";
		return false;
	}

	return true;
}

static void attempt_original_screenshot_cleanup(const QString &originalPath, int attempt)
{
	QFile originalFile(originalPath);
	if (!originalFile.exists()) {
		obs_log(LOG_INFO, "Original OBS screenshot no longer exists: %s",
			originalPath.toUtf8().constData());
		return;
	}

	if (originalFile.remove()) {
		obs_log(LOG_INFO, "Deleted original OBS screenshot: %s", originalPath.toUtf8().constData());
		return;
	}

	constexpr int maxAttempts = 8;
	if (attempt >= maxAttempts || !g_asyncContext) {
		obs_log(LOG_WARNING, "Could not delete original OBS screenshot after %d attempts: %s",
			attempt, originalPath.toUtf8().constData());
		return;
	}

	const int retryDelayMs = qMin(250 * attempt, 2000);
	QTimer::singleShot(retryDelayMs, g_asyncContext, [originalPath, attempt]() {
		attempt_original_screenshot_cleanup(originalPath, attempt + 1);
	});
}

static bool schedule_original_screenshot_cleanup(const QString &originalPath, const QString &newFilePath,
						 QString &error)
{
	QString originalClean;
	if (!validate_original_screenshot_cleanup(originalPath, newFilePath, originalClean, error))
		return false;

	if (!g_asyncContext) {
		error = "Screenshot cleanup service is not available.";
		return false;
	}

	QTimer::singleShot(250, g_asyncContext,
			   [originalClean]() { attempt_original_screenshot_cleanup(originalClean, 1); });
	return true;
}

static bool send_to_discord_webhook(const QByteArray &imageBytes, const QString &filename, const QString &message,
				    QString &error)
{
	const QUrl url(g_settings.webhookUrl.trimmed());
	if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
		error = "The Discord webhook URL is not valid.";
		return false;
	}

	if (url.scheme().toLower() != "https") {
		error = "Discord webhook URL must use HTTPS.";
		return false;
	}

	configure_qt_tls_plugin_paths();

	if (!QSslSocket::supportsSsl()) {
		QStringList backends;
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
		backends = QSslSocket::availableBackends();
#endif
		error = QString("TLS initialization failed. Qt SSL backend is not available. Available backends: %1")
				.arg(backends.isEmpty() ? QString("(none)") : backends.join(", "));
		return false;
	}

	if (!g_network)
		g_network = new QNetworkAccessManager(get_main_window_widget());

	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, QVariant("Better Screenshot OBS Plugin"));
	request.setTransferTimeout(30000);

	QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
	sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
	request.setSslConfiguration(sslConfig);

	auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

	QJsonObject payload;
	payload["content"] = message;

	QHttpPart payloadPart;
	payloadPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"payload_json\""));
	payloadPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/json"));
	payloadPart.setBody(QJsonDocument(payload).toJson(QJsonDocument::Compact));
	multiPart->append(payloadPart);

	QHttpPart filePart;
	filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
	filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
			   QVariant(QString("form-data; name=\"file[0]\"; filename=\"%1\"").arg(filename)));
	filePart.setBody(imageBytes);
	multiPart->append(filePart);

	QNetworkReply *reply = g_network->post(request, multiPart);
	multiPart->setParent(reply);

	QString sslErrorsText;

	QObject::connect(reply, &QNetworkReply::sslErrors, [reply, &sslErrorsText](const QList<QSslError> &errors) {
		Q_UNUSED(reply);

		QStringList parts;
		for (const QSslError &err : errors)
			parts << err.errorString();

		sslErrorsText = parts.join(" | ");
	});

	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();

	const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
	const int status = statusVar.isValid() ? statusVar.toInt() : 0;
	const QByteArray responseBody = reply->readAll();

	if (reply->error() != QNetworkReply::NoError || status >= 300) {
		error = reply->errorString();

		if (!sslErrorsText.isEmpty())
			error += QString(" SSL: %1").arg(sslErrorsText);

		if (error.isEmpty() && status > 0)
			error = QString("Discord webhook returned HTTP %1.").arg(status);

		if (!responseBody.isEmpty())
			error += QString(" Response: %1").arg(QString::fromUtf8(responseBody));

		reply->deleteLater();
		return false;
	}

	reply->deleteLater();
	return true;
}

static void show_error(const QString &title, const QString &message)
{
	QWidget *parent = get_main_window_widget();
	QMessageBox::critical(parent, title, message);
}

static bool capture_obs_image(QImage &image, QString &error)
{
	g_lastScreenshotPath.clear();
	g_screenshotCompleted = false;

	obs_frontend_take_screenshot();

	QElapsedTimer timer;
	timer.start();

	while (!g_screenshotCompleted && timer.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(10);
	}

	if (!g_screenshotCompleted) {
		error = "OBS did not finish creating the screenshot within the timeout.";
		return false;
	}

	if (g_lastScreenshotPath.trimmed().isEmpty()) {
		error = "OBS reported screenshot completion but did not return a screenshot path.";
		return false;
	}

	image = QImage(g_lastScreenshotPath);
	if (image.isNull()) {
		error = QString("OBS created a screenshot, but it could not be loaded: %1")
				.arg(QDir::toNativeSeparators(g_lastScreenshotPath));
		return false;
	}

	return true;
}

static bool take_and_process_screenshot(QString &resultMessage)
{
	if (!format_supported(g_settings.format)) {
		resultMessage = QString("The selected image format \"%1\" is not supported by this OBS/Qt build.")
					.arg(g_settings.format);
		return false;
	}

	if (g_settings.saveLocal) {
		QString folderError;
		if (!ensure_output_folder_exists(folderError)) {
			resultMessage = folderError;
			return false;
		}
	}

	QImage image;
	QString captureError;
	if (!capture_obs_image(image, captureError)) {
		resultMessage = captureError;
		return false;
	}

	const QString ext = normalized_extension(g_settings.format);
	const QString filePath = build_output_file_path();
	const QString fileName = QFileInfo(filePath).fileName();

	bool didSomething = false;
	QStringList actions;

	if (g_settings.saveLocal) {
		QString saveError;
		if (!save_image_locally(image, filePath, g_settings.format, saveError)) {
			resultMessage = QString("Local save failed: %1").arg(saveError);
			return false;
		}

		didSomething = true;
		actions << QString("saved to %1").arg(QDir::toNativeSeparators(filePath));
	}

	if (!g_settings.webhookUrl.trimmed().isEmpty()) {
		const QString webhookFormat =
			(g_settings.webhookUseCustomFormat && !g_settings.webhookFormat.trimmed().isEmpty())
				? g_settings.webhookFormat.trimmed().toLower()
				: g_settings.format.trimmed().toLower();

		if (!format_supported(webhookFormat)) {
			resultMessage = QString("The selected webhook image format \"%1\" is not supported.")
						.arg(webhookFormat);
			return false;
		}

		const QString webhookExt = normalized_extension(webhookFormat);
		const QString baseName = QFileInfo(filePath).completeBaseName();
		const QString webhookFileName =
			QString("%1.%2").arg(baseName.isEmpty() ? QString("screenshot") : baseName, webhookExt);

		QByteArray encoded;
		QString encodeError;
		if (!encode_image(image, webhookFormat, encoded, encodeError)) {
			resultMessage = QString("Webhook upload encoding failed: %1").arg(encodeError);
			return false;
	}

		QString webhookError;
		if (!send_to_discord_webhook(encoded, webhookFileName, g_settings.webhookMessage, webhookError)) {
			resultMessage = QString("Discord webhook upload failed: %1").arg(webhookError);
			return false;
	}

		didSomething = true;
		actions << QString("sent to Discord webhook as %1").arg(webhookExt.toUpper());
	}

	if (!didSomething) {
		resultMessage = "Nothing to do. Enable local save and/or enter a Discord webhook URL.";
		return false;
	}

	if (g_settings.deleteOriginalAfterSave) {
		QString deleteError;
		if (!schedule_original_screenshot_cleanup(g_lastScreenshotPath,
							 g_settings.saveLocal ? filePath : QString(), deleteError)) {
			resultMessage = QString("Screenshot processed, but cleanup failed: %1").arg(deleteError);
			return false;
		}

		actions << "queued original OBS PNG for cleanup";
	}

	resultMessage = QString("Screenshot %1.").arg(actions.join(" and "));
	return true;
}

static void capture_screenshot_now()
{
	QString result;
	if (take_and_process_screenshot(result)) {
		obs_log(LOG_INFO, "%s", result.toUtf8().constData());
	} else {
		obs_log(LOG_WARNING, "%s", result.toUtf8().constData());
		show_error("Better Screenshot", result);
	}
}

static void process_next_capture()
{
	g_captureScheduled = false;

	if (g_captureInProgress || g_pendingCaptures <= 0)
		return;

	--g_pendingCaptures;
	g_captureInProgress = true;
	capture_screenshot_now();
	g_captureInProgress = false;

	if (g_pendingCaptures > 0 && g_asyncContext) {
		g_captureScheduled = true;
		QTimer::singleShot(0, g_asyncContext, []() { process_next_capture(); });
	}
}

static void queue_capture_screenshot()
{
	++g_pendingCaptures;

	if (g_captureInProgress || g_captureScheduled)
		return;

	if (!g_asyncContext) {
		--g_pendingCaptures;
		obs_log(LOG_WARNING, "Cannot capture screenshot because the plugin task context is unavailable.");
		return;
	}

	g_captureScheduled = true;
	QTimer::singleShot(0, g_asyncContext, []() { process_next_capture(); });
}

static void capture_hotkey_callback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	queue_capture_screenshot();
}

static void save_capture_hotkey(obs_data_t *save_data)
{
	if (g_captureHotkeyId == OBS_INVALID_HOTKEY_ID)
		return;

	obs_data_array_t *hotkeyArray = obs_hotkey_save(g_captureHotkeyId);
	if (hotkeyArray) {
		obs_data_set_array(save_data, KEY_CAPTURE_HOTKEY, hotkeyArray);
		obs_data_array_release(hotkeyArray);
	}
}

static void load_capture_hotkey(obs_data_t *save_data)
{
	if (g_captureHotkeyId == OBS_INVALID_HOTKEY_ID)
		return;

	obs_data_array_t *hotkeyArray = obs_data_get_array(save_data, KEY_CAPTURE_HOTKEY);
	if (hotkeyArray) {
		obs_hotkey_load(g_captureHotkeyId, hotkeyArray);
		obs_data_array_release(hotkeyArray);
	}
}

static void settings_save_load_callback(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (saving) {
		obs_data_t *obj = obs_data_create();

		obs_data_set_string(obj, KEY_FORMAT, g_settings.format.toUtf8().constData());
		obs_data_set_bool(obj, KEY_SAVE_LOCAL, g_settings.saveLocal);
		obs_data_set_bool(obj, KEY_DELETE_ORIGINAL_AFTER_SAVE, g_settings.deleteOriginalAfterSave);
		obs_data_set_string(obj, KEY_SAVE_PATH, g_settings.savePath.toUtf8().constData());
		obs_data_set_string(obj, KEY_WEBHOOK_URL, g_settings.webhookUrl.toUtf8().constData());
		obs_data_set_string(obj, KEY_WEBHOOK_MESSAGE, g_settings.webhookMessage.toUtf8().constData());
		obs_data_set_bool(obj, KEY_WEBHOOK_USE_CUSTOM_FORMAT, g_settings.webhookUseCustomFormat);
		obs_data_set_string(obj, KEY_WEBHOOK_FORMAT, g_settings.webhookFormat.toUtf8().constData());

		obs_data_set_obj(save_data, CONFIG_ROOT, obj);
		obs_data_release(obj);

		save_capture_hotkey(save_data);
		return;
	}

	g_settings.format = "png";
	g_settings.saveLocal = true;
	g_settings.deleteOriginalAfterSave = false;
	g_settings.savePath = default_save_path();
	g_settings.webhookUrl.clear();
	g_settings.webhookMessage.clear();
	g_settings.webhookUseCustomFormat = false;
	g_settings.webhookFormat = "jpg";

	obs_data_t *obj = obs_data_get_obj(save_data, CONFIG_ROOT);
	if (obj) {
		const char *format = obs_data_get_string(obj, KEY_FORMAT);
		const char *savePath = obs_data_get_string(obj, KEY_SAVE_PATH);
		const char *webhookUrl = obs_data_get_string(obj, KEY_WEBHOOK_URL);
		const char *webhookMessage = obs_data_get_string(obj, KEY_WEBHOOK_MESSAGE);
		const char *webhookFormat = obs_data_get_string(obj, KEY_WEBHOOK_FORMAT);

		g_settings.format = (format && *format) ? QString::fromUtf8(format) : "png";
		g_settings.saveLocal = obs_data_get_bool(obj, KEY_SAVE_LOCAL);
		g_settings.deleteOriginalAfterSave = obs_data_get_bool(obj, KEY_DELETE_ORIGINAL_AFTER_SAVE);
		g_settings.savePath = (savePath && *savePath) ? QString::fromUtf8(savePath) : default_save_path();
		g_settings.webhookUrl = webhookUrl ? QString::fromUtf8(webhookUrl) : QString();
		g_settings.webhookMessage = webhookMessage ? QString::fromUtf8(webhookMessage) : QString();
		g_settings.webhookUseCustomFormat = obs_data_get_bool(obj, KEY_WEBHOOK_USE_CUSTOM_FORMAT);
		g_settings.webhookFormat = (webhookFormat && *webhookFormat) ? QString::fromUtf8(webhookFormat) : "jpg";

		obs_data_release(obj);
	}

	if (g_settings.format.isEmpty())
		g_settings.format = "png";

	if (g_settings.webhookFormat.isEmpty())
		g_settings.webhookFormat = "jpg";

	if (g_settings.savePath.isEmpty())
		g_settings.savePath = default_save_path();

	load_capture_hotkey(save_data);
}

static void show_settings_dialog(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = get_main_window_widget();
	if (!parent) {
		obs_log(LOG_WARNING, "Better Screenshot could not get the OBS main window.");
		return;
	}

	QDialog dialog(parent);
	dialog.setWindowTitle("Better Screenshot");
	dialog.setModal(true);
	dialog.resize(680, 480);

	auto *rootLayout = new QVBoxLayout(&dialog);

	auto *captureGroup = new QGroupBox("Capture");
	auto *captureLayout = new QFormLayout(captureGroup);

	auto *hotkeyInfo = new QLabel(
		"Background hotkey handling is enabled through OBS hotkeys. Set the shortcut in OBS under <b>File → Settings → Hotkeys</b> using the action "
		"<b>Better Screenshot: Capture Screenshot</b>.<br><br>");
	hotkeyInfo->setWordWrap(true);
	hotkeyInfo->setTextFormat(Qt::RichText);

	auto *formatCombo = new QComboBox();
	formatCombo->addItem("png");
	formatCombo->addItem("jpg");
	formatCombo->addItem("webp");
	{
		const int idx = formatCombo->findText(g_settings.format, Qt::MatchFixedString);
		formatCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}

	captureLayout->addRow(hotkeyInfo);
	captureLayout->addRow("Image format", formatCombo);

	auto *localGroup = new QGroupBox("Local save");
	auto *localLayout = new QGridLayout(localGroup);

	auto *saveLocalCheck = new QCheckBox("Save image locally");
	saveLocalCheck->setChecked(g_settings.saveLocal);

	auto *deleteOriginalCheck = new QCheckBox("Delete original image after save from OBS Recording Path");
	deleteOriginalCheck->setChecked(g_settings.deleteOriginalAfterSave);
	deleteOriginalCheck->setToolTip(
		"Deletes the PNG file generated by OBS after the new image has been processed successfully.");

	auto *pathEdit = new QLineEdit(g_settings.savePath.isEmpty() ? default_save_path() : g_settings.savePath);
	auto *browseButton = new QPushButton("Browse...");

	localLayout->addWidget(saveLocalCheck, 0, 0, 1, 3);
	localLayout->addWidget(deleteOriginalCheck, 1, 0, 1, 3);
	localLayout->addWidget(new QLabel("Folder"), 2, 0);
	localLayout->addWidget(pathEdit, 2, 1);
	localLayout->addWidget(browseButton, 2, 2);

	auto *discordGroup = new QGroupBox("Discord webhook");
	auto *discordLayout = new QFormLayout(discordGroup);

	auto *webhookEdit = new QLineEdit(g_settings.webhookUrl);
	webhookEdit->setPlaceholderText("https://discord.com/api/webhooks/...");

	auto *webhookFormatCheck = new QCheckBox("Use separate webhook image format");
	webhookFormatCheck->setChecked(g_settings.webhookUseCustomFormat);
	webhookFormatCheck->setToolTip(
		"When enabled, Discord uses this format instead of the main local image format.");

	auto *webhookFormatCombo = new QComboBox();
	webhookFormatCombo->addItem("png");
	webhookFormatCombo->addItem("jpg");
	webhookFormatCombo->addItem("webp");
	{
		const int idx = webhookFormatCombo->findText(g_settings.webhookFormat, Qt::MatchFixedString);
		webhookFormatCombo->setCurrentIndex(idx >= 0 ? idx : 1);
	}

	webhookFormatCombo->setEnabled(webhookFormatCheck->isChecked());

	auto *messageEdit = new QPlainTextEdit(g_settings.webhookMessage);
	messageEdit->setPlaceholderText("Optional message to send with the screenshot");

	discordLayout->addRow("Webhook URL", webhookEdit);
	discordLayout->addRow(webhookFormatCheck);
	discordLayout->addRow("Webhook image format", webhookFormatCombo);
	discordLayout->addRow("Message", messageEdit);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

	rootLayout->addWidget(captureGroup);
	rootLayout->addWidget(localGroup);
	rootLayout->addWidget(discordGroup);
	rootLayout->addWidget(buttons);

	auto updateLocalUi = [saveLocalCheck, deleteOriginalCheck, pathEdit, browseButton]() {
		const bool enabled = saveLocalCheck->isChecked();
		pathEdit->setEnabled(enabled);
		browseButton->setEnabled(enabled);
		deleteOriginalCheck->setEnabled(enabled);
	};

	updateLocalUi();

	QObject::connect(saveLocalCheck, &QCheckBox::toggled, &dialog, [updateLocalUi]() { updateLocalUi(); });

	QObject::connect(webhookFormatCheck, &QCheckBox::toggled, &dialog,
		 [webhookFormatCombo](bool checked) {
			 webhookFormatCombo->setEnabled(checked);
		 });

	QObject::connect(browseButton, &QPushButton::clicked, &dialog, [&dialog, pathEdit]() {
		QString startDir = pathEdit->text().trimmed();
		if (startDir.isEmpty())
			startDir = default_save_path();

		const QString folder =
			QFileDialog::getExistingDirectory(&dialog, "Select screenshot folder", startDir,
							  QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (!folder.isEmpty())
			pathEdit->setText(folder);
	});

	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	QObject::connect(
		buttons, &QDialogButtonBox::accepted, &dialog,
		[&dialog, formatCombo, saveLocalCheck, deleteOriginalCheck, pathEdit, webhookEdit, webhookFormatCheck, webhookFormatCombo, messageEdit]() {
			const QString selectedFormat = formatCombo->currentText().trimmed().toLower();
			const QString selectedPath = pathEdit->text().trimmed();
			const QString selectedWebhook = webhookEdit->text().trimmed();
			const QString selectedWebhookFormat = webhookFormatCombo->currentText().trimmed().toLower();

			if (saveLocalCheck->isChecked() && selectedPath.isEmpty()) {
				QMessageBox::warning(&dialog, "Better Screenshot", "Please choose a local folder.");
				return;
			}

			if (saveLocalCheck->isChecked()) {
				QString effectivePath = selectedPath.isEmpty() ? default_save_path() : selectedPath;
				QDir dir(effectivePath);
				if (!dir.exists() && !dir.mkpath(".")) {
					QMessageBox::warning(
						&dialog, "Better Screenshot",
						QString("Could not create folder: %1")
							.arg(QDir::toNativeSeparators(dir.absolutePath())));
					return;
				}
			}

			if (!selectedWebhook.isEmpty()) {
				const QUrl url(selectedWebhook);
				if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
					QMessageBox::warning(&dialog, "Better Screenshot",
							     "Please enter a valid Discord webhook URL.");
					return;
				}
			}

			if (webhookFormatCheck->isChecked() && !format_supported(selectedWebhookFormat)) {
				QMessageBox::warning(&dialog, "Better Screenshot",
					QString("The selected webhook image format \"%1\" is not supported.")
				    	.arg(selectedWebhookFormat));
				return;
			}

			g_settings.format = selectedFormat.isEmpty() ? QString("png") : selectedFormat;
			g_settings.saveLocal = saveLocalCheck->isChecked();
			g_settings.deleteOriginalAfterSave = saveLocalCheck->isChecked() &&
							     deleteOriginalCheck->isChecked();
			g_settings.savePath = selectedPath.isEmpty() ? default_save_path() : selectedPath;
			g_settings.webhookUrl = selectedWebhook;
			g_settings.webhookMessage = messageEdit->toPlainText();
			g_settings.webhookUseCustomFormat = webhookFormatCheck->isChecked();
			g_settings.webhookFormat = selectedWebhookFormat.isEmpty() ? QString("jpg") : selectedWebhookFormat;

			obs_frontend_save();
			dialog.accept();
		});

	dialog.exec();
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (event != OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN)
		return;

	char *lastPathRaw = obs_frontend_get_last_screenshot();
	if (lastPathRaw && *lastPathRaw)
		g_lastScreenshotPath = QString::fromUtf8(lastPathRaw);
	else
		g_lastScreenshotPath.clear();

	if (lastPathRaw)
		bfree(lastPathRaw);

	g_screenshotCompleted = true;
}

static void configure_qt_tls_plugin_paths()
{
	static bool configured = false;
	if (configured)
		return;

	configured = true;
	const QString appDir = QCoreApplication::applicationDirPath();

	const char *modulePathRaw = obs_get_module_file_name(obs_current_module());
	QString modulePath;
	QString moduleDir;

	if (modulePathRaw && *modulePathRaw) {
		modulePath = QString::fromUtf8(modulePathRaw);
		moduleDir = QFileInfo(modulePath).absolutePath();
	}

	const QString appTlsDir = QDir(appDir).filePath("tls");
	const QString moduleTlsDir = moduleDir.isEmpty() ? QString() : QDir(moduleDir).filePath("tls");
	const QString appBackend = QDir(appTlsDir).filePath("qschannelbackend.dll");
	const QString moduleBackend =
		moduleTlsDir.isEmpty() ? QString() : QDir(moduleTlsDir).filePath("qschannelbackend.dll");

	if (QFileInfo::exists(appBackend)) {
		QCoreApplication::addLibraryPath(appDir);
		obs_log(LOG_INFO, "Configured Qt TLS backend from app dir: %s", appDir.toUtf8().constData());
	}

	if (!moduleDir.isEmpty() && QFileInfo::exists(moduleBackend)) {
		QCoreApplication::addLibraryPath(moduleDir);
		obs_log(LOG_INFO, "Configured Qt TLS backend from module dir: %s", moduleDir.toUtf8().constData());
	}
}

bool obs_module_load(void)
{
	g_asyncContext = new QObject();

	if (g_settings.savePath.isEmpty())
		g_settings.savePath = default_save_path();

	obs_frontend_add_tools_menu_item("Better Screenshot", show_settings_dialog, nullptr);
	obs_frontend_add_save_callback(settings_save_load_callback, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	g_captureHotkeyId = obs_hotkey_register_frontend(
		"better_screenshot.capture", "Better Screenshot: Capture Screenshot", capture_hotkey_callback, nullptr);

	if (g_captureHotkeyId == OBS_INVALID_HOTKEY_ID) {
		obs_log(LOG_WARNING, "Failed to register Better Screenshot hotkey.");
	} else {
		obs_log(LOG_INFO, "Registered Better Screenshot hotkey id=%llu",
			static_cast<unsigned long long>(g_captureHotkeyId));
	}

	obs_hotkey_enable_background_press(true);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	g_pendingCaptures = 0;
	g_captureScheduled = false;
	g_captureInProgress = false;

	if (g_asyncContext) {
		delete g_asyncContext;
		g_asyncContext = nullptr;
	}

	obs_frontend_remove_save_callback(settings_save_load_callback, nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (g_captureHotkeyId != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_captureHotkeyId);
		g_captureHotkeyId = OBS_INVALID_HOTKEY_ID;
	}

	if (g_network) {
		delete g_network;
		g_network = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
