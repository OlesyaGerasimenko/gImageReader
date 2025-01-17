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

#include <QClipboard>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QtSpell.hpp>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#define USE_STD_NAMESPACE
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/strngs.h>
#include <tesseract/genericvector.h>
#undef USE_STD_NAMESPACE
#include <QMouseEvent>
#include <unistd.h>
#include <setjmp.h>

#ifdef Q_OS_WIN
#include <fcntl.h>
#define pipe(fds) _pipe(fds, 5000, _O_BINARY)
#endif

#include "ConfigSettings.hh"
#include "Displayer.hh"
#include "MainWindow.hh"
#include "OutputEditor.hh"
#include "Recognizer.hh"
#include "Utils.hh"

class Recognizer::ProgressMonitor : public MainWindow::ProgressMonitor {
public:
	ETEXT_DESC desc;

	ProgressMonitor(int nPages) : MainWindow::ProgressMonitor(nPages) {
		desc.progress = 0;
		desc.cancel = cancelCallback;
		desc.cancel_this = this;
	}
	int getProgress() const override {
		QMutexLocker locker(&mMutex);
		return 100.0 * ((mProgress + desc.progress / 100.0) / mTotal);
	}
	static bool cancelCallback(void* instance, int /*words*/) {
		ProgressMonitor* monitor = reinterpret_cast<ProgressMonitor*>(instance);
		QMutexLocker locker(&monitor->mMutex);
		return monitor->mCancelled;
	}
};


Recognizer::Recognizer(const UI_MainWindow& _ui) :
	ui(_ui) {
	QAction* currentPageAction = new QAction(_("Current Page"), this);
	currentPageAction->setData(static_cast<int>(PageSelection::Current));

	QAction* multiplePagesAction = new QAction(_("Multiple Pages..."), this);
	multiplePagesAction->setData(static_cast<int>(PageSelection::Multiple));

	m_menuPages = new QMenu(ui.toolButtonRecognize);
	m_menuPages->addAction(currentPageAction);
	m_menuPages->addAction(multiplePagesAction);

	m_pagesDialog = new QDialog(MAIN);
	m_pagesDialogUi.setupUi(m_pagesDialog);

	m_charListDialog = new QDialog(MAIN);
	m_charListDialogUi.setupUi(m_charListDialog);

	ui.toolButtonRecognize->setText(QString("%1\n%2").arg(m_modeLabel).arg(m_langLabel));
	ui.menuLanguages->installEventFilter(this);

	connect(ui.toolButtonRecognize, SIGNAL(clicked()), this, SLOT(recognizeButtonClicked()));
	connect(currentPageAction, SIGNAL(triggered()), this, SLOT(recognizeCurrentPage()));
	connect(multiplePagesAction, SIGNAL(triggered()), this, SLOT(recognizeMultiplePages()));
	connect(m_pagesDialogUi.lineEditPageRange, SIGNAL(textChanged(QString)), this, SLOT(clearLineEditPageRangeStyle()));
	connect(m_charListDialogUi.radioButtonBlacklist, SIGNAL(toggled(bool)), m_charListDialogUi.lineEditBlacklist, SLOT(setEnabled(bool)));
	connect(m_charListDialogUi.radioButtonWhitelist, SIGNAL(toggled(bool)), m_charListDialogUi.lineEditWhitelist, SLOT(setEnabled(bool)));

	ADD_SETTING(VarSetting<QString>("language", "eng:en_EN"));
	ADD_SETTING(ComboSetting("ocrregionstrategy", m_pagesDialogUi.comboBoxRecognitionArea, 0));
	ADD_SETTING(SwitchSetting("ocraddsourcefilename", m_pagesDialogUi.checkBoxPrependFilename));
	ADD_SETTING(SwitchSetting("ocraddsourcepage", m_pagesDialogUi.checkBoxPrependPage));
	ADD_SETTING(LineEditSetting("ocrcharwhitelist", m_charListDialogUi.lineEditWhitelist));
	ADD_SETTING(LineEditSetting("ocrcharblacklist", m_charListDialogUi.lineEditBlacklist));
	ADD_SETTING(SwitchSetting("ocrblacklistenabled", m_charListDialogUi.radioButtonBlacklist, true));
	ADD_SETTING(SwitchSetting("ocrwhitelistenabled", m_charListDialogUi.radioButtonWhitelist, false));
	ADD_SETTING(VarSetting<int>("psm", 6));
}

