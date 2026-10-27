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

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace BetterScreenshot {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	SettingsDialog(const Settings &settings, QWidget *parent = nullptr);

	/* Only meaningful after exec() returned QDialog::Accepted. */
	Settings result() const;

protected:
	void accept() override;

private:
	void updateEnabledState();
	Settings collect() const;

	QComboBox *formatCombo = nullptr;
	QSpinBox *qualitySpin = nullptr;

	QCheckBox *saveLocalCheck = nullptr;
	QCheckBox *useRecordingFolderCheck = nullptr;
	QLineEdit *pathEdit = nullptr;
	QPushButton *browseButton = nullptr;
	QLineEdit *prefixEdit = nullptr;

	QCheckBox *clipboardCheck = nullptr;

	QLineEdit *webhookEdit = nullptr;
	QCheckBox *webhookFormatCheck = nullptr;
	QComboBox *webhookFormatCombo = nullptr;
	QPlainTextEdit *messageEdit = nullptr;
};

} // namespace BetterScreenshot
