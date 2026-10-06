#ifndef REVSTUDIO_FILEEXPLORERPANEL_H
#define REVSTUDIO_FILEEXPLORERPANEL_H

#include <QWidget>

class QCheckBox;
class QFileSystemModel;
class QLineEdit;
class QModelIndex;
class QPoint;
class QTreeView;

namespace revstudio {

class ConsoleWidget;
class EditorTabs;
class FileExplorerFilterProxyModel;
class Session;
class Settings;

/**
 * G5/G6: the file explorer dock (GUI_Implementation_Note.md, section 6.7) -- a `QFileSystemModel` tree rooted at
 * the backend's working directory (from `done.cwd`/`variables.cwd`; section 4.7 notes the backend's cwd is
 * process-global and the explorer assumes it runs on the same machine/filesystem as RevStudio), a filter box, a
 * "show hidden files" toggle, and a context menu. Like VariablesPanel, every action that runs Rev code
 * (`setwd(...)`, `source(...)`) goes through the console (`ConsoleWidget::submitText`) rather than
 * `Session::submit()` directly, so it shows up in the transcript and history exactly as if typed.
 *
 * Double-click opens any file in a new editor tab (`EditorTabs::openFile`); `.Rev` files additionally offer
 * *Run* in the context menu. Double-click used to run `.Rev` files directly, as a stand-in from G5 when no
 * editor existed yet to open anything into -- now that EditorTabs exists (G6), double-click matches the spec's
 * actual "opens... in a new editor tab" instead.
 *
 * G7/Preferences: "Hidden files" persists its own state directly (it is already a live, always-visible control,
 * so there is nothing for a dialog to add); "watch for external changes" (6.7's `QFileSystemModel::
 * DontWatchForChanges`, for slow/network drives) has no control of its own here and lives only in Preferences,
 * applied through applyPreferences().
 */
class FileExplorerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FileExplorerPanel(Session* session, ConsoleWidget* console, EditorTabs* editor, Settings* settings,
                                QWidget* parent = nullptr);

    /** G7/Preferences: re-reads "watch for external changes" (6.7's `QFileSystemModel::DontWatchForChanges`).
     *  "Hidden files" is not here -- its own checkbox already persists its state directly, see the constructor. */
    void applyPreferences();

private:
    void setRootPath(const QString& path);
    void onDoubleClicked(const QModelIndex& proxyIndex);
    void showContextMenu(const QPoint& position);
    QString pathAt(const QModelIndex& proxyIndex) const;
    static bool isRevFile(const QString& path);

    Session*       session_;
    ConsoleWidget*  console_;
    EditorTabs*     editor_;
    Settings*       settings_;

    QFileSystemModel*             model_;
    FileExplorerFilterProxyModel* proxy_;
    QTreeView*                    tree_;
    QLineEdit*                    filterEdit_;
    QCheckBox*                    showHiddenCheck_;

    QString currentRoot_;
};

} // namespace revstudio

#endif