QStringList Recognizer::getAvailableLanguages() const {
	auto tess = initTesseract();
	GenericVector<STRING> availLanguages;
	tess->GetAvailableLanguagesAsVector(&availLanguages);
	QStringList result;
	for(int i = 0; i < availLanguages.size(); ++i) {
		result.append(availLanguages[i].c_str());
	}
	qSort(result.begin(), result.end(), [](const QString & s1, const QString & s2) {
		bool s1Script = s1.startsWith("script") || s1.left(1) == s1.left(1).toUpper();
		bool s2Script = s2.startsWith("script") || s2.left(1) == s2.left(1).toUpper();
		if(s1Script != s2Script) {
			return !s1Script;
		} else {
			return s1 < s2;
		}
	});
	return result;
}

std::unique_ptr<tesseract::TessBaseAPI> Recognizer::initTesseract(const char* language, bool* ok) const {
	// unfortunately tesseract creates deliberate aborts when an error occurs
	std::signal(SIGABRT, MainWindow::tesseractCrash);
	QByteArray current = setlocale(LC_ALL, NULL);
	setlocale(LC_ALL, "C");
	std::unique_ptr<tesseract::TessBaseAPI> tess(new tesseract::TessBaseAPI());
	int ret = tess->Init(nullptr, language);
	setlocale(LC_NUMERIC, current.constData());

	if(ok) {
		*ok = ret != -1;
	}
	return std::move(tess);
}

