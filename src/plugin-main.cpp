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
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QCoreApplication>
#include <QFileInfo>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

struct BetterScreenshotSettings {
	QString format = "png";
	bool saveLocal = true;
	bool deleteOriginalAfterSave = false;
	QString savePath;
	QString webhookUrl;
	QString webhookMessage;
	QString hotkey = "Ctrl+Shift+S";
};

static BetterScreenshotSettings g_settings;
static QPointer<QShortcut> g_shortcut;
static QPointer<QNetworkAccessManager> g_network;
static bool g_loaded_once = false;

static bool g_screenshotCompleted = false;
static QString g_lastScreenshotPath;

static const char *CONFIG_ROOT = "better_screenshot";
static const char *KEY_FORMAT = "format";
static const char *KEY_SAVE_LOCAL = "save_local";
static const char *KEY_DELETE_ORIGINAL_AFTER_SAVE = "delete_original_after_save";
static const char *KEY_SAVE_PATH = "save_path";
static const char *KEY_WEBHOOK_URL = "webhook_url";
static const char *KEY_WEBHOOK_MESSAGE = "webhook_message";
static const char *KEY_HOTKEY = "hotkey";

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

static bool delete_original_screenshot_file(const QString &originalPath, const QString &newFilePath, QString &error)
{
	const QString originalClean = QFileInfo(originalPath).absoluteFilePath();
	const QString newClean = newFilePath.trimmed().isEmpty() ? QString()
								 : QFileInfo(newFilePath).absoluteFilePath();

	if (originalClean.isEmpty()) {
		error = "Original OBS screenshot path is empty.";
		return false;
	}

	if (!newClean.isEmpty() && originalClean.compare(newClean, Qt::CaseInsensitive) == 0) {
		error = "Refusing to delete the output file because it matches the original screenshot path.";
		return false;
	}

	QFile originalFile(originalClean);
	if (!originalFile.exists()) {
		error = QString("Original OBS screenshot was not found: %1")
				.arg(QDir::toNativeSeparators(originalClean));
		return false;
	}

	if (!originalFile.remove()) {
		error = QString("Could not delete original OBS screenshot: %1")
				.arg(QDir::toNativeSeparators(originalClean));
		return false;
	}

	return true;
}

