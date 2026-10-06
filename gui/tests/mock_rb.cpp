// mock-rb: a stand-in for `rb --server`, used by the GUI's tests and by CI (which has no rb to run).
//
//   mock-rb --server-info
//   mock-rb --server [options]
//
// Options (also read from the environment variable MOCK_RB_OPTIONS, whitespace separated):
//   --stub                 behave like the phase 0 backend: `submit` answers error not_implemented, features []
//   --hello-delay-ms N     wait N ms before answering `hello`
//   --hello-protocol N     claim protocol N in the `hello` reply
//   --crash-after N        exit abruptly (code 3, no goodbye) after answering the N-th request
//   --malformed-after N    after answering the N-th request, also print a line that is not JSON
//   --malformed-count M    ... print M such lines (default 1)
//   --transcript FILE      replay FILE instead of the built-in behaviour (one JSON object per line):
//                            {"expect": {...subset of the request...}, "reply": [events...], "delay_ms": N, "exit": N}
//                          `re` is filled in from the request id. A mismatch answers error `mock_mismatch` and exits 3.
//
// Only QtCore is used (for JSON), so the mock is a plain console program on every platform.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QThread>
#include <QCoreApplication>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Options
{
    bool    serverMode = false;
    bool    serverInfo = false;
    bool    stub = false;
    int     helloDelayMs = 0;
    int     helloProtocol = 1;
    int     crashAfter = -1;
    int     malformedAfter = -1;
    int     malformedCount = 1;
    QString transcript;
};

struct Step
{
    QJsonObject expect;
    QJsonArray  reply;
    int         delayMs = 0;
    int         exitCode = -1;
};

const QString serverName = QStringLiteral("RevBayes (mock)");

void writeLine(const QByteArray& bytes)
{
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void emitEvent(const QJsonObject& event)
{
    writeLine(QJsonDocument(event).toJson(QJsonDocument::Compact));
}

Options parse(int argc, char* argv[])
{
    QStringList args;
    for (int i = 1; i < argc; ++i)
    {
        args << QString::fromLocal8Bit(argv[i]);
    }
    args += QString::fromLocal8Bit(qgetenv("MOCK_RB_OPTIONS")).split(QLatin1Char(' '), Qt::SkipEmptyParts);

    Options options;
    for (int i = 0; i < args.size(); ++i)
    {
        const QString& a = args.at(i);
        const auto next = [&]() { return i + 1 < args.size() ? args.at(++i) : QString(); };

        if (a == QLatin1String("--server"))                 options.serverMode = true;
        else if (a == QLatin1String("--server-info"))       options.serverInfo = true;
        else if (a == QLatin1String("--stub"))              options.stub = true;
        else if (a == QLatin1String("--hello-delay-ms"))    options.helloDelayMs = next().toInt();
        else if (a == QLatin1String("--hello-protocol"))    options.helloProtocol = next().toInt();
        else if (a == QLatin1String("--crash-after"))       options.crashAfter = next().toInt();
        else if (a == QLatin1String("--malformed-after"))   options.malformedAfter = next().toInt();
        else if (a == QLatin1String("--malformed-count"))   options.malformedCount = next().toInt();
        else if (a == QLatin1String("--transcript"))        options.transcript = next();
    }
    return options;
}

QList<Step> loadTranscript(const QString& path)
{
    QList<Step> steps;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        std::fprintf(stderr, "mock-rb: cannot read transcript %s\n", qPrintable(path));
        std::exit(2);
    }
    while (!file.atEnd())
    {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
        {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(line).object();
        Step step;
        step.expect   = object.value(QStringLiteral("expect")).toObject();
        step.reply    = object.value(QStringLiteral("reply")).toArray();
        step.delayMs  = object.value(QStringLiteral("delay_ms")).toInt(0);
        step.exitCode = object.value(QStringLiteral("exit")).toInt(-1);
        steps.append(step);
    }
    return steps;
}

bool matches(const QJsonObject& expect, const QJsonObject& request)
{
    for (auto it = expect.begin(); it != expect.end(); ++it)
    {
        if (request.value(it.key()) != it.value())
        {
            return false;
        }
    }
    return true;
}

QJsonObject withReply(QJsonObject event, const QJsonValue& id)
{
    if (!id.isUndefined() && !id.isNull() && !event.contains(QStringLiteral("re")))
    {
        event.insert(QStringLiteral("re"), id);
    }
    return event;
}

QJsonObject errorEvent(const QJsonValue& id, const QString& code, const QString& message)
{
    return withReply({{QStringLiteral("ev"), QStringLiteral("error")},
                      {QStringLiteral("code"), code},
                      {QStringLiteral("message"), message}}, id);
}

QJsonObject doneEvent(const QJsonValue& id)
{
    return withReply({{QStringLiteral("ev"), QStringLiteral("done")},
                      {QStringLiteral("status"), QStringLiteral("ok")},
                      {QStringLiteral("rc"), 0},
                      {QStringLiteral("cwd"), QDir::currentPath()},
                      {QStringLiteral("elapsed_ms"), 1}}, id);
}

} // namespace

