#ifndef REVSTUDIO_PREFERENCESDIALOG_H
#define REVSTUDIO_PREFERENCESDIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLineEdit;
class QSpinBox;

namespace revstudio {

class Settings;

/**
 * G7 (GUI_Implementation_Note.md, section 12.3's task table; "font, theme, `rb` path, explorer options"). A
 * plain modal settings editor over `Settings` -- it reads the current values in its constructor and, only on
 * accept(), writes all of them back. It does not touch any other widget directly: MainWindow::openPreferences()
 * re-applies everything live (theme, font, the file explorer's "watch for changes") after a successful exec(),
 * the same way it already re-applies the theme from the View menu.
 *
 * The backend path (`rb`) is the one field that does NOT take effect immediately, matching how `--rb` has always
 * worked: BackendLocator only runs at startup, so a changed path here is picked up the next time RevStudio starts,
 * not by restarting the current backend (Session::restart() reuses the program path from the LAST start() call,
 * which would still be the old one). Said so directly in the dialog rather than silently doing nothing until the
 * user notices on their own.
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(Settings* settings, QWidget* parent = nullptr);

public slots:
    void accept() override;

private:
    Settings* settings_;

    QComboBox*        themeCombo_;
    QFontComboBox*    fontFamilyCombo_;
    QSpinBox*         fontSizeSpin_;
    QLineEdit*        backendPathEdit_;
    QCheckBox*        watchForChangesCheck_;
};

} // namespace revstudio

#endif