static bool send_to_discord_webhook(const QByteArray &imageBytes, const QString &filename, const QString &message,
				    QString &error)
{
	if (!g_network)
		g_network = new QNetworkAccessManager(get_main_window_widget());

	const QUrl url(g_settings.webhookUrl.trimmed());
	if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
		error = "The Discord webhook URL is not valid.";
		return false;
	}

	if (url.scheme().toLower() != "https") {
		error = "Discord webhook URL must use HTTPS.";
		return false;
	}

	if (!QSslSocket::supportsSsl()) {
		QStringList backends;
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
		backends = QSslSocket::availableBackends();
#endif
		error = QString("TLS initialization failed. Qt SSL backend is not available. Available backends: %1")
				.arg(backends.isEmpty() ? QString("(none)") : backends.join(", "));
		return false;
	}

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
		QByteArray encoded;
		QString encodeError;
		if (!encode_image(image, g_settings.format, encoded, encodeError)) {
			resultMessage = QString("Webhook upload encoding failed: %1").arg(encodeError);
			return false;
		}

		QString webhookError;
		if (!send_to_discord_webhook(encoded, fileName.isEmpty() ? QString("screenshot.%1").arg(ext) : fileName,
					     g_settings.webhookMessage, webhookError)) {
			resultMessage = QString("Discord webhook upload failed: %1").arg(webhookError);
			return false;
		}

		didSomething = true;
		actions << "sent to Discord webhook";
	}

	if (!didSomething) {
		resultMessage = "Nothing to do. Enable local save and/or enter a Discord webhook URL.";
		return false;
	}

	if (g_settings.deleteOriginalAfterSave) {
		QString deleteError;
		if (!delete_original_screenshot_file(g_lastScreenshotPath, g_settings.saveLocal ? filePath : QString(),
						     deleteError)) {
			resultMessage = QString("Screenshot processed, but cleanup failed: %1").arg(deleteError);
			return false;
		}

		actions << "deleted original OBS PNG";
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

static void rebuild_shortcut()
{
	if (g_shortcut) {
		delete g_shortcut;
		g_shortcut = nullptr;
	}

	const QString hotkeyText = g_settings.hotkey.trimmed();
	if (hotkeyText.isEmpty())
		return;

	QWidget *mainWindow = get_main_window_widget();
	if (!mainWindow)
		return;

	g_shortcut = new QShortcut(QKeySequence::fromString(hotkeyText, QKeySequence::PortableText), mainWindow);
	g_shortcut->setContext(Qt::ApplicationShortcut);

	QObject::connect(g_shortcut, &QShortcut::activated, []() { capture_screenshot_now(); });

	obs_log(LOG_INFO, "Better Screenshot hotkey set to %s", hotkeyText.toUtf8().constData());
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
		obs_data_set_string(obj, KEY_HOTKEY, g_settings.hotkey.toUtf8().constData());

		obs_data_set_obj(save_data, CONFIG_ROOT, obj);
		obs_data_release(obj);
		return;
	}

	obs_data_t *obj = obs_data_get_obj(save_data, CONFIG_ROOT);
	if (!obj) {
		if (!g_loaded_once) {
			g_settings.format = "png";
			g_settings.saveLocal = true;
			g_settings.deleteOriginalAfterSave = false;
			g_settings.savePath = default_save_path();
			g_settings.webhookUrl.clear();
			g_settings.webhookMessage.clear();
			g_settings.hotkey = "Ctrl+Shift+S";

			rebuild_shortcut();
			g_loaded_once = true;
		}
		return;
	}

	const char *format = obs_data_get_string(obj, KEY_FORMAT);
	const char *savePath = obs_data_get_string(obj, KEY_SAVE_PATH);
	const char *webhookUrl = obs_data_get_string(obj, KEY_WEBHOOK_URL);
	const char *webhookMessage = obs_data_get_string(obj, KEY_WEBHOOK_MESSAGE);
	const char *hotkey = obs_data_get_string(obj, KEY_HOTKEY);

	g_settings.format = (format && *format) ? QString::fromUtf8(format) : "png";
	g_settings.saveLocal = obs_data_get_bool(obj, KEY_SAVE_LOCAL);
	g_settings.deleteOriginalAfterSave = obs_data_get_bool(obj, KEY_DELETE_ORIGINAL_AFTER_SAVE);
	g_settings.savePath = (savePath && *savePath) ? QString::fromUtf8(savePath) : default_save_path();
	g_settings.webhookUrl = webhookUrl ? QString::fromUtf8(webhookUrl) : QString();
	g_settings.webhookMessage = webhookMessage ? QString::fromUtf8(webhookMessage) : QString();
	g_settings.hotkey = (hotkey && *hotkey) ? QString::fromUtf8(hotkey) : "Ctrl+Shift+S";

	if (g_settings.format.isEmpty())
		g_settings.format = "png";

	if (g_settings.savePath.isEmpty())
		g_settings.savePath = default_save_path();

	if (g_settings.hotkey.isEmpty())
		g_settings.hotkey = "Ctrl+Shift+S";

	rebuild_shortcut();
	g_loaded_once = true;

	obs_data_release(obj);
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
	dialog.resize(640, 420);

	auto *rootLayout = new QVBoxLayout(&dialog);

	auto *captureGroup = new QGroupBox("Capture");
	auto *captureLayout = new QFormLayout(captureGroup);

	auto *hotkeyEdit =
		new QKeySequenceEdit(QKeySequence::fromString(g_settings.hotkey, QKeySequence::PortableText));
	hotkeyEdit->setClearButtonEnabled(true);

	auto *formatCombo = new QComboBox();
	formatCombo->addItem("png");
	formatCombo->addItem("jpg");
	formatCombo->addItem("webp");
	{
		const int idx = formatCombo->findText(g_settings.format, Qt::MatchFixedString);
		formatCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}

	captureLayout->addRow("Hotkey", hotkeyEdit);
	captureLayout->addRow("Image format", formatCombo);

	auto *localGroup = new QGroupBox("Local save");
	auto *localLayout = new QGridLayout(localGroup);

	auto *saveLocalCheck = new QCheckBox("Save image locally");
	saveLocalCheck->setChecked(g_settings.saveLocal);

	auto *deleteOriginalCheck = new QCheckBox("Delete original image after save");
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

	auto *messageEdit = new QPlainTextEdit(g_settings.webhookMessage);
	messageEdit->setPlaceholderText("Optional message to send with the screenshot");

	discordLayout->addRow("Webhook URL", webhookEdit);
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
		[&dialog, hotkeyEdit, formatCombo, saveLocalCheck, deleteOriginalCheck, pathEdit, webhookEdit,
		 messageEdit]() {
			const QString selectedFormat = formatCombo->currentText().trimmed().toLower();
			const QString selectedPath = pathEdit->text().trimmed();
			const QString selectedWebhook = webhookEdit->text().trimmed();

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

			g_settings.hotkey = hotkeyEdit->keySequence().toString(QKeySequence::PortableText).trimmed();
			g_settings.format = selectedFormat.isEmpty() ? QString("png") : selectedFormat;
			g_settings.saveLocal = saveLocalCheck->isChecked();
			g_settings.deleteOriginalAfterSave = saveLocalCheck->isChecked() &&
							     deleteOriginalCheck->isChecked();
			g_settings.savePath = selectedPath.isEmpty() ? default_save_path() : selectedPath;
			g_settings.webhookUrl = selectedWebhook;
			g_settings.webhookMessage = messageEdit->toPlainText();

			rebuild_shortcut();
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

static void setup_qt_plugin_paths()
{
	const QString appDir = QCoreApplication::applicationDirPath();

	const char *modulePathRaw = obs_get_module_file_name(obs_current_module());
	QString modulePath;
	QString moduleDir;

	if (modulePathRaw && *modulePathRaw) {
		modulePath = QString::fromUtf8(modulePathRaw);
		moduleDir = QFileInfo(modulePath).absolutePath();
	}

	obs_log(LOG_INFO, "Better Screenshot app dir: %s", appDir.toUtf8().constData());
	obs_log(LOG_INFO, "Better Screenshot module path: %s",
		modulePath.isEmpty() ? "(empty)" : modulePath.toUtf8().constData());
	obs_log(LOG_INFO, "Better Screenshot module dir: %s",
		moduleDir.isEmpty() ? "(empty)" : moduleDir.toUtf8().constData());

	const QString appTlsDir = QDir(appDir).filePath("tls");
	const QString moduleTlsDir = moduleDir.isEmpty() ? QString() : QDir(moduleDir).filePath("tls");

	if (QDir(appTlsDir).exists()) {
		QCoreApplication::addLibraryPath(appDir);
		obs_log(LOG_INFO, "Added Qt library path from app dir: %s", appDir.toUtf8().constData());
	} else {
		obs_log(LOG_WARNING, "App tls folder not found: %s", appTlsDir.toUtf8().constData());
	}

	if (!moduleDir.isEmpty() && QDir(moduleTlsDir).exists()) {
		QCoreApplication::addLibraryPath(moduleDir);
		obs_log(LOG_INFO, "Added Qt library path from module dir: %s", moduleDir.toUtf8().constData());
	} else if (!moduleDir.isEmpty()) {
		obs_log(LOG_WARNING, "Module tls folder not found: %s", moduleTlsDir.toUtf8().constData());
	}

	const QStringList paths = QCoreApplication::libraryPaths();
	for (const QString &path : paths)
		obs_log(LOG_INFO, "Qt library path: %s", path.toUtf8().constData());
}

bool obs_module_load(void)
{
	setup_qt_plugin_paths();

	if (g_settings.savePath.isEmpty())
		g_settings.savePath = default_save_path();

	obs_frontend_add_tools_menu_item("Better Screenshot", show_settings_dialog, nullptr);
	obs_frontend_add_save_callback(settings_save_load_callback, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	rebuild_shortcut();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(settings_save_load_callback, nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (g_shortcut) {
		delete g_shortcut;
		g_shortcut = nullptr;
	}

	if (g_network) {
		delete g_network;
		g_network = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
