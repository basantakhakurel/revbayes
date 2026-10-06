#include "app/MainWindow.h"

#include "app/ConsoleWidget.h"
#include "app/EditorTabs.h"
#include "app/FileExplorerPanel.h"
#include "app/HelpDock.h"
#include "app/PreferencesDialog.h"
#include "app/Settings.h"
#include "app/VariablesPanel.h"
#include "backend/Session.h"
#include "revstudio_version.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QToolBar>

namespace revstudio {

MainWindow::MainWindow(Session* session, Settings* settings, QWidget* parent)
    : QMainWindow(parent), session_(session), settings_(settings), console_(new ConsoleWidget(session, this)),
      editor_(new EditorTabs(console_, settings_, this)),
      variablesPanel_(new VariablesPanel(session, console_, this)),
      explorerPanel_(new FileExplorerPanel(session, console_, editor_, settings_, this)),
      helpPanel_(new HelpDock(session, this)), status_(new QLabel(this)), cursorStatus_(new QLabel(this))
{
    setWindowTitle(tr("RevStudio %1").arg(QStringLiteral(REVSTUDIO_VERSION)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/revstudio.svg")));
    resize(1100, 700);

    setCentralWidget(editor_);
    console_->applyFont(settings_->editorFont());   // editor_ already applied its own via its own constructor

    consoleDock_ = new QDockWidget(tr("Console"), this);
    consoleDock_->setObjectName(QStringLiteral("consoleDock"));   // QMainWindow::saveState() needs object names
    consoleDock_->setWidget(console_);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock_);

    variablesDock_ = new QDockWidget(tr("Variables"), this);
    variablesDock_->setObjectName(QStringLiteral("variablesDock"));
    variablesDock_->setWidget(variablesPanel_);
    addDockWidget(Qt::RightDockWidgetArea, variablesDock_);

    explorerDock_ = new QDockWidget(tr("Files"), this);
    explorerDock_->setObjectName(QStringLiteral("explorerDock"));
    explorerDock_->setWidget(explorerPanel_);
    addDockWidget(Qt::LeftDockWidgetArea, explorerDock_);

    helpDock_ = new QDockWidget(tr("Help"), this);
    helpDock_->setObjectName(QStringLiteral("helpDock"));
    helpDock_->setWidget(helpPanel_);
    addDockWidget(Qt::RightDockWidgetArea, helpDock_);
    tabifyDockWidget(variablesDock_, helpDock_);   // stacked as tabs, not another strip down the right edge

    // Ctrl+Return/F5 are scoped to the editor (added to it directly, not just the menu bar): without this, Qt's
    // shortcut system -- which checks window-scoped action shortcuts before delivering the key event to the
    // focused widget at all -- would steal Ctrl+Return away from ConsoleInputEdit's OWN plain-Return-submits
    // handling whenever the console happened to have focus, since that handler never sees the key press in the
    // first place if a window-scoped shortcut already claimed it.
    auto* runSelectionAction = new QAction(tr("Run &Selection/Line"), this);
    runSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    runSelectionAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(runSelectionAction, &QAction::triggered, editor_, &EditorTabs::runSelectionOrLine);
    editor_->addAction(runSelectionAction);

    auto* runFileAction = new QAction(tr("Run &File"), this);
    runFileAction->setShortcut(Qt::Key_F5);
    runFileAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(runFileAction, &QAction::triggered, editor_, &EditorTabs::runCurrentFile);
    editor_->addAction(runFileAction);

    // Q8/6.10: "F1 on the token under the caret." Scoped to the editor the same way and for the same reason as
    // the two actions just above (so it is not lost to window-scoped shortcut dispatch); see the class comment
    // on why the console input does not get one too.
    auto* helpOnWordAction = new QAction(tr("&Help on Selection"), this);
    helpOnWordAction->setShortcut(Qt::Key_F1);
    helpOnWordAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(helpOnWordAction, &QAction::triggered, this, [this]
    {
        helpPanel_->showTopic(editor_->currentWordUnderCursor());
        helpDock_->show();
        helpDock_->raise();
    });
    editor_->addAction(helpOnWordAction);

    // No QAction shortcut for Ctrl+C here on purpose -- see the class comment on the eventFilter() below, which
    // is what actually binds it, conditionally, so plain copying in a text widget keeps working.
    stopAction_ = new QAction(tr("&Stop (Ctrl+C)"), this);
    stopAction_->setEnabled(false);
    connect(stopAction_, &QAction::triggered, console_, &ConsoleWidget::requestStop);

    QToolBar* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));   // same reason as the docks' object names, above
    toolbar->addAction(tr("New"), editor_, &EditorTabs::newDocument);
    toolbar->addAction(tr("Open..."), this, &MainWindow::openWithDialog);
    toolbar->addAction(tr("Save"), editor_, &EditorTabs::saveCurrent);
    toolbar->addSeparator();
    toolbar->addAction(runSelectionAction);
    toolbar->addAction(runFileAction);
    toolbar->addAction(stopAction_);
    toolbar->addSeparator();
    toolbar->addAction(tr("Clear Console"), console_, &ConsoleWidget::clearOutput);
    toolbar->addAction(consoleDock_->toggleViewAction());
    toolbar->addAction(variablesDock_->toggleViewAction());
    toolbar->addAction(explorerDock_->toggleViewAction());
    toolbar->addAction(helpDock_->toggleViewAction());

