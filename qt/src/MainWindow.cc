/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * MainWindow.cc
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

#include <QCloseEvent>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDesktopServices>
#include <QDir>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QProcess>
#include <QProgressBar>
#include <QStatusBar>
#include <QUrl>
#include <csignal>
#include <iostream>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
#include <QtSpell.hpp>
#define USE_STD_NAMESPACE
#include <tesseract/baseapi.h>
#undef USE_STD_NAMESPACE

#include "MainWindow.hh"
#include "Acquirer.hh"
#include "ConfigSettings.hh"
#include "Displayer.hh"
#include "DisplayerToolSelect.hh"
#include "DisplayerToolHOCR.hh"
#include "OutputEditorText.hh"
#include "OutputEditorHOCR.hh"
#include "Recognizer.hh"
#include "SourceManager.hh"
#include "TessdataManager.hh"
#include "Utils.hh"
#include "ui_AboutDialog.h"


#define CHECKURL "https://raw.githubusercontent.com/manisandro/gImageReader/master/LATEST"
#define DOWNLOADURL "https://github.com/manisandro/gImageReader/releases"
#define CHANGELOGURL "https://raw.githubusercontent.com/manisandro/gImageReader/master/NEWS"

void MainWindow::signalHandler(int signal) {
	signalHandlerExec(signal, false);
}

void MainWindow::tesseractCrash(int signal) {
	signalHandlerExec(signal, true);
}

void MainWindow::signalHandlerExec(int signal, bool tesseractCrash) {
	std::signal(signal, nullptr);

	QString filename;
	if(MAIN->getOutputEditor() && MAIN->getOutputEditor()->getModified()) {
		filename = QDir(Utils::documentsFolder()).absoluteFilePath(QString("%1_crash-save.txt").arg(PACKAGE_NAME));
		int i = 0;
		while(QFile(filename).exists()) {
			++i;
			filename = QDir(Utils::documentsFolder()).absoluteFilePath(QString("%1_crash-save_%2.txt").arg(PACKAGE_NAME).arg(i));
		}
		MAIN->getOutputEditor()->save(filename);
	}

	QProcess process;
	process.start(QApplication::applicationFilePath(), QStringList() << "crashhandle" << QString::number(QApplication::applicationPid()) << QString::number(tesseractCrash) << filename);
#ifdef Q_OS_LINUX
	// Allow crash handler spawned debugger to attach to the crashed process
	prctl(PR_SET_PTRACER, process.pid(), 0, 0, 0);
#endif
	process.waitForFinished(-1);
	std::raise(signal);
}

#if !(defined(__ARMEL__) || defined(__LCC__) && __LCC__ <= 121)
static void terminateHandler() {
	std::set_terminate(nullptr);
	std::exception_ptr exptr = std::current_exception();
	if (exptr != 0) {
		try {
			std::rethrow_exception(exptr);
		} catch (std::exception& ex) {
			std::cerr << "Terminated due to exception: " << ex.what() << std::endl;
		} catch (...) {
			std::cerr << "Terminated due to unknown exception" << std::endl;
		}
	} else {
		std::cerr << "Terminated due to unknown reason:" << std::endl;
	}
	MainWindow::signalHandler(SIGABRT);
}
#endif

MainWindow* MainWindow::s_instance = nullptr;