void Recognizer::updateLanguagesMenu() {
	ui.menuLanguages->clear();
	delete m_langMenuRadioGroup;
	m_langMenuRadioGroup = new QActionGroup(this);
	delete m_langMenuCheckGroup;
	m_langMenuCheckGroup = new QActionGroup(this);
	m_langMenuCheckGroup->setExclusive(false);
	delete m_psmCheckGroup;
	m_psmCheckGroup = new QActionGroup(this);
	connect(m_psmCheckGroup, SIGNAL(triggered(QAction*)), this, SLOT(psmSelected(QAction*)));
	m_menuMultilanguage = nullptr;
	m_curLang = Config::Lang();
	QAction* curitem = nullptr;
	QAction* activeitem = nullptr;
	bool haveOsd = false;

	QStringList parts = ConfigSettings::get<VarSetting<QString>>("language")->getValue().split(":");
	Config::Lang curlang = {parts.empty() ? "eng" : parts[0], parts.size() < 2 ? "" : parts[1], parts.size() < 3 ? "" : parts[2]};

	QList<QString> dicts = QtSpell::Checker::getLanguageList();

	QStringList availLanguages = getAvailableLanguages();

	if(availLanguages.empty()) {
		QMessageBox::warning(MAIN, _("No languages available"), _("No tesseract languages are available for use. Recognition will not work."));
		m_langLabel = "";
		ui.toolButtonRecognize->setText(QString("%1\n%2").arg(m_modeLabel).arg(m_langLabel));
	}

	// Add menu items for languages, with spelling submenu if available
	for(const QString& langprefix : availLanguages) {
		if(langprefix == "osd") {
			haveOsd = true;
			continue;
		}
		Config::Lang lang = {langprefix, QString(), QString()};
		if(!MAIN->getConfig()->searchLangSpec(lang)) {
			lang.name = lang.prefix;
		}
		QList<QString> spelldicts;
		if(!lang.code.isEmpty()) {
			for(const QString& dict : dicts) {
				if(dict.left(2) == lang.code.left(2)) {
					spelldicts.append(dict);
				}
			}
			std::sort(spelldicts.begin(), spelldicts.end());
		}
		if(!spelldicts.empty()) {
			QAction* item = new QAction(lang.name, ui.menuLanguages);
			QMenu* submenu = new QMenu();
			for(const QString& dict : spelldicts) {
				Config::Lang itemlang = {lang.prefix, dict, lang.name};
				curitem = new QAction(QtSpell::Checker::decodeLanguageCode(dict), m_langMenuRadioGroup);
				curitem->setCheckable(true);
				curitem->setData(QVariant::fromValue(itemlang));
				connect(curitem, SIGNAL(triggered()), this, SLOT(setLanguage()));
				if(curlang.prefix == lang.prefix && (
				            curlang.code == dict ||
				            (!activeitem && (curlang.code == dict.left(2) || curlang.code.isEmpty())))) {
					curlang = itemlang;
					activeitem = curitem;
				}
				submenu->addAction(curitem);
			}
			item->setMenu(submenu);
			ui.menuLanguages->addAction(item);
		} else {
			curitem = new QAction(lang.name, m_langMenuRadioGroup);
			curitem->setCheckable(true);
			curitem->setData(QVariant::fromValue(lang));
			connect(curitem, SIGNAL(triggered()), this, SLOT(setLanguage()));
			if(curlang.prefix == lang.prefix) {
				curlang = lang;
				activeitem = curitem;
			}
			ui.menuLanguages->addAction(curitem);
		}
	}

	// Add multilanguage menu
	bool isMultilingual = false;
	if(!availLanguages.isEmpty()) {
		ui.menuLanguages->addSeparator();
		m_multilingualAction = new QAction(_("Multilingual"), m_langMenuRadioGroup);
		m_multilingualAction->setCheckable(true);
		m_menuMultilanguage = new QMenu();
		isMultilingual = curlang.prefix.contains('+');
		QStringList sellangs = curlang.prefix.split('+', QString::SkipEmptyParts);
		for(const QString& langprefix : availLanguages) {
			if(langprefix == "osd") {
				continue;
			}
			Config::Lang lang = {langprefix, "", ""};
			if(!MAIN->getConfig()->searchLangSpec(lang)) {
				lang.name = lang.prefix;
			}
			QAction* item = new QAction(lang.name, m_langMenuCheckGroup);
			item->setCheckable(true);
			item->setData(QVariant::fromValue(lang.prefix));
			item->setChecked(isMultilingual && sellangs.contains(lang.prefix));
			connect(item, SIGNAL(triggered()), this, SLOT(setMultiLanguage()));
			m_menuMultilanguage->addAction(item);
		}
		m_menuMultilanguage->installEventFilter(this);
		m_multilingualAction->setMenu(m_menuMultilanguage);
		ui.menuLanguages->addAction(m_multilingualAction);
	}
	if(isMultilingual) {
		activeitem = m_multilingualAction;
		setMultiLanguage();
	} else if(activeitem == nullptr) {
		activeitem = curitem;
	}
	if(activeitem) {
		activeitem->trigger();
	}

	// Add PSM items
	ui.menuLanguages->addSeparator();
	QMenu* psmMenu = new QMenu();
	int activePsm = ConfigSettings::get<VarSetting<int>>("psm")->getValue();

	struct PsmEntry {
		QString label;
		tesseract::PageSegMode psmMode;
		bool requireOsd;
	};
	QVector<PsmEntry> psmModes = {
		PsmEntry{_("Automatic page segmentation"), tesseract::PSM_AUTO, false},
		PsmEntry{_("Page segmentation with orientation and script detection"), tesseract::PSM_AUTO_OSD, true},
		PsmEntry{_("Assume single column of text"), tesseract::PSM_SINGLE_COLUMN, false},
		PsmEntry{_("Assume single block of vertically aligned text"), tesseract::PSM_SINGLE_BLOCK_VERT_TEXT, false},
		PsmEntry{_("Assume a single uniform block of text"), tesseract::PSM_SINGLE_BLOCK, false},
		PsmEntry{_("Assume a line of text"), tesseract::PSM_SINGLE_LINE, false},
		PsmEntry{_("Assume a single word"), tesseract::PSM_SINGLE_WORD, false},
		PsmEntry{_("Assume a single word in a circle"), tesseract::PSM_CIRCLE_WORD, false},
		PsmEntry{_("Sparse text in no particular order"), tesseract::PSM_SPARSE_TEXT, false},
		PsmEntry{_("Sparse text with orientation and script detection"), tesseract::PSM_SPARSE_TEXT_OSD, true}
	};
	for(const auto& entry : psmModes) {
		QAction* item = psmMenu->addAction(entry.label);
		item->setData(entry.psmMode);
		item->setEnabled(!entry.requireOsd || haveOsd);
		item->setCheckable(true);
		item->setChecked(activePsm == entry.psmMode);
		m_psmCheckGroup->addAction(item);
	}

	QAction* psmAction = new QAction(_("Page segmentation mode"), ui.menuLanguages);
	psmAction->setMenu(psmMenu);
	ui.menuLanguages->addAction(psmAction);
	ui.menuLanguages->addAction(_("Character whitelist / blacklist..."), this, SLOT(manageCharacterLists()));


	// Add installer item
	ui.menuLanguages->addSeparator();
	ui.menuLanguages->addAction(_("Manage languages..."), MAIN, SLOT(manageLanguages()));
}