    statusBar()->addWidget(status_);
    statusBar()->addPermanentWidget(cursorStatus_);

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* newAction = fileMenu->addAction(tr("&New"), editor_, &EditorTabs::newDocument);
    newAction->setShortcut(QKeySequence::New);
    QAction* openAction = fileMenu->addAction(tr("&Open..."), this, &MainWindow::openWithDialog);
    openAction->setShortcut(QKeySequence::Open);
    recentFilesMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    connect(recentFilesMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentFilesMenu);
    fileMenu->addSeparator();
    QAction* saveAction = fileMenu->addAction(tr("&Save"), editor_, &EditorTabs::saveCurrent);
    saveAction->setShortcut(QKeySequence::Save);
    QAction* saveAsAction = fileMenu->addAction(tr("Save &As..."), editor_, &EditorTabs::saveCurrentAs);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    fileMenu->addSeparator();
    QAction* closeTabAction = fileMenu->addAction(tr("&Close Tab"), this, [this]
    {
        editor_->closeTabWithPrompt(editor_->currentIndex());
    });
    closeTabAction->setShortcut(QKeySequence::Close);
    fileMenu->addSeparator();
    QAction* preferencesAction = fileMenu->addAction(tr("&Preferences..."), this, &MainWindow::openPreferences);
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    fileMenu->addSeparator();
    QAction* quit = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->addAction(runSelectionAction);
    runMenu->addAction(runFileAction);
    runMenu->addAction(stopAction_);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(consoleDock_->toggleViewAction());
    viewMenu->addAction(variablesDock_->toggleViewAction());
    viewMenu->addAction(explorerDock_->toggleViewAction());
    viewMenu->addAction(helpDock_->toggleViewAction());
    QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const auto addThemeAction = [&](Theme::Mode mode, const QString& label)
    {
        QAction* action = themeMenu->addAction(label);
        action->setCheckable(true);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setThemeMode(mode); });
        themeActions_[static_cast<int>(mode)] = action;
    };
    addThemeAction(Theme::Mode::System, tr("&System"));
    addThemeAction(Theme::Mode::Light, tr("&Light"));
    addThemeAction(Theme::Mode::Dark, tr("&Dark"));
    themeActions_[static_cast<int>(settings_->themeMode())]->setChecked(true);   // Theme::apply() already ran in main()

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(helpOnWordAction);
    helpMenu->addAction(tr("Help &Index"), this, [this]
    {
        helpPanel_->showTopic(QString());
        helpDock_->show();
        helpDock_->raise();
    });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About RevStudio"), this, &MainWindow::showAbout);
    helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);      // the usual LGPL courtesy (note, 5.4)

    connect(session_, &Session::stateChanged, this, &MainWindow::updateStatus);
    connect(editor_, &EditorTabs::currentCursorPositionChanged, this, &MainWindow::updateCursorStatus);
    // Q7's error navigation: a "Problem processing line N in file F" link in the console.
    connect(console_, &ConsoleWidget::errorLinkActivated, this, [this](const QString& path, int line)
    {
        editor_->openFileAtLine(path, line);
    });
    // Section 6.8: "closing... prompts for unsaved tabs, and so does a backend quit event." A full prompt-and-
    // decide flow belongs with G2's still-open restart/failure UX (there is nothing to restart into yet, see
    // this class's own comment on that); this is the minimal honest version -- say so, so nothing is lost
    // silently, without pretending to offer a restart that does not exist.
    connect(session_, &Session::backendQuit, this, [this](const QString&)
    {
        if (editor_->hasUnsavedChanges())
        {
            QMessageBox::information(this, tr("Backend Exited"),
                tr("The backend exited (Rev code called quit()). Your editor tabs are untouched, but no backend "
                   "is running until you restart RevStudio."));
        }
    });
    qApp->installEventFilter(this);   // Ctrl+C as Stop; see the class comment

    updateStatus();
    updateCursorStatus(1, 1);

    const QStringList toRestore = settings_->openDocuments();
    for (const QString& path : toRestore)
    {
        editor_->openFile(path);
    }
    if (editor_->count() == 0)
    {
        editor_->newDocument();
    }

    if (!settings_->windowGeometry().isEmpty())
    {
        restoreGeometry(settings_->windowGeometry());
    }
    if (!settings_->windowState().isEmpty())
    {
        restoreState(settings_->windowState());
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride)
    {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_C && keyEvent->modifiers() == Qt::ControlModifier
            && session_->state() == Session::State::Busy)
        {
            // Only while Busy, and only without a text selection, so a plain Ctrl+C keeps copying normally the
            // rest of the time -- including a selection made IN the console output while a long run is still
            // streaming into it, which this would otherwise yank away from under the user.
            auto* focused = qobject_cast<QPlainTextEdit*>(QApplication::focusWidget());
            if (!focused || !focused->textCursor().hasSelection())
            {
                event->accept();
                console_->requestStop();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    const QStringList toPersist = editor_->openFilePaths();   // captured before closeAllWithPrompt() empties them
    if (!editor_->closeAllWithPrompt())
    {
        event->ignore();
        return;
    }
    settings_->setOpenDocuments(toPersist);
    settings_->setWindowGeometry(saveGeometry());
    settings_->setWindowState(saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::log(const QString& text)
{
    console_->logBackend(text);
}

void MainWindow::setThemeMode(Theme::Mode mode)
{
    Theme::apply(mode);
    settings_->setThemeMode(mode);
    // Keeps the View > Theme radio group in sync when the mode changes from somewhere other than the user
    // clicking one of its own actions (Preferences) -- clicking one of them already does this itself, via
    // QActionGroup's own exclusivity, so this is a harmless no-op redundant setChecked(true) on that path.
    themeActions_[static_cast<int>(mode)]->setChecked(true);
}

void MainWindow::rebuildRecentFilesMenu()
{
    recentFilesMenu_->clear();
    const QStringList files = settings_->recentFiles();
    if (files.isEmpty())
    {
        QAction* empty = recentFilesMenu_->addAction(tr("(No Recent Files)"));
        empty->setEnabled(false);
        return;
    }
    for (const QString& path : files)
    {
        recentFilesMenu_->addAction(path, this, [this, path] { editor_->openFile(path); });
    }
}

void MainWindow::openWithDialog()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open File"), QString(),
                                                        tr("Rev Scripts (*.Rev);;All Files (*)"));
    if (!path.isEmpty())
    {
        editor_->openFile(path);
    }
}

void MainWindow::openPreferences()
{
    PreferencesDialog dialog(settings_, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    setThemeMode(settings_->themeMode());
    editor_->applyFontToAllTabs(settings_->editorFont());
    console_->applyFont(settings_->editorFont());
    explorerPanel_->applyPreferences();
}

void MainWindow::updateCursorStatus(int line, int column)
{
    cursorStatus_->setText(tr("Ln %1, Col %2").arg(line).arg(column));
}

void MainWindow::updateStatus()
{
    QString text;
    switch (session_->state())
    {
    case Session::State::NotStarted: text = tr("Backend: not started"); break;
    case Session::State::Starting:   text = tr("Backend: starting..."); break;
    case Session::State::Ready:      text = tr("Idle  |  %1").arg(session_->serverVersion()); break;
    case Session::State::Busy:       text = tr("Running..."); break;
    case Session::State::Closing:    text = tr("Backend: shutting down..."); break;
    case Session::State::Closed:     text = tr("Backend: closed"); break;
    case Session::State::Failed:     text = tr("Backend: FAILED"); break;
    }
    status_->setText(text);
    stopAction_->setEnabled(session_->state() == Session::State::Busy);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About RevStudio"),
                       tr("<b>RevStudio %1</b><br>A front end for RevBayes.<br><br>"
                          "Built with Qt %2. Licensed under the GNU General Public License, version 3.")
                           .arg(QStringLiteral(REVSTUDIO_VERSION), QString::fromLatin1(qVersion())));
}

} // namespace revstudio