MainWindow::MainWindow(const QStringList& files)
	: m_idleActions(0) {
	s_instance = this;

	std::signal(SIGSEGV, signalHandler);
	std::signal(SIGABRT, signalHandler);
#if !(defined(__ARMEL__) || defined(__LCC__) && __LCC__ <= 121)
	std::set_terminate(terminateHandler);
#endif

	QList<QNetworkProxy> listOfProxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl(CHECKURL)));
	if (listOfProxies.size()) {
		QNetworkProxy::setApplicationProxy(listOfProxies[0]);
	}

	qRegisterMetaType<MainWindow::State>();

	ui.setupUi(this);

	m_config = new Config(this);
	m_acquirer = new Acquirer(ui);
	m_displayer = new Displayer(ui);
	m_recognizer = new Recognizer(ui);
	m_sourceManager = new SourceManager(ui);

	ui.centralwidget->layout()->addWidget(m_displayer);
	ui.toolBarMain->setLayoutDirection(Qt::LeftToRight);

	m_idleActions.setExclusive(false);
	m_idleActions.addAction(ui.actionZoomIn);
	m_idleActions.addAction(ui.actionZoomOut);
	m_idleActions.addAction(ui.actionOriginalSize);
	m_idleActions.addAction(ui.actionBestFit);
	m_idleActions.addAction(ui.actionRotateLeft);
	m_idleActions.addAction(ui.actionRotateRight);
	m_idleActions.addAction(ui.actionAutodetectLayout);
	m_idleWidgets.append(ui.spinBoxRotation);
	m_idleWidgets.append(ui.spinBoxPage);
	m_idleWidgets.append(ui.spinBoxBrightness);
	m_idleWidgets.append(ui.spinBoxContrast);
	m_idleWidgets.append(ui.spinBoxResolution);
	m_idleWidgets.append(ui.toolButtonRecognize);

	connect(ui.actionRedetectLanguages, SIGNAL(triggered()), m_recognizer, SLOT(updateLanguagesMenu()));
	connect(ui.actionManageLanguages, SIGNAL(triggered()), this, SLOT(manageLanguages()));
	connect(ui.actionPreferences, SIGNAL(triggered()), this, SLOT(showConfig()));
	connect(ui.actionHelp, SIGNAL(triggered()), this, SLOT(showHelp()));
	connect(ui.actionAbout, SIGNAL(triggered()), this, SLOT(showAbout()));
	connect(ui.actionImageControls, SIGNAL(toggled(bool)), ui.widgetImageControls, SLOT(setVisible(bool)));
	connect(m_acquirer, SIGNAL(scanPageAvailable(QString)), m_sourceManager, SLOT(addSource(QString)));
	connect(m_sourceManager, SIGNAL(sourceChanged()), this, SLOT(onSourceChanged()));
	connect(ui.actionToggleOutputPane, SIGNAL(toggled(bool)), ui.dockWidgetOutput, SLOT(setVisible(bool)));
	connect(ui.comboBoxOCRMode, SIGNAL(currentIndexChanged(int)), this, SLOT(setOCRMode(int)));
	connect(m_recognizer, SIGNAL(languageChanged(Config::Lang)), this, SLOT(languageChanged(Config::Lang)));
	connect(ui.actionAutodetectLayout, SIGNAL(triggered()), m_displayer, SLOT(autodetectOCRAreas()));

	ADD_SETTING(VarSetting<QByteArray>("wingeom"));
	ADD_SETTING(VarSetting<QByteArray>("winstate"));
	ADD_SETTING(ActionSetting("showcontrols", ui.actionImageControls));
	ADD_SETTING(ComboSetting("outputeditor", ui.comboBoxOCRMode, 0));

	m_recognizer->updateLanguagesMenu();

	m_progressWidget = new QWidget(this);
	m_progressWidget->setLayout(new QHBoxLayout());
	m_progressWidget->layout()->setContentsMargins(0, 0, 0, 0);
	m_progressWidget->layout()->setSpacing(2);
	m_progressWidget->layout()->addWidget(new QLabel());
	m_progressBar = new QProgressBar();
	m_progressBar->setRange(0, 100);
	m_progressBar->setMaximumWidth(100);
	m_progressTimer.setSingleShot(false);
	connect(&m_progressTimer, SIGNAL(timeout()), this, SLOT(progressUpdate()));
	m_progressWidget->layout()->addWidget(m_progressBar);
	m_progressCancelButton = new QToolButton();
	m_progressCancelButton->setIcon(QIcon::fromTheme("dialog-close"));
	connect(m_progressCancelButton, SIGNAL(clicked(bool)), this, SLOT(progressCancel()));
	m_progressWidget->layout()->addWidget(m_progressCancelButton);
	statusBar()->addPermanentWidget(m_progressWidget);
	m_progressWidget->setVisible(false);

	pushState(State::Idle, _("Select an image to begin..."));

	restoreGeometry(ConfigSettings::get<VarSetting<QByteArray>>("wingeom")->getValue());
	restoreState(ConfigSettings::get<VarSetting<QByteArray>>("winstate")->getValue());
	ui.dockWidgetOutput->setVisible(false);

	ui.actionSources->trigger();