void Recognizer::setLanguage() {
	QAction* item = qobject_cast<QAction*>(QObject::sender());
	if(item->isChecked()) {
		Config::Lang lang = item->data().value<Config::Lang>();
		if(!lang.code.isEmpty()) {
			m_langLabel = QString("%1 (%2)").arg(lang.name, lang.code);
		} else {
			m_langLabel = QString("%1").arg(lang.name);
		}
		ui.toolButtonRecognize->setText(QString("%1\n%2").arg(m_modeLabel).arg(m_langLabel));
		m_curLang = lang;
		ConfigSettings::get<VarSetting<QString>>("language")->setValue(lang.prefix + ":" + lang.code);
		emit languageChanged(m_curLang);
	}
}

void Recognizer::setMultiLanguage() {
	m_multilingualAction->setChecked(true);
	QString langs;
	for(QAction* action : m_langMenuCheckGroup->actions()) {
		if(action->isChecked()) {
			langs += action->data().toString() + "+";
		}
	}
	if(langs.isEmpty()) {
		langs = "eng+";
	}
	langs = langs.left(langs.length() - 1);
	m_langLabel = langs;
	ui.toolButtonRecognize->setText(QString("%1\n%2").arg(m_modeLabel).arg(m_langLabel));
	m_curLang = {langs, "", "Multilingual"};
	ConfigSettings::get<VarSetting<QString>>("language")->setValue(langs + ":");
	emit languageChanged(m_curLang);
}

void Recognizer::setRecognizeMode(const QString& mode) {
	m_modeLabel = mode;
	ui.toolButtonRecognize->setText(QString("%1\n%2").arg(m_modeLabel).arg(m_langLabel));
}

void Recognizer::clearLineEditPageRangeStyle() {
	qobject_cast<QLineEdit*>(QObject::sender())->setStyleSheet("");
}

void Recognizer::psmSelected(QAction* action) {
	ConfigSettings::get<VarSetting<int>>("psm")->setValue(action->data().toInt());
}

void Recognizer::manageCharacterLists() {
	m_charListDialog->exec();
}

QList<int> Recognizer::selectPages(bool& autodetectLayout) {
	int nPages = MAIN->getDisplayer()->getNPages();

	m_pagesDialogUi.lineEditPageRange->setText(QString("1-%1").arg(nPages));
	m_pagesDialogUi.lineEditPageRange->setFocus();
	m_pagesDialogUi.labelRecognitionArea->setVisible(MAIN->getDisplayer()->allowAutodetectOCRAreas());
	m_pagesDialogUi.comboBoxRecognitionArea->setVisible(MAIN->getDisplayer()->allowAutodetectOCRAreas());
	m_pagesDialogUi.groupBoxPrepend->setVisible(MAIN->getDisplayer()->allowAutodetectOCRAreas());

	m_pagesDialogUi.comboBoxRecognitionArea->setItemText(0, MAIN->getDisplayer()->hasMultipleOCRAreas() ? _("Current selection") : _("Entire page"));

	QRegExp validateRegEx("^[\\d,\\-\\s]+$");
	QList<int> pages;
	while(m_pagesDialog->exec() == QDialog::Accepted) {
		pages.clear();
		QString text = m_pagesDialogUi.lineEditPageRange->text();
		if(validateRegEx.indexIn(text) != -1) {
			text.replace(QRegExp("\\s+"), "");
			for(const QString& block : text.split(',', QString::SkipEmptyParts)) {
				QStringList ranges = block.split('-', QString::SkipEmptyParts);
				if(ranges.size() == 1) {
					int page = ranges[0].toInt();
					if(page > 0 && page <= nPages) {
						pages.append(page);
					}
				} else if(ranges.size() == 2) {
					int start = std::max(1, ranges[0].toInt());
					int end = std::min(nPages, ranges[1].toInt());
					for(int page = start; page <= end; ++page) {
						pages.append(page);
					}
				} else {
					pages.clear();
					break;
				}
			}
		}
		if(pages.empty()) {
			m_pagesDialogUi.lineEditPageRange->setStyleSheet("background: #FF7777; color: #FFFFFF;");
		} else {
			break;
		}
	}
	std::sort(pages.begin(), pages.end());
	autodetectLayout = m_pagesDialogUi.comboBoxRecognitionArea->isVisible() ? m_pagesDialogUi.comboBoxRecognitionArea->currentIndex() == 1 : false;
	return pages;
}