int main(int argc, char* argv[])
{
#if defined(Q_OS_WIN)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const Options options = parse(argc, argv);

    if (options.serverInfo)
    {
        emitEvent({{QStringLiteral("protocol"), 1},
                   {QStringLiteral("version"), serverName},
                   {QStringLiteral("features"), options.stub ? QJsonArray() : QJsonArray{QStringLiteral("mock")}}});
        return 0;
    }
    if (!options.serverMode)
    {
        std::fprintf(stderr, "mock-rb: expected --server or --server-info\n");
        return 2;
    }

    QList<Step> transcript;
    if (!options.transcript.isEmpty())
    {
        transcript = loadTranscript(options.transcript);
    }
    int transcriptIndex = 0;
    int variablesRevision = 0;
    int handled = 0;

    std::string text;
    while (std::getline(std::cin, text))
    {
        if (!text.empty() && text.back() == '\r')
        {
            text.pop_back();
        }
        if (text.empty())
        {
            continue;
        }

        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
        const QJsonValue  id      = request.contains(QStringLiteral("id")) ? request.value(QStringLiteral("id")) : QJsonValue::Null;
        const QString     cmd     = request.value(QStringLiteral("cmd")).toString();

        bool shutdownNow = false;

        if (!options.transcript.isEmpty())
        {
            if (transcriptIndex >= transcript.size() || !matches(transcript.at(transcriptIndex).expect, request))
            {
                emitEvent(errorEvent(id, QStringLiteral("mock_mismatch"),
                                     QStringLiteral("request %1 did not match transcript step %2").arg(QString::fromStdString(text)).arg(transcriptIndex)));
                return 3;
            }
            const Step step = transcript.at(transcriptIndex++);
            if (step.delayMs > 0)
            {
                QThread::msleep(static_cast<unsigned long>(step.delayMs));
            }
            for (const QJsonValue& event : step.reply)
            {
                emitEvent(withReply(event.toObject(), id));
            }
            if (step.exitCode >= 0)
            {
                return step.exitCode;
            }
        }
        else if (cmd == QLatin1String("hello"))
        {
            if (options.helloDelayMs > 0)
            {
                QThread::msleep(static_cast<unsigned long>(options.helloDelayMs));
            }
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("hello")},
                                 {QStringLiteral("protocol"), options.helloProtocol},
                                 {QStringLiteral("server"), serverName},
                                 {QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid())},
                                 {QStringLiteral("cwd"), QDir::currentPath()},
                                 {QStringLiteral("features"), options.stub ? QJsonArray() : QJsonArray{QStringLiteral("mock")}}}, id));
        }
        else if (cmd == QLatin1String("ping"))
        {
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("pong")}}, id));
        }
        else if (cmd == QLatin1String("interrupt"))
        {
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("ack")},
                                 {QStringLiteral("of"), QStringLiteral("interrupt")},
                                 {QStringLiteral("was_busy"), false}}, id));
        }
        else if (cmd == QLatin1String("complete") && !options.stub)
        {
            // A tiny, deterministic stand-in matching doc/server-protocol.md's own sample session: completing
            // the identifier run right before `cursor` when it is exactly "dnExp" (GUI tests drive this with
            // predictable buffers; the real backend's actual completion logic is not something to re-implement
            // here).
            const QString buffer = request.value(QStringLiteral("buffer")).toString();
            const QByteArray bytes = buffer.toUtf8();
            const int cursor = request.contains(QStringLiteral("cursor")) ? request.value(QStringLiteral("cursor")).toInt()
                                                                            : bytes.size();
            int start = qBound(0, cursor, bytes.size());
            while (start > 0 && (std::isalnum(static_cast<unsigned char>(bytes.at(start - 1))) || bytes.at(start - 1) == '_'))
            {
                --start;
            }
            const QString prefix = QString::fromUtf8(bytes.mid(start, cursor - start));
            const QString head = QString::fromUtf8(bytes.left(start));
            QJsonArray items;
            if (prefix == QLatin1String("dnExp"))
            {
                items.append(QJsonObject{{QStringLiteral("text"), head + QStringLiteral("dnExponential")},
                                          {QStringLiteral("kind"), QStringLiteral("function")}});
                items.append(QJsonObject{{QStringLiteral("text"), head + QStringLiteral("dnExponentialError")},
                                          {QStringLiteral("kind"), QStringLiteral("function")}});
            }
            else if (prefix == QLatin1String("uniq"))
            {
                // A second, single-match fixture: GUI tests use this one to check the "nothing to choose between,
                // just insert it" path without needing to drive the completer's own popup.
                items.append(QJsonObject{{QStringLiteral("text"), head + QStringLiteral("uniqueFunctionName")},
                                          {QStringLiteral("kind"), QStringLiteral("function")}});
            }
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("completions")},
                                 {QStringLiteral("replace_from"), start},
                                 {QStringLiteral("items"), items}}, id));
        }
        else if (cmd == QLatin1String("help") && !options.stub)
        {
            // A tiny, deterministic stand-in: the empty topic is the "index", "dnExponential" is a known topic
            // with real text, and anything else is reported not found -- enough for GUI tests to exercise all
            // three without re-implementing the real help system here.
            const QString topic = request.value(QStringLiteral("topic")).toString();
            bool found = true;
            QString text;
            if (topic.isEmpty())
            {
                text = QStringLiteral("Index of help topics.");
            }
            else if (topic == QLatin1String("dnExponential"))
            {
                text = QStringLiteral("dnExponential(lambda): the exponential distribution.");
            }
            else
            {
                found = false;
            }
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("help")},
                                 {QStringLiteral("topic"), topic},
                                 {QStringLiteral("found"), found},
                                 {QStringLiteral("text"), text}}, id));
        }
        else if (cmd == QLatin1String("shutdown"))
        {
            emitEvent(withReply({{QStringLiteral("ev"), QStringLiteral("bye")},
                                 {QStringLiteral("reason"), QStringLiteral("shutdown")}}, id));
            shutdownNow = true;
        }
        else if (cmd == QLatin1String("submit") && !options.stub)
        {
            const QString code = request.value(QStringLiteral("text")).toString();
            emitEvent({{QStringLiteral("ev"), QStringLiteral("output")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("stream"), QStringLiteral("rbout")},
                       {QStringLiteral("text"), code == QLatin1String("1+1") ? QStringLiteral("2\n") : code + QLatin1Char('\n')}});
            emitEvent(doneEvent(id));
            emitEvent({{QStringLiteral("ev"), QStringLiteral("variables")},
                       {QStringLiteral("rev"), ++variablesRevision},
                       {QStringLiteral("cwd"), QDir::currentPath()},
                       {QStringLiteral("rows"), QJsonArray()}});
        }
        else
        {
            emitEvent(errorEvent(id, QStringLiteral("not_implemented"),
                                 QStringLiteral("'%1' is not implemented by the mock").arg(cmd)));
        }

        ++handled;
        if (options.malformedAfter > 0 && handled == options.malformedAfter)
        {
            for (int i = 0; i < options.malformedCount; ++i)
            {
                writeLine("this is not json");
            }
        }
        if (options.crashAfter > 0 && handled >= options.crashAfter)
        {
            std::_Exit(3);                      // no flush, no goodbye: a crash
        }
        if (shutdownNow)
        {
            return 0;
        }
    }
    return 0;                                   // end of input
}
