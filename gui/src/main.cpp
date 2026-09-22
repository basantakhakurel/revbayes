#include "app/MainWindow.h"
#include "app/SelfTest.h"
#include "backend/BackendLocator.h"
#include "backend/Session.h"
#include "revstudio_version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QIcon>
#include <QTimer>

#include <cstdio>
#include <cstring>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {

/**
 * A Windows GUI-subsystem program has no console. If it was started from a console WITHOUT redirection, borrow the
 * parent's so --version and --selftest can print. If stdout is already redirected to a file or a pipe, leave it alone.
 */
void attachParentConsoleIfNeeded()
{
#if defined(Q_OS_WIN)
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            FILE* unused = nullptr;
            freopen_s(&unused, "CONOUT$", "w", stdout);
            freopen_s(&unused, "CONOUT$", "w", stderr);
        }
    }
#endif
}

void printUsage()
{
    std::printf("Usage: RevStudio [options] [files...]\n"
                "\n"
                "  --rb PATH       path of the rb executable (default: search, see gui/README.md)\n"
                "  --cwd DIR       working directory for the backend (default: the current directory)\n"
                "  --selftest      start the backend, run a short conversation, report, and exit (0 = success)\n"
                "  --report FILE   with --selftest: also write the report to FILE\n"
                "  -v, --version   print the version and exit\n"
                "  -h, --help      print this text and exit\n"
                "\n"
                "Files are accepted but not opened yet (phase 2).\n");
}

} // namespace

int main(int argc, char* argv[])
{
    // These must work without a display, so they are handled before any Qt GUI object exists.
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0)
        {
            attachParentConsoleIfNeeded();
            std::printf("RevStudio %s (Qt %s)\n", REVSTUDIO_VERSION, qVersion());
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            attachParentConsoleIfNeeded();
            printUsage();
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("RevBayes"));
    QApplication::setApplicationName(QStringLiteral("RevStudio"));
    QApplication::setApplicationVersion(QStringLiteral(REVSTUDIO_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/revstudio.svg")));
    QGuiApplication::setDesktopFileName(QStringLiteral("revstudio"));      // lets Wayland shells match the .desktop file

    QCommandLineParser parser;
    const QCommandLineOption rbOption(QStringLiteral("rb"), QStringLiteral("Path of the rb executable."), QStringLiteral("path"));
    const QCommandLineOption cwdOption(QStringLiteral("cwd"), QStringLiteral("Working directory for the backend."), QStringLiteral("dir"));
    const QCommandLineOption selftestOption(QStringLiteral("selftest"), QStringLiteral("Run the self-test and exit."));
    const QCommandLineOption reportOption(QStringLiteral("report"), QStringLiteral("Write the self-test report to a file."), QStringLiteral("file"));
    parser.addOptions({rbOption, cwdOption, selftestOption, reportOption});
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Rev scripts to open (not implemented yet)."), QStringLiteral("[files...]"));

    if (!parser.parse(app.arguments()))
    {
        attachParentConsoleIfNeeded();
        std::fprintf(stderr, "RevStudio: %s\nTry --help.\n", qPrintable(parser.errorText()));
        return 2;
    }

    const QString workingDirectory = parser.isSet(cwdOption) ? parser.value(cwdOption) : QDir::currentPath();

    revstudio::Session session;
    revstudio::MainWindow window(&session);
    window.show();

    if (parser.isSet(selftestOption))
    {
        attachParentConsoleIfNeeded();

        revstudio::SelfTest::Options options;
        options.rbPath           = parser.value(rbOption);
        options.reportPath       = parser.value(reportOption);
        options.workingDirectory = workingDirectory;

        revstudio::SelfTest selfTest(&session, options);
        QObject::connect(&selfTest, &revstudio::SelfTest::finished, &app, [](int code) { QCoreApplication::exit(code); });
        QTimer::singleShot(0, &selfTest, &revstudio::SelfTest::start);
        return app.exec();
    }

    // Normal start: find a backend and start the session.
    const revstudio::BackendLocator locator(parser.value(rbOption), QString(), QCoreApplication::applicationDirPath());
    const auto backend = locator.locate();
    if (backend)
    {
        window.log(QObject::tr("Starting backend %1 (found via %2)").arg(backend->path, backend->origin));
        session.start(backend->path, workingDirectory);
    }
    else
    {
        window.log(QObject::tr("No rb executable found. Use --rb PATH or set REVBAYES_EXECUTABLE. Searched:"));
        for (const auto& candidate : locator.candidates())
        {
            window.log(QStringLiteral("    %1 (%2)").arg(candidate.path, candidate.origin));
        }
    }

    // Ask the backend to shut down cleanly when the application quits; the process is killed if it does not comply.
    QObject::connect(&app, &QApplication::aboutToQuit, &session, [&session]
    {
        if (session.state() == revstudio::Session::State::Ready || session.state() == revstudio::Session::State::Busy)
        {
            QEventLoop loop;
            QObject::connect(&session, &revstudio::Session::closed, &loop, &QEventLoop::quit);
            QTimer::singleShot(4000, &loop, &QEventLoop::quit);
            session.shutdown();
            loop.exec();
        }
    });

    return app.exec();
}