void Recognizer::recognizeButtonClicked() {
	int nPages = MAIN->getDisplayer()->getNPages();
	if(nPages == 1) {
		recognize({MAIN->getDisplayer()->getCurrentPage()});
	} else {
		ui.toolButtonRecognize->setCheckable(true);
		ui.toolButtonRecognize->setChecked(true);
		m_menuPages->popup(ui.toolButtonRecognize->mapToGlobal(QPoint(0, ui.toolButtonRecognize->height())));
		ui.toolButtonRecognize->setChecked(false);
		ui.toolButtonRecognize->setCheckable(false);
	}
}

void Recognizer::recognizeCurrentPage() {
	recognize({MAIN->getDisplayer()->getCurrentPage()});
}

void Recognizer::recognizeMultiplePages() {
	bool autodetectLayout = false;
	QList<int> pages = selectPages(autodetectLayout);
	recognize(pages, autodetectLayout);
}

void Recognizer::recognize(const QList<int>& pages, bool autodetectLayout) {
	bool prependFile = pages.size() > 1 && ConfigSettings::get<SwitchSetting>("ocraddsourcefilename")->getValue();
	bool prependPage = pages.size() > 1 && ConfigSettings::get<SwitchSetting>("ocraddsourcepage")->getValue();
	bool ok = false;
	auto tess = initTesseract(m_curLang.prefix.toLocal8Bit().constData(), &ok);
	if(ok) {
		QString failed;
		tess->SetPageSegMode(static_cast<tesseract::PageSegMode>(m_psmCheckGroup->checkedAction()->data().toInt()));
		if(m_charListDialogUi.radioButtonWhitelist->isChecked()) {
			tess->SetVariable("tessedit_char_whitelist", m_charListDialogUi.lineEditWhitelist->text().toLocal8Bit());
		}
		if(m_charListDialogUi.radioButtonBlacklist->isChecked()) {
			tess->SetVariable("tessedit_char_blacklist", m_charListDialogUi.lineEditBlacklist->text().toLocal8Bit());
		}
		OutputEditor::ReadSessionData* readSessionData = MAIN->getOutputEditor()->initRead(*tess);
		ProgressMonitor monitor(pages.size());
		MAIN->showProgress(&monitor);
		Utils::busyTask([&] {
			int npages = pages.size();
			int idx = 0;
			QString prevFile;
			for(int page : pages) {
				monitor.desc.progress = 0;
				++idx;
				QMetaObject::invokeMethod(MAIN, "pushState", Qt::QueuedConnection, Q_ARG(MainWindow::State, MainWindow::State::Busy), Q_ARG(QString, _("Recognizing page %1 (%2 of %3)").arg(page).arg(idx).arg(npages)));

				PageData pageData;
				pageData.success = false;
				QMetaObject::invokeMethod(this, "setPage", Qt::BlockingQueuedConnection, Q_RETURN_ARG(PageData, pageData), Q_ARG(int, page), Q_ARG(bool, autodetectLayout));
				if(!pageData.success) {
					failed.append(_("\n- Page %1: failed to render page").arg(page));
					MAIN->getOutputEditor()->readError(_("\n[Failed to recognize page %1]\n"), readSessionData);
					continue;
				}
				readSessionData->file = pageData.filename;
				readSessionData->page = pageData.page;
				readSessionData->angle = pageData.angle;
				readSessionData->resolution = pageData.resolution;
				bool firstChunk = true;
				bool newFile = readSessionData->file != prevFile;
				prevFile = readSessionData->file;
				for(const QImage& image : pageData.ocrAreas) {
					readSessionData->prependPage = prependPage && firstChunk;
					readSessionData->prependFile = prependFile && (readSessionData->prependPage || newFile);
					firstChunk = false;
					newFile = false;
					tess->SetImage(image.bits(), image.width(), image.height(), 4, image.bytesPerLine());
					tess->SetSourceResolution(MAIN->getDisplayer()->getCurrentResolution());
					tess->Recognize(&monitor.desc);
					if(!monitor.cancelled()) {
						MAIN->getOutputEditor()->read(*tess, readSessionData);
					}
				}
				QMetaObject::invokeMethod(MAIN, "popState", Qt::QueuedConnection);
				monitor.increaseProgress();
				if(monitor.cancelled()) {
					break;
				}
			}
			return true;
		}, _("Recognizing..."));
		MAIN->hideProgress();
		MAIN->getOutputEditor()->finalizeRead(readSessionData);
		if(!failed.isEmpty()) {
			QMessageBox::critical(MAIN, _("Recognition errors occurred"), _("The following errors occurred:%1").arg(failed));
		}
	}
}