#if ENABLE_VERSIONCHECK
	if(ConfigSettings::get<SwitchSetting>("updatecheck")->getValue()) {
		connect(&m_versionCheckThread, SIGNAL(finished()), this, SLOT(checkVersion()));
		m_versionCheckThread.start();
	}
#endif

	m_sourceManager->addSources(files);
}

MainWindow::~MainWindow() {
#if ENABLE_VERSIONCHECK
	while(m_versionCheckThread.isRunning()) {
		QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
	}
#endif
	delete m_acquirer;
	delete m_outputEditor;
	delete m_sourceManager;
	m_displayer->setTool(nullptr);
	delete m_displayerTool;
	delete m_displayer;
	delete m_recognizer;
	delete m_config;
	s_instance = nullptr;
}

void MainWindow::openFiles(const QStringList& files) {
	m_sourceManager->addSources(files);
}

void MainWindow::setOutputPaneVisible(bool visible) {
	ui.actionToggleOutputPane->setChecked(visible);
}

void MainWindow::pushState(MainWindow::State state, const QString& msg) {
	m_stateStack.push(QPair<State, QString>(state, msg));
	ui.statusbar->showMessage(msg);
	setState(state);
	if(state == State::Busy) {
		QApplication::setOverrideCursor(Qt::WaitCursor);
	}
}

void MainWindow::popState() {
	if(m_stateStack.top().first == State::Busy) {
		QApplication::restoreOverrideCursor();
	}
	m_stateStack.pop();
	const QPair<State, QString>& pair = m_stateStack.top();
	ui.statusbar->showMessage(pair.second);
	setState(pair.first);
}

void MainWindow::setState(State state) {
	bool isIdle = state == State::Idle;
	m_idleActions.setEnabled(!isIdle);
	for(QWidget* widget : m_idleWidgets) {
		widget->setEnabled(!isIdle);
	}
}

void MainWindow::closeEvent(QCloseEvent* ev) {
	if(m_stateStack.top().first == State::Busy) {
		ev->ignore();
	} else if(!m_outputEditor->clear()) {
		ev->ignore();
	} else if(!isMaximized()) {
		ConfigSettings::get<VarSetting<QByteArray>>("wingeom")->setValue(saveGeometry());
		ConfigSettings::get<VarSetting<QByteArray>>("winstate")->setValue(saveState());
	}
}

void MainWindow::onSourceChanged() {
	QList<Source*> sources = m_sourceManager->getSelectedSources();
	if(m_displayer->setSources(sources)) {
		setWindowTitle(QString("%1 - %2").arg(sources.size() == 1 ? sources.front()->displayname : _("Multiple sources")).arg(PACKAGE_NAME));
		if(m_stateStack.top().first == State::Idle) {
			pushState(State::Normal, _("Ready"));
		}
	} else {
		if(m_stateStack.top().first == State::Normal) {
			popState();
		}
		setWindowTitle(PACKAGE_NAME);
	}
}

void MainWindow::showAbout() {
	QDialog d(this);
	Ui::AboutDialog aboutDialogUi;
	aboutDialogUi.setupUi(&d);
	aboutDialogUi.labelVersion->setText(QString("%1 (%2)").arg(PACKAGE_VERSION, QString(PACKAGE_REVISION).left(6)));;
	aboutDialogUi.labelTesseractVer->setText(QString("<html><head/><body><p style=\"font-size:small;\">%1 %2</p></body></html>").arg(_("Using tesseract")).arg(TESSERACT_VERSION_STR));
	d.exec();
}

void MainWindow::showHelp(const QString& chapter) {
#ifdef Q_OS_WIN32
	// Always use relative path on Windows
	QString manualDirPath;
#else
	QString manualDirPath(MANUAL_DIR);
#endif
	if(manualDirPath.isEmpty()) {
		manualDirPath = QString("%1/../share/doc/gimagereader").arg(QApplication::applicationDirPath());
	}
	QDir manualDir(manualDirPath);
	QString language = QLocale::system().name().left(2);
	QString manualFile = manualDir.absoluteFilePath(QString("manual-%1.html").arg(language));
	if(!QFile(manualFile).exists()) {
		manualFile = manualDir.absoluteFilePath("manual.html");
	}
	QUrl manualUrl = QUrl::fromLocalFile(manualFile);
	manualUrl.setFragment(chapter);
	QDesktopServices::openUrl(manualUrl);
}

