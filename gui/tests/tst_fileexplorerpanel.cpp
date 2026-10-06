#include "app/FileExplorerPanel.h"

#include "app/ConsoleWidget.h"
#include "app/EditorTabs.h"
#include "app/Settings.h"
#include "backend/Session.h"

#include <QSettings>

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
constexpr int waitMs = 10000;

void touch(const QString& path)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
}

QModelIndex findRow(QAbstractItemView* view, const QString& name)
{
    QAbstractItemModel* model = view->model();
    const QModelIndex root = view->rootIndex();
    for (int i = 0; i < model->rowCount(root); ++i)
    {
        const QModelIndex idx = model->index(i, 0, root);
        if (model->data(idx).toString() == name)
        {
            return idx;
        }
    }
    return QModelIndex();
}

} // namespace

class TstFileExplorerPanel : public QObject
{
    Q_OBJECT

private slots:
    void rootFollowsTheBackendCwdAndListsItsEntries()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/script.Rev"));
        touch(dir.path() + QStringLiteral("/notes.txt"));
        touch(dir.path() + QStringLiteral("/.hidden"));

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs editor(&console, &settings);
        FileExplorerPanel panel(&session, &console, &editor, &settings);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(findRow(tree, QStringLiteral("script.Rev")).isValid(), waitMs);
        QVERIFY(findRow(tree, QStringLiteral("notes.txt")).isValid());
        QVERIFY(!findRow(tree, QStringLiteral(".hidden")).isValid());   // dotfiles hidden by default (6.7)

        auto* showHidden = panel.findChild<QCheckBox*>(QStringLiteral("explorerShowHidden"));
        QVERIFY(showHidden);
        showHidden->setChecked(true);
        QVERIFY(findRow(tree, QStringLiteral(".hidden")).isValid());

        session.shutdown();
    }

    void theFilterBoxNarrowsTheList()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/alpha.Rev"));
        touch(dir.path() + QStringLiteral("/beta.txt"));

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs editor(&console, &settings);
        FileExplorerPanel panel(&session, &console, &editor, &settings);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        QTRY_VERIFY_WITH_TIMEOUT(findRow(tree, QStringLiteral("alpha.Rev")).isValid(), waitMs);

        auto* filter = panel.findChild<QLineEdit*>(QStringLiteral("explorerFilter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("alpha"));
        QVERIFY(findRow(tree, QStringLiteral("alpha.Rev")).isValid());
        QVERIFY(!findRow(tree, QStringLiteral("beta.txt")).isValid());

        session.shutdown();
    }

    // Section 6.7: double-click opens a file in a new editor tab; Run is a separate, context-menu-only action
    // (covered elsewhere) now that EditorTabs (G6) exists to open something into -- G5's original stand-in,
    // running a .Rev file directly on double-click, is gone.
    void doubleClickingAFileOpensItInTheEditor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/script.Rev"));

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs editor(&console, &settings);
        FileExplorerPanel panel(&session, &console, &editor, &settings);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        QModelIndex scriptIndex;
        QTRY_VERIFY_WITH_TIMEOUT((scriptIndex = findRow(tree, QStringLiteral("script.Rev"))).isValid(), waitMs);

        const int tabsBefore = editor.count();
        emit tree->doubleClicked(scriptIndex);

        QCOMPARE(editor.count(), tabsBefore + 1);
        const QString expectedPath = QDir(dir.path()).filePath(QStringLiteral("script.Rev"));
        QCOMPARE(editor.openFilePaths(), QStringList({expectedPath}));

        session.shutdown();
    }

    // G7/Preferences: the checkbox persists its own state directly, so a fresh panel picks it back up without
    // needing to be told through Preferences -- see the class comment on why this one option has no dialog field.
    void showHiddenCheckboxStartsFromItsPersistedDefault()
    {
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        settings.setExplorerShowHiddenByDefault(true);

        Session session;
        ConsoleWidget console(&session);
        EditorTabs editor(&console, &settings);
        FileExplorerPanel panel(&session, &console, &editor, &settings);

        auto* showHidden = panel.findChild<QCheckBox*>(QStringLiteral("explorerShowHidden"));
        QVERIFY(showHidden->isChecked());

        showHidden->setChecked(false);
        QVERIFY(!settings.explorerShowHiddenByDefault());   // toggling it updates the persisted default live
    }

    // 6.7: "on slow network drives set QFileSystemModel::DontWatchForChanges (option in Preferences)."
    void watchForChangesComesFromSettingsAndApplyPreferencesReappliesIt()
    {
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        settings.setExplorerWatchForChanges(false);

        Session session;
        ConsoleWidget console(&session);
        EditorTabs editor(&console, &settings);
        FileExplorerPanel panel(&session, &console, &editor, &settings);

        auto* model = panel.findChild<QFileSystemModel*>(QStringLiteral("explorerModel"));
        QVERIFY(model->testOption(QFileSystemModel::DontWatchForChanges));

        settings.setExplorerWatchForChanges(true);
        panel.applyPreferences();
        QVERIFY(!model->testOption(QFileSystemModel::DontWatchForChanges));
    }
};

QTEST_MAIN(TstFileExplorerPanel)
#include "tst_fileexplorerpanel.moc"
