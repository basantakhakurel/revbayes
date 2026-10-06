#include "app/Settings.h"

#include <QFontDatabase>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace revstudio;

namespace {

/** A Settings backed by a temp-file QSettings, never the real user's settings file. The temp dir must outlive
 *  the QSettings (which flushes to disk on destruction), so both are kept together here. */
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

class TstSettings : public QObject
{
    Q_OBJECT

private slots:
    void backendPathDefaultsToEmpty()
    {
        TempSettings t;
        QVERIFY(t.settings.backendPath().isEmpty());
    }

    void backendPathRoundTrips()
    {
        TempSettings t;
        t.settings.setBackendPath(QStringLiteral("/opt/revbayes/bin/rb"));
        QCOMPARE(t.settings.backendPath(), QStringLiteral("/opt/revbayes/bin/rb"));
    }

    void themeModeDefaultsToSystem()
    {
        TempSettings t;
        QCOMPARE(t.settings.themeMode(), Theme::Mode::System);
    }

    void themeModeRoundTrips()
    {
        TempSettings t;
        t.settings.setThemeMode(Theme::Mode::Dark);
        QCOMPARE(t.settings.themeMode(), Theme::Mode::Dark);

        t.settings.setThemeMode(Theme::Mode::Light);
        QCOMPARE(t.settings.themeMode(), Theme::Mode::Light);
    }

    void themeModeFallsBackToSystemForAnUnrecognisedStoredValue()
    {
        TempSettings t;
        t.backing.setValue(QStringLiteral("theme/mode"), 99);   // not a value this build's Theme::Mode knows
        QCOMPARE(t.settings.themeMode(), Theme::Mode::System);
    }

    void windowGeometryAndStateRoundTrip()
    {
        TempSettings t;
        QVERIFY(t.settings.windowGeometry().isEmpty());
        QVERIFY(t.settings.windowState().isEmpty());

        const QByteArray geometry = "pretend-geometry-bytes";
        const QByteArray state    = "pretend-state-bytes";
        t.settings.setWindowGeometry(geometry);
        t.settings.setWindowState(state);
        QCOMPARE(t.settings.windowGeometry(), geometry);
        QCOMPARE(t.settings.windowState(), state);
    }

    void editorFontDefaultsToElevenPointFixedFont()
    {
        TempSettings t;
        const QFont font = t.settings.editorFont();
        QCOMPARE(font.pointSize(), 11);   // section 6.11: "11 pt by default"
        QCOMPARE(font.family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }

    void editorFontRoundTrips()
    {
        TempSettings t;
        QFont font(QStringLiteral("Courier New"));
        font.setPointSize(14);
        t.settings.setEditorFont(font);

        const QFont stored = t.settings.editorFont();
        QCOMPARE(stored.family(), QStringLiteral("Courier New"));
        QCOMPARE(stored.pointSize(), 14);
    }

    void explorerPreferencesDefaultAndRoundTrip()
    {
        TempSettings t;
        QVERIFY(!t.settings.explorerShowHiddenByDefault());
        QVERIFY(t.settings.explorerWatchForChanges());   // the positive sense: watching is the normal case

        t.settings.setExplorerShowHiddenByDefault(true);
        t.settings.setExplorerWatchForChanges(false);
        QVERIFY(t.settings.explorerShowHiddenByDefault());
        QVERIFY(!t.settings.explorerWatchForChanges());
    }

    void twoSettingsOverTheSameFileSeeEachOthersWrites()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/shared.ini");

        QSettings backingA(path, QSettings::IniFormat);
        Settings  a(&backingA);
        a.setBackendPath(QStringLiteral("/usr/local/bin/rb"));
        backingA.sync();

        QSettings backingB(path, QSettings::IniFormat);
        Settings  b(&backingB);
        QCOMPARE(b.backendPath(), QStringLiteral("/usr/local/bin/rb"));
    }
};

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