void MainWindow::manageLanguages() {
	TessdataManager manager(MAIN);
	if(manager.setup()) {
		manager.exec();
	}
}

void MainWindow::showConfig() {
	m_config->showDialog();
	m_recognizer->updateLanguagesMenu();
}

void MainWindow::setOCRMode(int idx) {
	if(m_outputEditor && !m_outputEditor->clear()) {
		ui.comboBoxOCRMode->blockSignals(true);
		if(dynamic_cast<OutputEditorText*>(m_outputEditor)) {
			ui.comboBoxOCRMode->setCurrentIndex(0);
		} else { /*if(dynamic_cast<OutputEditorHOCR*>(m_outputEditor))*/
			ui.comboBoxOCRMode->setCurrentIndex(1);
		}
		ui.comboBoxOCRMode->blockSignals(false);
	} else {
		delete m_displayerTool;
		delete m_outputEditor;
		if(idx == 0) {
			m_displayerTool = new DisplayerToolSelect(m_displayer);
			m_outputEditor = new OutputEditorText();
		} else { /*if(idx == 1)*/
			m_displayerTool = new DisplayerToolHOCR(m_displayer);
			m_outputEditor = new OutputEditorHOCR(static_cast<DisplayerToolHOCR*>(m_displayerTool));
		}
		ui.actionAutodetectLayout->setVisible(m_displayerTool->allowAutodetectOCRAreas());
		m_displayer->setTool(m_displayerTool);
		m_outputEditor->setLanguage(m_recognizer->getSelectedLanguage());
		connect(ui.actionToggleOutputPane, SIGNAL(toggled(bool)), m_outputEditor, SLOT(onVisibilityChanged(bool)));
		ui.dockWidgetOutput->setWidget(m_outputEditor->getUI());
	}
}

void MainWindow::addNotification(const QString& title, const QString& message, const QList<NotificationAction>& actions, MainWindow::Notification* handle) {
	QFrame* frame = new QFrame();
	frame->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
	frame->setStyleSheet("background: #FFD000;");
	QHBoxLayout* layout = new QHBoxLayout(frame);
	layout->addWidget(new QLabel(QString("<b>%1</b>").arg(title), frame));
	QLabel* msgLabel = new QLabel(message, frame);
	msgLabel->setWordWrap(true);
	layout->addWidget(msgLabel, 1);
	for(const NotificationAction& action : actions) {
		QToolButton* btn = new QToolButton(frame);
		btn->setText(action.text);
		connect(btn, SIGNAL(clicked()), action.target, action.slot);
		if(action.close) {
			btn->setProperty("handle", QVariant::fromValue(reinterpret_cast<void*>(handle)));
			btn->setProperty("frame", QVariant::fromValue(reinterpret_cast<void*>(frame)));
			connect(btn, SIGNAL(clicked()), this, SLOT(hideNotification()));
		}
		layout->addWidget(btn);
	}
	QToolButton* closeBtn = new QToolButton();
	closeBtn->setIcon(QIcon::fromTheme("dialog-close"));
	closeBtn->setProperty("handle", QVariant::fromValue(reinterpret_cast<void*>(handle)));
	closeBtn->setProperty("frame", QVariant::fromValue(reinterpret_cast<void*>(frame)));
	connect(closeBtn, SIGNAL(clicked()), this, SLOT(hideNotification()));
	layout->addWidget(closeBtn);
	ui.centralwidget->layout()->addWidget(frame);
	if(handle) {
		*handle = frame;
	}
}

void MainWindow::hideNotification(Notification handle) {
	if(!handle && QObject::sender()) {
		handle = static_cast<QFrame*>(static_cast<QToolButton*>(QObject::sender())->property("frame").value<void*>());
		Notification* h = reinterpret_cast<void**>(static_cast<QToolButton*>(QObject::sender())->property("handle").value<void*>());
		if(h) {
			*h = nullptr;
		}
	}
	if(handle) {
		static_cast<QFrame*>(handle)->deleteLater();
	}
}

