/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * Recognizer.hh
 * Copyright (C) 2013-2019 Sandro Mani <manisandro@gmail.com>
 *
 * gImageReader is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gImageReader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RECOGNIZER_HPP
#define RECOGNIZER_HPP

#include <QToolButton>
#include <memory>

#include "Config.hh"
#include "Displayer.hh"
#include "ui_PageRangeDialog.h"
#include "ui_CharacterListDialog.h"

namespace tesseract {
class TessBaseAPI;
}
class UI_MainWindow;

class Recognizer : public QObject {
	Q_OBJECT
public:
	enum class OutputDestination { Buffer, Clipboard };

	Recognizer(const UI_MainWindow& _ui);
	QStringList getAvailableLanguages() const;
	const Config::Lang& getSelectedLanguage() const {
		return m_curLang;
	}

public slots:
	bool recognizeImage(const QImage& image, OutputDestination dest);
	void setRecognizeMode(const QString& mode);
	void updateLanguagesMenu();

signals:
	void languageChanged(const Config::Lang& lang);

private:
	struct ProgressMonitor;
	enum class PageSelection { Prompt, Current, Multiple };
	enum class PageArea { EntirePage, Autodetect };
	struct PageData {
		bool success;
		QString filename;
		int page;
		double angle;
		int resolution;
		QList<QImage> ocrAreas;
	};

	const UI_MainWindow& ui;
	QMenu* m_menuPages = nullptr;
	QMenu* m_menuMultilanguage = nullptr;
	QDialog* m_pagesDialog;
	Ui::PageRangeDialog m_pagesDialogUi;
	QDialog* m_charListDialog;
	Ui::CharacterListDialog m_charListDialogUi;
	QActionGroup* m_langMenuRadioGroup = nullptr;
	QActionGroup* m_langMenuCheckGroup = nullptr;
	QActionGroup* m_psmCheckGroup = nullptr;
	QAction* m_multilingualAction = nullptr;
	QString m_modeLabel;
	QString m_langLabel;
	Config::Lang m_curLang;

	std::unique_ptr<tesseract::TessBaseAPI> initTesseract(const char* language = nullptr, bool* ok = nullptr) const;
	QList<int> selectPages(bool& autodetectLayout);
	void recognize(const QList<int>& pages, bool autodetectLayout = false);
	bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
	void clearLineEditPageRangeStyle();
	void manageCharacterLists();
	void psmSelected(QAction* action);
	void recognizeButtonClicked();
	void recognizeCurrentPage();
	void recognizeMultiplePages();
	void setLanguage();
	void setMultiLanguage();
	PageData setPage(int page, bool autodetectLayout);
};

#endif // RECOGNIZER_HPP
