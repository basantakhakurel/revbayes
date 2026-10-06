#include "app/VariablesPanel.h"

#include "app/ConsoleWidget.h"
#include "backend/Session.h"

#include <QCheckBox>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTreeView>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
const QString transcriptDir = QStringLiteral(TRANSCRIPT_DIR);
constexpr int waitMs = 10000;

/** Finds the row with `name` in NameColumn under `parent`, searching only one level deep (this project's tree
 *  is never deeper than that). Returns an invalid index if not found -- including "filtered out", which is
 *  exactly what some of the tests below want to check. */
QModelIndex findRow(QAbstractItemView* view, const QString& name, const QModelIndex& parent = QModelIndex())
{
    QAbstractItemModel* model = view->model();
    for (int i = 0; i < model->rowCount(parent); ++i)
    {
        const QModelIndex idx = model->index(i, 0, parent);
        if (model->data(idx).toString() == name)
        {
            return idx;
        }
    }
    return QModelIndex();
}

} // namespace

class TstVariablesPanel : public QObject
{
    Q_OBJECT

private slots:
    void readyPopulatesVariablesAndFunctions()
    {
        Session session;
        ConsoleWidget console(&session);
        VariablesPanel panel(&session, &console);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/variables_panel.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("variablesTree"));
        QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(findRow(tree, QStringLiteral("x")).isValid(), waitMs);

        auto* functionsTree = panel.findChild<QTreeView*>(QStringLiteral("functionsTree"));
        QVERIFY(functionsTree);
        QTRY_COMPARE_WITH_TIMEOUT(functionsTree->model()->rowCount(), 1, waitMs);
        QCOMPARE(functionsTree->model()->index(0, 0).data().toString(), QStringLiteral("f"));

        session.shutdown();
    }

    void systemVariablesAreHiddenUntilTheToggleIsChecked()
    {
        Session session;
        ConsoleWidget console(&session);
        VariablesPanel panel(&session, &console);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/variables_panel.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("variablesTree"));
        QTRY_VERIFY_WITH_TIMEOUT(findRow(tree, QStringLiteral("x")).isValid(), waitMs);
        QVERIFY(!findRow(tree, QStringLiteral("args")).isValid());   // system: hidden by default (6.6)

        auto* showHidden = panel.findChild<QCheckBox*>(QStringLiteral("variablesShowHidden"));
        QVERIFY(showHidden);
        showHidden->setChecked(true);
        QVERIFY(findRow(tree, QStringLiteral("args")).isValid());

        session.shutdown();
    }

    void selectingARowRequestsInspectAndFillsTheDetailsPane()
    {
        Session session;
        ConsoleWidget console(&session);
        VariablesPanel panel(&session, &console);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy inspected(&session, &Session::inspected);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/variables_panel.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("variablesTree"));
        QModelIndex xIndex;
        QTRY_VERIFY_WITH_TIMEOUT((xIndex = findRow(tree, QStringLiteral("x"))).isValid(), waitMs);

        tree->selectionModel()->setCurrentIndex(xIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.count(), 1, waitMs);

        auto* details = panel.findChild<QPlainTextEdit*>(QStringLiteral("variablesDetails"));
        QVERIFY(details);
        QTRY_VERIFY_WITH_TIMEOUT(details->toPlainText().contains(QStringLiteral("1.5")), waitMs);

        session.shutdown();
    }

    void doubleClickInsertsTheNameIntoTheConsole()
    {
        Session session;
        ConsoleWidget console(&session);
        VariablesPanel panel(&session, &console);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/variables_panel.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("variablesTree"));
        QModelIndex xIndex;
        QTRY_VERIFY_WITH_TIMEOUT((xIndex = findRow(tree, QStringLiteral("x"))).isValid(), waitMs);

        emit tree->doubleClicked(xIndex);

        auto* input = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleInput"));
        QVERIFY(input);
        QCOMPARE(input->toPlainText(), QStringLiteral("x"));

        session.shutdown();
    }

    // Exercises the same `clear(x)` -> `done` -> functions-refresh sequence the context menu's Remove action
    // triggers (via ConsoleWidget::submitText, exactly as that action calls it) without needing to drive an
    // actual QMenu::exec() -- the action itself is a one-line call to the method this test calls directly.
    void removingAVariableRefreshesBothTabs()
    {
        Session session;
        ConsoleWidget console(&session);
        VariablesPanel panel(&session, &console);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/variables_panel.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* tree = panel.findChild<QTreeView*>(QStringLiteral("variablesTree"));
        QModelIndex xIndex;
        QTRY_VERIFY_WITH_TIMEOUT((xIndex = findRow(tree, QStringLiteral("x"))).isValid(), waitMs);

        // The shared transcript's steps are a fixed sequence (a fresh mock-rb process per test, but one script);
        // this test's "clear(x)" is step 4, so step 3 (inspect, from selecting "x") must happen first, same as a
        // real user would naturally do before deciding to remove something.
        QSignalSpy inspected(&session, &Session::inspected);
        tree->selectionModel()->setCurrentIndex(xIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.count(), 1, waitMs);

        console.submitText(QStringLiteral("clear(x)"));
        QTRY_VERIFY_WITH_TIMEOUT(!findRow(tree, QStringLiteral("x")).isValid(), waitMs);

        auto* output = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("> clear(x)")));   // echoed, as section 6.7 specifies

        // The functions re-request the submit above triggered (see the transcript file's comment) must have
        // already been answered before shutdown, or it would be the transcript's next expected step instead.
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QTreeView*>(QStringLiteral("functionsTree"))->model()->rowCount() == 1, waitMs);

        session.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
    }
};

QTEST_MAIN(TstVariablesPanel)
#include "tst_variablespanel.moc"
