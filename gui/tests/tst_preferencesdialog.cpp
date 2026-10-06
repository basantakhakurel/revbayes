#include "app/PreferencesDialog.h"

#include "app/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace revstudio;

namespace {

struct TempSettings
{
    QTemporaryDir dir;
    QSettings     backing;
    Settings      settings;

    TempSettings() : backing(dir.path() + QStringLiteral("/revstudio-test.ini"), QSettings::IniFormat),
                     settings(&backing)
    {
    }
};

} // namespace

class TstPreferencesDialog : public QObject
{
    Q_OBJECT

private slots:
    void loadsTheCurrentSettingsOnConstruction()
    {
        // An arbitrary-but-unavailable family name (e.g. "Courier New" on a machine without it) would round-trip
        // through QFontComboBox as whatever substitute Qt's own font matching picks instead, which has nothing
        // to do with this dialog -- so this resolves the same alias Settings::editorFont() itself falls back to
        // ("monospace" on this Linux fontconfig setup) to the concrete family QFontComboBox will actually match
        // it to (e.g. "DejaVu Sans Mono"), the same resolution QFontInfo performs.
        const QString availableFamily =
            QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();

        TempSettings t;
        t.settings.setThemeMode(Theme::Mode::Dark);
        QFont font(availableFamily);
        font.setPointSize(16);
        t.settings.setEditorFont(font);
        t.settings.setBackendPath(QStringLiteral("/opt/revbayes/bin/rb"));
        t.settings.setExplorerWatchForChanges(false);

        PreferencesDialog dialog(&t.settings);

        QCOMPARE(dialog.findChild<QComboBox*>(QStringLiteral("preferencesTheme"))->currentIndex(),
                 static_cast<int>(Theme::Mode::Dark));
        QCOMPARE(dialog.findChild<QFontComboBox*>(QStringLiteral("preferencesFontFamily"))->currentFont().family(),
                 availableFamily);
        QCOMPARE(dialog.findChild<QSpinBox*>(QStringLiteral("preferencesFontSize"))->value(), 16);
        QCOMPARE(dialog.findChild<QLineEdit*>(QStringLiteral("preferencesBackendPath"))->text(),
                 QStringLiteral("/opt/revbayes/bin/rb"));
        QVERIFY(!dialog.findChild<QCheckBox*>(QStringLiteral("preferencesWatchForChanges"))->isChecked());
    }

    void acceptingWritesEveryFieldBackToSettings()
    {
        TempSettings t;
        PreferencesDialog dialog(&t.settings);

        dialog.findChild<QComboBox*>(QStringLiteral("preferencesTheme"))->setCurrentIndex(static_cast<int>(Theme::Mode::Light));
        dialog.findChild<QFontComboBox*>(QStringLiteral("preferencesFontFamily"))->setCurrentFont(QFont(QStringLiteral("Monospace")));
        dialog.findChild<QSpinBox*>(QStringLiteral("preferencesFontSize"))->setValue(18);
        dialog.findChild<QLineEdit*>(QStringLiteral("preferencesBackendPath"))->setText(QStringLiteral("/usr/local/bin/rb"));
        dialog.findChild<QCheckBox*>(QStringLiteral("preferencesWatchForChanges"))->setChecked(false);

        dialog.accept();

        QCOMPARE(t.settings.themeMode(), Theme::Mode::Light);
        QCOMPARE(t.settings.editorFont().family(), QStringLiteral("Monospace"));
        QCOMPARE(t.settings.editorFont().pointSize(), 18);
        QCOMPARE(t.settings.backendPath(), QStringLiteral("/usr/local/bin/rb"));
        QVERIFY(!t.settings.explorerWatchForChanges());
    }

    void rejectingLeavesSettingsUntouched()
    {
        TempSettings t;
        t.settings.setBackendPath(QStringLiteral("/opt/revbayes/bin/rb"));

        PreferencesDialog dialog(&t.settings);
        dialog.findChild<QLineEdit*>(QStringLiteral("preferencesBackendPath"))->setText(QStringLiteral("/somewhere/else/rb"));
        dialog.reject();

        QCOMPARE(t.settings.backendPath(), QStringLiteral("/opt/revbayes/bin/rb"));
    }

    // Settings::backendPath() has no built-in trimming; the dialog's own accept() does it once, here, so a
    // path pasted with trailing whitespace does not quietly fail BackendLocator's later exact-path checks.
    void backendPathIsTrimmedOnAccept()
    {
        TempSettings t;
        PreferencesDialog dialog(&t.settings);
        dialog.findChild<QLineEdit*>(QStringLiteral("preferencesBackendPath"))->setText(QStringLiteral("  /usr/local/bin/rb  "));
        dialog.accept();

        QCOMPARE(t.settings.backendPath(), QStringLiteral("/usr/local/bin/rb"));
    }
};

QTEST_MAIN(TstPreferencesDialog)
#include "tst_preferencesdialog.moc"
