/*
Better Screenshot - An OBS plugin to take screenshots with more control over the output.
Copyright (C) <2026> <@MMLTECH> <https://www.youtube.com/@mmltech> <https://streamrsc.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "settings-dialog.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

namespace BetterScreenshot {

namespace {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool formatSupported(const QString &format)
{
	const QByteArray wanted = format.toUtf8();
	for (const QByteArray &item : QImageWriter::supportedImageFormats()) {
		if (item.compare(wanted, Qt::CaseInsensitive) == 0)
			return true;
	}
	return false;
}

/* Only formats the Qt build in this OBS can actually write are offered. */
QComboBox *createFormatCombo(const QString &current)
{
	auto *combo = new QComboBox();
	for (const char *format : {"png", "jpg", "webp"}) {
		const QString f = QString::fromLatin1(format);
		if (formatSupported(f))
			combo->addItem(f.toUpper(), f);
	}

	const int idx = combo->findData(current);
	combo->setCurrentIndex(idx >= 0 ? idx : 0);
	return combo;
}

QString comboFormat(const QComboBox *combo)
{
	return combo->currentData().toString();
}

} // namespace

SettingsDialog::SettingsDialog(const Settings &settings, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(T("PluginName"));
	setModal(true);
	setMinimumWidth(640);

	auto *rootLayout = new QVBoxLayout(this);

	/* Capture */
	auto *captureGroup = new QGroupBox(T("Dialog.Group.Capture"));
	auto *captureLayout = new QFormLayout(captureGroup);

	auto *hotkeyInfo = new QLabel(T("Dialog.HotkeyInfo"));
	hotkeyInfo->setWordWrap(true);
	hotkeyInfo->setTextFormat(Qt::RichText);

	formatCombo = createFormatCombo(settings.format);

	qualitySpin = new QSpinBox();
	qualitySpin->setRange(1, 100);
	qualitySpin->setValue(settings.quality);
	qualitySpin->setToolTip(T("Dialog.Quality.Tooltip"));

	clipboardCheck = new QCheckBox(T("Dialog.CopyToClipboard"));
	clipboardCheck->setChecked(settings.copyToClipboard);

	captureLayout->addRow(hotkeyInfo);
	captureLayout->addRow(T("Dialog.ImageFormat"), formatCombo);
	captureLayout->addRow(T("Dialog.Quality"), qualitySpin);
	captureLayout->addRow(clipboardCheck);

	/* Local save */
	auto *localGroup = new QGroupBox(T("Dialog.Group.LocalSave"));
	auto *localLayout = new QGridLayout(localGroup);

	saveLocalCheck = new QCheckBox(T("Dialog.SaveLocally"));
	saveLocalCheck->setChecked(settings.saveLocal);

	useRecordingFolderCheck = new QCheckBox(T("Dialog.UseRecordingFolder"));
	useRecordingFolderCheck->setChecked(settings.useRecordingFolder);
	useRecordingFolderCheck->setToolTip(T("Dialog.UseRecordingFolder.Tooltip"));

	pathEdit = new QLineEdit(settings.savePath.isEmpty() ? Settings::defaultSavePath() : settings.savePath);
	browseButton = new QPushButton(T("Dialog.Browse"));

	prefixEdit = new QLineEdit(settings.filenamePrefix);
	prefixEdit->setToolTip(T("Dialog.FilenamePrefix.Tooltip"));

	localLayout->addWidget(saveLocalCheck, 0, 0, 1, 3);
	localLayout->addWidget(useRecordingFolderCheck, 1, 0, 1, 3);
	localLayout->addWidget(new QLabel(T("Dialog.Folder")), 2, 0);
	localLayout->addWidget(pathEdit, 2, 1);
	localLayout->addWidget(browseButton, 2, 2);
	localLayout->addWidget(new QLabel(T("Dialog.FilenamePrefix")), 3, 0);
	localLayout->addWidget(prefixEdit, 3, 1, 1, 2);

	/* Discord */
	auto *discordGroup = new QGroupBox(T("Dialog.Group.Discord"));
	auto *discordLayout = new QFormLayout(discordGroup);

	webhookEdit = new QLineEdit(settings.webhookUrl);
	webhookEdit->setPlaceholderText(QStringLiteral("https://discord.com/api/webhooks/..."));

	webhookFormatCheck = new QCheckBox(T("Dialog.WebhookCustomFormat"));
	webhookFormatCheck->setChecked(settings.webhookUseCustomFormat);
	webhookFormatCheck->setToolTip(T("Dialog.WebhookCustomFormat.Tooltip"));

	webhookFormatCombo = createFormatCombo(settings.webhookFormat);

	messageEdit = new QPlainTextEdit(settings.webhookMessage);
	messageEdit->setPlaceholderText(T("Dialog.Message.Placeholder"));
	messageEdit->setMaximumHeight(80);

	discordLayout->addRow(T("Dialog.WebhookUrl"), webhookEdit);
	discordLayout->addRow(webhookFormatCheck);
	discordLayout->addRow(T("Dialog.WebhookFormat"), webhookFormatCombo);
	discordLayout->addRow(T("Dialog.Message"), messageEdit);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

	rootLayout->addWidget(captureGroup);
	rootLayout->addWidget(localGroup);
	rootLayout->addWidget(discordGroup);
	rootLayout->addWidget(buttons);

	connect(formatCombo, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateEnabledState);
	connect(webhookFormatCombo, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateEnabledState);
	connect(saveLocalCheck, &QCheckBox::toggled, this, &SettingsDialog::updateEnabledState);
	connect(useRecordingFolderCheck, &QCheckBox::toggled, this, &SettingsDialog::updateEnabledState);
	connect(webhookFormatCheck, &QCheckBox::toggled, this, &SettingsDialog::updateEnabledState);
	connect(webhookEdit, &QLineEdit::textChanged, this, &SettingsDialog::updateEnabledState);

	connect(browseButton, &QPushButton::clicked, this, [this]() {
		QString startDir = pathEdit->text().trimmed();
		if (startDir.isEmpty())
			startDir = Settings::defaultSavePath();

		const QString folder =
			QFileDialog::getExistingDirectory(this, T("Dialog.SelectFolderTitle"), startDir,
							  QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (!folder.isEmpty())
			pathEdit->setText(folder);
	});

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	updateEnabledState();
}

void SettingsDialog::updateEnabledState()
{
	const bool webhook = !webhookEdit->text().trimmed().isEmpty();
	const bool customWebhookFormat = webhook && webhookFormatCheck->isChecked();

	const bool lossy = Settings::isLossy(comboFormat(formatCombo)) ||
			   (customWebhookFormat && Settings::isLossy(comboFormat(webhookFormatCombo)));
	qualitySpin->setEnabled(lossy);

	const bool save = saveLocalCheck->isChecked();
	const bool customFolder = save && !useRecordingFolderCheck->isChecked();
	useRecordingFolderCheck->setEnabled(save);
	pathEdit->setEnabled(customFolder);
	browseButton->setEnabled(customFolder);
	prefixEdit->setEnabled(save);

	webhookFormatCheck->setEnabled(webhook);
	webhookFormatCombo->setEnabled(customWebhookFormat);
	messageEdit->setEnabled(webhook);
}

Settings SettingsDialog::collect() const
{
	Settings s;
	s.format = comboFormat(formatCombo);
	s.quality = qualitySpin->value();
	s.saveLocal = saveLocalCheck->isChecked();
	s.useRecordingFolder = useRecordingFolderCheck->isChecked();
	s.savePath = pathEdit->text().trimmed();
	s.filenamePrefix = prefixEdit->text().trimmed();
	s.copyToClipboard = clipboardCheck->isChecked();
	s.webhookUrl = webhookEdit->text().trimmed();
	s.webhookMessage = messageEdit->toPlainText();
	s.webhookUseCustomFormat = webhookFormatCheck->isChecked();
	s.webhookFormat = comboFormat(webhookFormatCombo);
	s.sanitize();
	return s;
}

Settings SettingsDialog::result() const
{
	return collect();
}

void SettingsDialog::accept()
{
	const Settings s = collect();

	if (s.saveLocal && !s.useRecordingFolder) {
		if (pathEdit->text().trimmed().isEmpty()) {
			QMessageBox::warning(this, T("PluginName"), T("Warn.ChooseFolder"));
			return;
		}

		QDir dir(s.savePath);
		if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
			QMessageBox::warning(
				this, T("PluginName"),
				T("Warn.CouldNotCreateFolder").arg(QDir::toNativeSeparators(dir.absolutePath())));
			return;
		}
	}

	if (s.wantsWebhook()) {
		const QUrl url(s.webhookUrl);
		if (!url.isValid() || url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0 ||
		    url.host().isEmpty()) {
			QMessageBox::warning(this, T("PluginName"), T("Warn.InvalidWebhookUrl"));
			return;
		}
	}

	if (!s.hasAnyOutput()) {
		QMessageBox::warning(this, T("PluginName"), T("Error.NothingToDo"));
		return;
	}

	QDialog::accept();
}

} // namespace BetterScreenshot