void MainWindow::VersionCheckThread::run() {
	QString messages;
	QString newver = Utils::download(QUrl(CHECKURL), messages, 5000);
	newver.replace(QRegExp("\\s+"), "");
	QRegExp pat(R"(^[\d+\.]+\d+$)");
	if(pat.exactMatch(newver)) {
		m_newestVersion = newver;
	}
}

void MainWindow::checkVersion() {
	QString newver = m_versionCheckThread.getNewestVersion();
	qDebug("Newest version is: %s", qPrintable(newver));
	if(newver.isEmpty()) {
		return;
	}
	QString curver = PACKAGE_VERSION;

	if(newver.compare(curver) > 0) {
		addNotification(_("New version"), _("gImageReader %1 is available").arg(newver), {
			{_("Download"), this, SLOT(openDownloadUrl()), false},
			{_("Changelog"), this, SLOT(openChangeLogUrl()), false},
			{_("Don't notify again"), m_config, SLOT(disableUpdateCheck()), true}
		});
	}
}

void MainWindow::openDownloadUrl() {
	QDesktopServices::openUrl(QUrl(DOWNLOADURL));
}

void MainWindow::openChangeLogUrl() {
	QDesktopServices::openUrl(QUrl(CHANGELOGURL));
}

void MainWindow::showProgress(ProgressMonitor* monitor, int updateInterval) {
	m_progressMonitor = monitor;
	m_progressTimer.start(updateInterval);
	m_progressCancelButton->setEnabled(true);
	m_progressBar->setValue(0);
	m_progressWidget->show();
}

void MainWindow::hideProgress() {
	m_progressWidget->hide();
	m_progressTimer.stop();
	m_progressMonitor = nullptr;
}

void MainWindow::progressCancel() {
	if(m_progressMonitor) {
		m_progressCancelButton->setEnabled(false);
		m_progressMonitor->cancel();
	}
}

void MainWindow::progressUpdate() {
	if(m_progressMonitor) {
		m_progressBar->setValue(m_progressMonitor->getProgress());
	}
}

void MainWindow::languageChanged(const Config::Lang& lang) {
	if(m_outputEditor) {
		m_outputEditor->setLanguage(lang);
	}
	hideNotification(m_notifierHandle);
	m_notifierHandle = nullptr;
	const QString& code = lang.code;
	if(!code.isEmpty() && !QtSpell::checkLanguageInstalled(code) && ConfigSettings::get<SwitchSetting>("dictinstall")->getValue()) {
		NotificationAction actionDontShowAgain = {_("Don't show again"), m_config, SLOT(disableDictInstall()), true};
		NotificationAction actionInstall = {_("Install"), this, SLOT(dictionaryAutoinstall()), false};
#ifdef Q_OS_LINUX
		if(getConfig()->useSystemDataLocations()) {
			QDBusConnectionInterface* iface = QDBusConnection::sessionBus().interface();
			iface->startService("org.freedesktop.PackageKit");
			if(!iface->isServiceRegistered("org.freedesktop.PackageKit").value()) {
				actionInstall = {_("Help"), this, SLOT(showHelp()), false}; // TODO #InstallSpelling
				qWarning("Could not find PackageKit on DBus, dictionary autoinstallation will not work");
			}
		}
#endif
		const QString& name = m_recognizer->getSelectedLanguage().name;
		addNotification(_("Spelling dictionary missing"), _("The spellcheck dictionary for %1 is not installed").arg(name), {actionInstall, actionDontShowAgain}, &m_notifierHandle);
	}
}

