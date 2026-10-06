#ifndef REVSTUDIO_SETTINGS_H
#define REVSTUDIO_SETTINGS_H

#include "app/Theme.h"

#include <QByteArray>
#include <QFont>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

namespace revstudio {

/**
 * Thin, typed wrapper over QSettings for everything RevStudio persists between runs: the window layout, the
 * theme, and the backend path remembered from an explicit `--rb` (BackendLocator's "the saved setting", step 2
 * of GUI_Implementation_Note.md section 10.3/6.5).
 *
 * With no argument, backs onto QSettings' own default constructor, which resolves a per-user settings file from
 * QCoreApplication::organizationName()/applicationName() (set once in main.cpp) -- the normal, persistent case.
 * A test passes its own QSettings (pointed at a temp file) instead, so tests never touch the real user's
 * settings and never interfere with each other.
 */
class Settings
{
public:
    explicit Settings(QSettings* backing = nullptr);

    QString backendPath() const;
    void    setBackendPath(const QString& path);

    Theme::Mode themeMode() const;
    void        setThemeMode(Theme::Mode mode);

    QByteArray windowGeometry() const;
    void       setWindowGeometry(const QByteArray& geometry);

    QByteArray windowState() const;
    void       setWindowState(const QByteArray& state);

    /** Most-recently-opened first, capped, deduplicated. */
    QStringList recentFiles() const;
    void        addRecentFile(const QString& path);

    /** G6's (trimmed) session restore: which files were open last time, so they can be reopened fresh from
     *  disk -- not their unsaved content; see EditorTabs' class comment for why that part is out of scope. */
    QStringList openDocuments() const;
    void        setOpenDocuments(const QStringList& paths);

    /** The editor/console monospace font (section 6.11: "11 pt by default"). Stored as family + point size, not
     *  a serialized QFont, so it stays readable (and portable across QSettings backends/versions) in the
     *  underlying file. */
    QFont editorFont() const;
    void  setEditorFont(const QFont& font);

    /** G5/6.7's "Hidden files" checkbox remembers its own state as the default for next time. */
    bool explorerShowHiddenByDefault() const;
    void setExplorerShowHiddenByDefault(bool show);

    /** 6.7: "on slow network drives set QFileSystemModel::DontWatchForChanges (option in Preferences)." Stored
     *  in the positive sense (watching is the normal case) to avoid a double negative in the code that reads it. */
    bool explorerWatchForChanges() const;
    void setExplorerWatchForChanges(bool watch);

private:
    QSettings*                 settings_;   // == backing_, or owned_.get()
    std::unique_ptr<QSettings> owned_;
};

} // namespace revstudio

#endif
