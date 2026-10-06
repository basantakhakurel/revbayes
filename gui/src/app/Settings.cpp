#include "app/Settings.h"

#include <QFontDatabase>

namespace revstudio {

namespace {
constexpr auto keyBackendPath         = "backend/path";
constexpr auto keyThemeMode           = "theme/mode";
constexpr auto keyWindowGeometry      = "window/geometry";
constexpr auto keyWindowState         = "window/state";
constexpr auto keyRecentFiles         = "editor/recentFiles";
constexpr auto keyOpenDocuments       = "editor/openDocuments";
constexpr auto keyEditorFontFamily    = "editor/fontFamily";
constexpr auto keyEditorFontPointSize = "editor/fontPointSize";
constexpr auto keyExplorerShowHidden  = "explorer/showHiddenByDefault";
constexpr auto keyExplorerWatch       = "explorer/watchForChanges";
constexpr int  maxRecentFiles         = 10;
constexpr int  defaultFontPointSize   = 11;   // section 6.11
}

Settings::Settings(QSettings* backing)
{
    if (backing)
    {
        settings_ = backing;
    }
    else
    {
        owned_    = std::make_unique<QSettings>();
        settings_ = owned_.get();
    }
}

QString Settings::backendPath() const
{
    return settings_->value(QLatin1String(keyBackendPath)).toString();
}

void Settings::setBackendPath(const QString& path)
{
    settings_->setValue(QLatin1String(keyBackendPath), path);
}

Theme::Mode Settings::themeMode() const
{
    const int stored = settings_->value(QLatin1String(keyThemeMode), static_cast<int>(Theme::Mode::System)).toInt();
    if (stored == static_cast<int>(Theme::Mode::Light) || stored == static_cast<int>(Theme::Mode::Dark))
    {
        return static_cast<Theme::Mode>(stored);
    }
    return Theme::Mode::System;   // also the fallback for a value from a future version this build does not know
}

void Settings::setThemeMode(Theme::Mode mode)
{
    settings_->setValue(QLatin1String(keyThemeMode), static_cast<int>(mode));
}

QByteArray Settings::windowGeometry() const
{
    return settings_->value(QLatin1String(keyWindowGeometry)).toByteArray();
}

void Settings::setWindowGeometry(const QByteArray& geometry)
{
    settings_->setValue(QLatin1String(keyWindowGeometry), geometry);
}

QByteArray Settings::windowState() const
{
    return settings_->value(QLatin1String(keyWindowState)).toByteArray();
}

void Settings::setWindowState(const QByteArray& state)
{
    settings_->setValue(QLatin1String(keyWindowState), state);
}

QStringList Settings::recentFiles() const
{
    return settings_->value(QLatin1String(keyRecentFiles)).toStringList();
}

void Settings::addRecentFile(const QString& path)
{
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > maxRecentFiles)
    {
        files.removeLast();
    }
    settings_->setValue(QLatin1String(keyRecentFiles), files);
}

QStringList Settings::openDocuments() const
{
    return settings_->value(QLatin1String(keyOpenDocuments)).toStringList();
}

void Settings::setOpenDocuments(const QStringList& paths)
{
    settings_->setValue(QLatin1String(keyOpenDocuments), paths);
}

QFont Settings::editorFont() const
{
    QFont fallback = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fallback.setPointSize(defaultFontPointSize);

    QFont font(settings_->value(QLatin1String(keyEditorFontFamily), fallback.family()).toString());
    const int pointSize = settings_->value(QLatin1String(keyEditorFontPointSize), fallback.pointSize()).toInt();
    font.setPointSize(pointSize > 0 ? pointSize : fallback.pointSize());
    return font;
}

void Settings::setEditorFont(const QFont& font)
{
    settings_->setValue(QLatin1String(keyEditorFontFamily), font.family());
    settings_->setValue(QLatin1String(keyEditorFontPointSize), font.pointSize());
}

bool Settings::explorerShowHiddenByDefault() const
{
    return settings_->value(QLatin1String(keyExplorerShowHidden), false).toBool();
}

void Settings::setExplorerShowHiddenByDefault(bool show)
{
    settings_->setValue(QLatin1String(keyExplorerShowHidden), show);
}

bool Settings::explorerWatchForChanges() const
{
    return settings_->value(QLatin1String(keyExplorerWatch), true).toBool();
}

void Settings::setExplorerWatchForChanges(bool watch)
{
    settings_->setValue(QLatin1String(keyExplorerWatch), watch);
}

} // namespace revstudio
