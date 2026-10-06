#include "app/VariablesModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using namespace revstudio;

namespace {

QJsonObject row(const QString& name, const QString& type, const QString& kind, const QString& summary,
                 const QStringList& flags = {}, const QString& parent = QString())
{
    QJsonObject object{{QStringLiteral("name"), name}, {QStringLiteral("type"), type},
                       {QStringLiteral("kind"), kind}, {QStringLiteral("summary"), summary},
                       {QStringLiteral("flags"), QJsonArray::fromStringList(flags)}};
    if (!parent.isEmpty())
    {
        object.insert(QStringLiteral("parent"), parent);
    }
    return object;
}

} // namespace

class TstVariablesModel : public QObject
{
    Q_OBJECT

private slots:
    void aFreshSnapshotPopulatesTopLevelRows()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1.5"))});

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex a = model.index(0, VariablesModel::NameColumn);
        QCOMPARE(model.data(a).toString(), QStringLiteral("a"));
        QCOMPARE(model.data(model.index(0, VariablesModel::TypeColumn)).toString(), QStringLiteral("RealPos"));
        QCOMPARE(model.data(model.index(0, VariablesModel::ValueColumn)).toString(), QStringLiteral("1.5"));
        QCOMPARE(model.data(a, VariablesModel::KindRole).toString(), QStringLiteral("constant"));
    }

    void elementVariablesNestUnderTheirParent()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({
            row(QStringLiteral("x"), QStringLiteral("Natural[]"), QStringLiteral("constant"), QStringLiteral("Natural[2]")),
            row(QStringLiteral("x[1]"), QStringLiteral("Natural"), QStringLiteral("constant"), QStringLiteral("1"),
                {QStringLiteral("element")}, QStringLiteral("x")),
            row(QStringLiteral("x[2]"), QStringLiteral("Natural"), QStringLiteral("constant"), QStringLiteral("2"),
                {QStringLiteral("element")}, QStringLiteral("x")),
        });

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex x = model.index(0, VariablesModel::NameColumn);
        QCOMPARE(model.rowCount(x), 2);
        QCOMPARE(model.data(model.index(0, VariablesModel::NameColumn, x)).toString(), QStringLiteral("x[1]"));
        QCOMPARE(model.data(model.index(1, VariablesModel::NameColumn, x)).toString(), QStringLiteral("x[2]"));
        QCOMPARE(model.parent(model.index(0, 0, x)), x);
    }

    void unchangedRowsAreUpdatedInPlaceNotResetOrReordered()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1.5")),
                             row(QStringLiteral("b"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("2.5"))});

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1.5")),
                             row(QStringLiteral("b"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("9.0"))});

        QCOMPARE(reset.count(), 0);                      // no wholesale reset: a view's selection/scroll survives
        QVERIFY(changed.count() >= 1);                    // "b" changed, so at least one dataChanged fired
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, VariablesModel::NameColumn)).toString(), QStringLiteral("a"));   // order kept
        QCOMPARE(model.data(model.index(1, VariablesModel::ValueColumn)).toString(), QStringLiteral("9.0"));
    }

    void aRemovedVariableIsRemovedAndSurvivorsKeepTheirPositions()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1")),
                             row(QStringLiteral("b"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("2")),
                             row(QStringLiteral("c"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("3"))});

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1")),
                             row(QStringLiteral("c"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("3"))});

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, VariablesModel::NameColumn)).toString(), QStringLiteral("a"));
        QCOMPARE(model.data(model.index(1, VariablesModel::NameColumn)).toString(), QStringLiteral("c"));
    }

    void anInsertedVariableAppearsAtItsSortedPosition()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1")),
                             row(QStringLiteral("c"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("3"))});

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1")),
                             row(QStringLiteral("b"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("2")),
                             row(QStringLiteral("c"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("3"))});

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(1, VariablesModel::NameColumn)).toString(), QStringLiteral("b"));
    }

    void aChangedSummaryIsHighlightedAndFadesOut()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1"))});
        QVERIFY(!model.data(model.index(0, 0), Qt::BackgroundRole).isValid());   // nothing changed yet: no highlight

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("2"))});
        QVERIFY(model.data(model.index(0, 0), Qt::BackgroundRole).isValid());    // just changed: highlighted

        QTRY_VERIFY_WITH_TIMEOUT(!model.data(model.index(0, 0), Qt::BackgroundRole).isValid(), 2000);   // faded out
    }

    void clearRemovesEverything()
    {
        VariablesModel model;
        new QAbstractItemModelTester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest, &model);

        model.applySnapshot({row(QStringLiteral("a"), QStringLiteral("RealPos"), QStringLiteral("constant"), QStringLiteral("1"))});
        model.clear();
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_MAIN(TstVariablesModel)
#include "tst_variablesmodel.moc"
