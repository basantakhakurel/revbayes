#include "app/RevString.h"

#include <QtTest>

using namespace revstudio;

class TstRevString : public QObject
{
    Q_OBJECT

private slots:
    void aPlainPathIsJustQuoted()
    {
        QCOMPARE(RevString::quote(QStringLiteral("/home/user/script.Rev")), QStringLiteral("\"/home/user/script.Rev\""));
    }

    void backslashesAreDoubled()
    {
        QCOMPARE(RevString::quote(QStringLiteral("C:\\Users\\foo.Rev")), QStringLiteral("\"C:\\\\Users\\\\foo.Rev\""));
    }

    void embeddedQuotesAreEscaped()
    {
        // D15: the old code never did this at all.
        QCOMPARE(RevString::quote(QStringLiteral("a \"quoted\" word.txt")), QStringLiteral("\"a \\\"quoted\\\" word.txt\""));
    }

    void backslashesAndQuotesTogetherEscapeCorrectly()
    {
        QCOMPARE(RevString::quote(QStringLiteral("C:\\path \"with\" quotes")),
                QStringLiteral("\"C:\\\\path \\\"with\\\" quotes\""));
    }
};

QTEST_MAIN(TstRevString)
#include "tst_revstring.moc"
