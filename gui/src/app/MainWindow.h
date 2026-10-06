#ifndef REVSTUDIO_MAINWINDOW_H
#define REVSTUDIO_MAINWINDOW_H

#include "app/Theme.h"

#include <QMainWindow>

class QAction;
class QCloseEvent;
class QDockWidget;
class QEvent;
class QLabel;
class QMenu;

namespace revstudio {

class ConsoleWidget;
class EditorTabs;
class FileExplorerPanel;
class HelpDock;
class Session;
class Settings;
class VariablesPanel;

/**
 * G1 (docks, toolbar, menus, Settings, Theme) + G3 (ConsoleWidget) + G4/G5 (Variables/Files docks) + G6
 * (EditorTabs, the real central widget -- section 6.1's mockup: "the editor tabs are the central widget").
 * ConsoleWidget moved from the central widget (its G3-era stand-in, before an editor existed) to its own dock.
 *
 * Installs an application-wide event filter for exactly one thing: Ctrl+C as "Stop" (ConsoleWidget::requestStop())
 * while the backend is Busy, pulled forward from phase 3's Q2. A plain QAction shortcut would not do, because
 * Qt's window-scoped shortcut dispatch runs before a key event ever reaches the focused widget's own handling --
 * it would permanently steal Ctrl+C away from ordinary copying in the console output and the editor, Busy or not.
 * A QEvent::ShortcutOverride filter, only accepted while Busy and only when the focused widget has no text
 * selection to copy, gets Stop without that cost.
 *
 * Q8's help dock: F1 is scoped to the editor only (ScriptEditor::wordUnderCursor()), not the console input too --
 * section 6.10 says "F1 on the token under the caret" without saying which caret, and the editor is the one
 * place a Rev name is actually being typed/read most of the time; wiring the console's own private input-edit
 * class for this as well was judged not worth the extra plumbing for a first version.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Session* session, Settings* settings, QWidget* parent = nullptr);

    /** For diagnostics before the Session has anything to say (main.cpp's backend search). Forwards to the
     *  console's Backend tab. */
    void log(const QString& text);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateStatus();
    void updateCursorStatus(int line, int column);
    void showAbout();
    void setThemeMode(Theme::Mode mode);
    void rebuildRecentFilesMenu();
    void openWithDialog();
    void openPreferences();

    // Declaration order doubles as construction order (initializer-list position is NOT what controls it): the
    // console must exist before EditorTabs/panels that call ConsoleWidget::submitText() in their own
    // constructors, and before FileExplorerPanel, which also needs the (by-then-constructed) EditorTabs.
    Session*           session_;
    Settings*           settings_;
    ConsoleWidget*      console_;
    QDockWidget*        consoleDock_;
    EditorTabs*         editor_;
    VariablesPanel*     variablesPanel_;
    QDockWidget*        variablesDock_;
    FileExplorerPanel*  explorerPanel_;
    QDockWidget*        explorerDock_;
    HelpDock*           helpPanel_;
    QDockWidget*        helpDock_;
    QMenu*              recentFilesMenu_;
    QLabel*             status_;
    QLabel*             cursorStatus_;
    QAction*            stopAction_;
    QAction*            themeActions_[3] = {nullptr, nullptr, nullptr};   // indexed by Theme::Mode
};

} // namespace revstudio

#endif