void MainWindow::dictionaryAutoinstall() {
	QString code = m_recognizer->getSelectedLanguage().code;
	pushState(State::Busy, _("Installing spelling dictionary for '%1'").arg(code));
#ifdef Q_OS_WIN
	bool isWindows = true;
#else
	bool isWindows = false;
#endif

	if(!isWindows && MAIN->getConfig()->useSystemDataLocations()) {
		// Place this in a ifdef since DBus stuff cannot be compiled on Windows
#ifdef Q_OS_LINUX
		QStringList files;
		for(const QString& langCulture : m_config->searchLangCultures(code)) {
			files.append("/usr/share/myspell/" + langCulture + ".dic");
			files.append("/usr/share/hunspell/" + langCulture + ".dic");
		}
		QDBusMessage req = QDBusMessage::createMethodCall("org.freedesktop.PackageKit", "/org/freedesktop/PackageKit", "org.freedesktop.PackageKit.Modify", "InstallProvideFiles");
		req.setArguments(QList<QVariant>() << QVariant::fromValue((quint32)winId()) << QVariant::fromValue(files) << QVariant::fromValue(QString("always")));
		QDBusMessage reply = QDBusConnection::sessionBus().call(req, QDBus::BlockWithGui, 3600000);
		if(reply.type() == QDBusMessage::ErrorMessage) {
			if(QMessageBox::Help == QMessageBox::critical(this, _("Error"), _("Failed to install spelling dictionary: %1").arg(reply.errorMessage()), QMessageBox::Ok | QMessageBox::Help, QMessageBox::Ok)) {
				showHelp("#InstallSpelling");
			}
		}
		m_recognizer->updateLanguagesMenu();
		popState();
#endif
	} else {
		QString url = "https://cgit.freedesktop.org/libreoffice/dictionaries/tree/";
		QString plainurl = "https://cgit.freedesktop.org/libreoffice/dictionaries/plain/";
		QDir spellingDir(getConfig()->spellingLocation());
		if(!QDir().mkpath(spellingDir.absolutePath())) {
			popState();
			if(QMessageBox::Help == QMessageBox::critical(this, _("Error"), _("Failed to create directory for spelling dictionaries."), QMessageBox::Ok | QMessageBox::Help, QMessageBox::Ok)) {
				showHelp("#InstallSpelling");
			}
			return;
		}
		QString messages;
		QString html = QString(Utils::download(url, messages));
		if(html.isEmpty()) {
			popState();
			if(QMessageBox::Help == QMessageBox::critical(this, _("Error"), _("Could not read %1: %2.").arg(url).arg(messages), QMessageBox::Ok | QMessageBox::Help, QMessageBox::Ok)) {
				showHelp("#InstallSpelling");
			}
			return;
		}
		int pos = 0;
		QString langCode = code.left(code.indexOf('_'));
		QRegExp langPat(QString(">(%1_?[A-Z]*)<").arg(langCode));
		QRegExp dictPat(QString(">(%1_?[\\w_]*\\.(dic|aff))<").arg(langCode));
		QStringList downloaded;
		QStringList failed;

		while((pos = langPat.indexIn(html, pos)) != -1) {
			QString lang = langPat.cap(1);
			pos += langPat.matchedLength();

			QString dictHtml = QString(Utils::download(url + lang + "/", messages));
			if(dictHtml.isEmpty()) {
				continue;
			}

			int dictPos = 0;
			while((dictPos = dictHtml.indexOf(dictPat, dictPos)) != -1) {
				QString filename = dictPat.cap(1);
				pushState(State::Busy, _("Downloading '%1'...").arg(filename));
				QByteArray data = Utils::download(plainurl + lang + "/" + filename, messages);
				if(!data.isNull()) {
					QFile file(spellingDir.absoluteFilePath(filename));
					if(file.open(QIODevice::WriteOnly)) {
						file.write(data);
						downloaded.append(filename);
					} else {
						failed.append(filename);
					}
				} else {
					failed.append(filename);
				}
				popState();
				dictPos += dictPat.matchedLength();
			}
		}

		popState();
		if(!failed.isEmpty()) {
			QMessageBox::critical(this, _("Error"), _("The following dictionaries could not be downloaded:\n%1\n\nCheck the connectivity and directory permissions.\nHint: If you don't have write permissions in system folders, you can switch to user paths in the settings dialog.").arg(failed.join("\n")));
		} else if(!downloaded.isEmpty()) {
			m_recognizer->updateLanguagesMenu();
			QMessageBox::information(this, _("Dictionaries installed"), _("The following dictionary files were installed:\n%1").arg(downloaded.join("\n")));
		} else {
			if(QMessageBox::Help == QMessageBox::critical(this, _("Error"), _("No spelling dictionaries found for '%1'.").arg(code), QMessageBox::Ok | QMessageBox::Help, QMessageBox::Ok)) {
				showHelp("#InstallSpelling");
			}
		}
	}
}