bool Recognizer::recognizeImage(const QImage& image, OutputDestination dest) {
	bool ok = false;
	auto tess = initTesseract(m_curLang.prefix.toLocal8Bit().constData(), &ok);
	if(!ok) {
		QMessageBox::critical(MAIN, _("Recognition errors occurred"), _("Failed to initialize tesseract"));
		return false;
	}
	tess->SetImage(image.bits(), image.width(), image.height(), 4, image.bytesPerLine());
	ProgressMonitor monitor(1);
	MAIN->showProgress(&monitor);
	if(dest == OutputDestination::Buffer) {
		OutputEditor::ReadSessionData* readSessionData = MAIN->getOutputEditor()->initRead(*tess);
		readSessionData->file = MAIN->getDisplayer()->getCurrentImage(readSessionData->page);
		readSessionData->angle = MAIN->getDisplayer()->getCurrentAngle();
		readSessionData->resolution = MAIN->getDisplayer()->getCurrentResolution();
		Utils::busyTask([&] {
			tess->Recognize(&monitor.desc);
			if(!monitor.cancelled()) {
				MAIN->getOutputEditor()->read(*tess, readSessionData);
			}
			return true;
		}, _("Recognizing..."));
		MAIN->getOutputEditor()->finalizeRead(readSessionData);
	} else if(dest == OutputDestination::Clipboard) {
		QString output;
		if(Utils::busyTask([&] {
		tess->Recognize(&monitor.desc);
			if(!monitor.cancelled()) {
				char* text = tess->GetUTF8Text();
				output = QString::fromUtf8(text);
				delete[] text;
				return true;
			}
			return false;
		}, _("Recognizing..."))) {
			QApplication::clipboard()->setText(output);
		}
	}
	MAIN->hideProgress();
	return true;
}

Recognizer::PageData Recognizer::setPage(int page, bool autodetectLayout) {
	PageData pageData;
	pageData.success = MAIN->getDisplayer()->setup(&page);
	if(pageData.success) {
		if(autodetectLayout) {
			MAIN->getDisplayer()->autodetectOCRAreas();
		}
		pageData.filename = MAIN->getDisplayer()->getCurrentImage(pageData.page);
		pageData.angle = MAIN->getDisplayer()->getCurrentAngle();
		pageData.resolution = MAIN->getDisplayer()->getCurrentResolution();
		pageData.ocrAreas = MAIN->getDisplayer()->getOCRAreas();
	}
	return pageData;
}

bool Recognizer::eventFilter(QObject* obj, QEvent* ev) {
	if(obj == ui.menuLanguages && ev->type() == QEvent::MouseButtonPress) {
		QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(ev);
		QAction* actionAtPos = ui.menuLanguages->actionAt(mouseEvent->pos());
		if(actionAtPos && actionAtPos == m_multilingualAction) {
			m_multilingualAction->toggle();
			if(m_multilingualAction->isChecked()) {
				setMultiLanguage();
			}
			return true;
		}
	} else if(obj == m_menuMultilanguage && (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonRelease)) {
		QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(ev);
		QAction* action = m_menuMultilanguage->actionAt(mouseEvent->pos());
		if(action) {
			if(ev->type() == QEvent::MouseButtonRelease) {
				action->trigger();
			}
			return true;
		}
	}
	return QObject::eventFilter(obj, ev);
}
