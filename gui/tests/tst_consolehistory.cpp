#include "app/ConsoleHistory.h"

#include <QtTest>

using namespace revstudio;

class TstConsoleHistory : public QObject
{
    Q_OBJECT

private slots:
    void emptyHistoryGivesNothing()
    {
        ConsoleHistory history;
        QVERIFY(!history.up(QStringLiteral("draft")).has_value());
        QVERIFY(!history.down().has_value());
    }

    void d9_upThenDownReturnsToTheNewestEntryNotTheOldest()
    {
        // The exact regression from GUI_Implementation_Note.md's D9: history "a b c", Up gives c then b, and
        // Down must give c back (the newer one), not a (the older one, which is what the old GTK console did).
        ConsoleHistory history;
        history.add(QStringLiteral("a"));
        history.add(QStringLiteral("b"));
        history.add(QStringLiteral("c"));

        QCOMPARE(history.up(QStringLiteral("")), std::optional<QString>(QStringLiteral("c")));
        QCOMPARE(history.up(QStringLiteral("")), std::optional<QString>(QStringLiteral("b")));
        QCOMPARE(history.down(), std::optional<QString>(QStringLiteral("c")));
    }

    void upStopsAtTheOldestEntry()
    {
        ConsoleHistory history;
        history.add(QStringLiteral("a"));
        history.add(QStringLiteral("b"));

        QCOMPARE(history.up(QStringLiteral("")), std::optional<QString>(QStringLiteral("b")));
        QCOMPARE(history.up(QStringLiteral("")), std::optional<QString>(QStringLiteral("a")));
        QVERIFY(!history.up(QStringLiteral("")).has_value());   // already at the oldest: no further change
        QVERIFY(history.isBrowsing());
    }

    void downPastTheNewestEntryRestoresTheDraftAndEndsBrowsing()
    {
        ConsoleHistory history;
        history.add(QStringLiteral("a"));

        history.up(QStringLiteral("unsent draft"));
        QVERIFY(history.isBrowsing());
        QCOMPARE(history.down(), std::optional<QString>(QStringLiteral("unsent draft")));
        QVERIFY(!history.isBrowsing());
    }

    void downWithoutBrowsingDoesNothing()
    {
        ConsoleHistory history;
        history.add(QStringLiteral("a"));
        QVERIFY(!history.down().has_value());
    }

    void addEndsBrowsingAndIgnoresBlankOrImmediatelyRepeatedEntries()
    {
        ConsoleHistory history;
        history.add(QStringLiteral("a"));
        history.add(QStringLiteral(""));     // ignored
        history.add(QStringLiteral("a"));    // ignored: same as the immediately preceding entry
        QCOMPARE(history.size(), 1);

        history.up(QStringLiteral(""));
        QVERIFY(history.isBrowsing());
        history.add(QStringLiteral("b"));
        QVERIFY(!history.isBrowsing());
        QCOMPARE(history.size(), 2);
    }

    void capacityDropsTheOldestEntriesFirst()
    {
        ConsoleHistory history(2);
        history.add(QStringLiteral("a"));
        history.add(QStringLiteral("b"));
        history.add(QStringLiteral("c"));
        QCOMPARE(history.entries(), QStringList({QStringLiteral("b"), QStringLiteral("c")}));
    }

    void setEntriesLoadsPersistedHistoryAndRespectsCapacity()
    {
        ConsoleHistory history(2);
        history.setEntries({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")});
        QCOMPARE(history.entries(), QStringList({QStringLiteral("y"), QStringLiteral("z")}));
        QCOMPARE(history.up(QStringLiteral("")), std::optional<QString>(QStringLiteral("z")));
    }
};

QTEST_MAIN(TstConsoleHistory)
#include "tst_consolehistory.moc"
