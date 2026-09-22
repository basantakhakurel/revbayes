#include "backend/ProtocolCodec.h"

#include <QtTest>

using namespace revstudio;

class TstProtocolCodec : public QObject
{
    Q_OBJECT

private slots:
    void framer_splitsLines()
    {
        LineFramer framer;
        framer.append("one\ntwo\nthree\n");

        QByteArray line;
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("one"));
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("two"));
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("three"));
        QVERIFY(!framer.nextLine(&line));
        QCOMPARE(framer.pendingBytes(), qsizetype(0));
    }

    void framer_keepsAPartialLineUntilItsNewlineArrives()
    {
        LineFramer framer;
        QByteArray line;

        framer.append("hel");
        QVERIFY(!framer.nextLine(&line));
        QCOMPARE(framer.pendingBytes(), qsizetype(3));

        framer.append("lo\nwor");
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("hello"));
        QVERIFY(!framer.nextLine(&line));

        framer.append("ld\n");
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("world"));
    }

    void framer_dropsTheCarriageReturnOfCrLf()
    {
        LineFramer framer;
        framer.append("a\r\nb\n");
        QByteArray line;
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("a"));
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("b"));
    }

    void framer_worksOneByteAtATime()
    {
        const QByteArray input = "{\"a\":1}\n{\"b\":2}\n";
        LineFramer framer;
        QList<QByteArray> lines;
        QByteArray line;
        for (char c : input)
        {
            framer.append(QByteArray(1, c));
            while (framer.nextLine(&line))
            {
                lines << line;
            }
        }
        QCOMPARE(lines, (QList<QByteArray>{"{\"a\":1}", "{\"b\":2}"}));
    }

    void framer_emptyLinesAreReturnedAsEmpty()
    {
        LineFramer framer;
        framer.append("\n\nx\n");
        QByteArray line;
        QVERIFY(framer.nextLine(&line)); QVERIFY(line.isEmpty());
        QVERIFY(framer.nextLine(&line)); QVERIFY(line.isEmpty());
        QVERIFY(framer.nextLine(&line)); QCOMPARE(line, QByteArray("x"));
    }

    void framer_dropsAnOversizedLineAndCarriesOn()
    {
        LineFramer framer(8);
        framer.append("123456789012\nok\n");

        QByteArray line;
        QVERIFY(framer.nextLine(&line));
        QCOMPARE(line, QByteArray("ok"));                       // the long line is gone, the next one survives
        QVERIFY(framer.takeOverflow());
        QVERIFY(!framer.takeOverflow());                        // the flag is cleared by reading it
    }

    void framer_dropsAnOversizedLineThatArrivesInSeveralReads()
    {
        LineFramer framer(8);
        QByteArray line;

        framer.append("123456789012");                          // already too long and still no newline
        QVERIFY(!framer.nextLine(&line));
        QVERIFY(framer.takeOverflow());
        QCOMPARE(framer.pendingBytes(), qsizetype(0));          // memory is released, not accumulated

        framer.append("34567");                                 // still inside the same line
        QVERIFY(!framer.nextLine(&line));

        framer.append("\nok\n");                                // its newline, then a good line
        QVERIFY(framer.nextLine(&line));
        QCOMPARE(line, QByteArray("ok"));
    }

    void encodeRequest_isOneCompactLineEndingInANewline()
    {
        const QByteArray bytes = protocol::encodeRequest(QStringLiteral("submit"), 7,
                                                         QJsonObject{{"text", "a <- 1\nb <- \"x\""}});
        QVERIFY(bytes.endsWith('\n'));
        QCOMPARE(bytes.count('\n'), 1);                         // newlines inside strings are escaped

        QString error;
        const auto parsed = protocol::parseMessage(bytes.trimmed(), &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->value("cmd").toString(), QStringLiteral("submit"));
        QCOMPARE(parsed->value("id").toInteger(), qint64(7));
        QCOMPARE(parsed->value("text").toString(), QStringLiteral("a <- 1\nb <- \"x\""));
    }

    void parseMessage_acceptsAnObject()
    {
        QString error;
        const auto parsed = protocol::parseMessage("{\"ev\":\"pong\",\"re\":5}", &error);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->value("ev").toString(), QStringLiteral("pong"));
        QCOMPARE(parsed->value("re").toInteger(), qint64(5));
    }

    void parseMessage_roundTripsUnicode()
    {
        const QString path = QString::fromUtf8("/home/\xc3\xa9l\xc3\xa8ve/\xe3\x83\x87\xe3\x82\xa3\xe3\x83\xac\xe3\x82\xaf\xe3\x83\x88\xe3\x83\xaa");
        const QByteArray bytes = protocol::encodeRequest(QStringLiteral("x"), 1, QJsonObject{{"cwd", path}});
        const auto parsed = protocol::parseMessage(bytes.trimmed());
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->value("cwd").toString(), path);
    }

    void parseMessage_rejectsGarbageAndNonObjects_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::newRow("not json")   << QByteArray("this is not json");
        QTest::newRow("empty")      << QByteArray("");
        QTest::newRow("array")      << QByteArray("[1,2]");
        QTest::newRow("number")     << QByteArray("42");
        QTest::newRow("truncated")  << QByteArray("{\"ev\":\"pong\"");
    }

    void parseMessage_rejectsGarbageAndNonObjects()
    {
        QFETCH(QByteArray, line);
        QString error;
        QVERIFY(!protocol::parseMessage(line, &error).has_value());
        QVERIFY(!error.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstProtocolCodec)
#include "tst_protocolcodec.moc"
