#include "backend/BackendLocator.h"

#include <QtTest>

using namespace revstudio;

namespace {

/** A file that looks like an executable to QFileInfo on this platform. */
QString makeExecutable(const QString& directory, const QString& name)
{
    QDir().mkpath(directory);
    const QString path = directory + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return QString();
    }
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup);
    return QDir::cleanPath(path);
}

QStringList origins(const QList<BackendLocator::Candidate>& candidates)
{
    QStringList result;
    for (const auto& c : candidates)
    {
        result << c.origin;
    }
    return result;
}

} // namespace

class TstBackendLocator : public QObject
{
    Q_OBJECT

private slots:
    void anExplicitPathIsTheOnlyCandidate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString other = makeExecutable(dir.path() + "/bin", BackendLocator::executableName());
        QProcessEnvironment env;
        env.insert("REVBAYES_EXECUTABLE", other);

        // The explicit path does not exist. It must NOT silently fall back to the environment variable.
        const BackendLocator locator(dir.path() + "/does/not/exist/rb", QString(), dir.path(), env);
        const auto candidates = locator.candidates();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().origin, QStringLiteral("--rb"));
        QVERIFY(!locator.locate().has_value());
    }

    void candidatesFollowTheDocumentedOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcessEnvironment env;
        env.insert("REVBAYES_EXECUTABLE", dir.path() + "/env/rb");
        env.insert("PATH", dir.path() + "/p1" + QDir::listSeparator() + dir.path() + "/p2");

        const BackendLocator locator(QString(), dir.path() + "/settings/rb", dir.path() + "/app", env);
        QCOMPARE(origins(locator.candidates()),
                 (QStringList{"settings", "REVBAYES_EXECUTABLE", "next to the GUI", "../bin", "PATH", "PATH"}));
    }

    void locateReturnsTheFirstExistingExecutable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = BackendLocator::executableName();

        // Only the PATH entry and the app-dir entry exist; the app dir comes first in the search order.
        const QString onPath = makeExecutable(dir.path() + "/p1", name);
        const QString besideGui = makeExecutable(dir.path() + "/app", name);
        QVERIFY(!onPath.isEmpty() && !besideGui.isEmpty());

        QProcessEnvironment env;
        env.insert("PATH", dir.path() + "/p1");
        const BackendLocator locator(QString(), QString(), dir.path() + "/app", env);

        const auto found = locator.locate();
        QVERIFY(found.has_value());
        QCOMPARE(found->path, besideGui);
        QCOMPARE(found->origin, QStringLiteral("next to the GUI"));

        QFile::remove(besideGui);
        const auto fallback = locator.locate();
        QVERIFY(fallback.has_value());
        QCOMPARE(fallback->path, onPath);
        QCOMPARE(fallback->origin, QStringLiteral("PATH"));
    }

    void anExplicitExistingPathIsFound()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = makeExecutable(dir.path(), BackendLocator::executableName());
        const BackendLocator locator(path, QString(), QString(), QProcessEnvironment());
        const auto found = locator.locate();
        QVERIFY(found.has_value());
        QCOMPARE(found->path, path);
        QCOMPARE(found->origin, QStringLiteral("--rb"));
    }

    void aDirectoryIsNotAnExecutable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const BackendLocator locator(dir.path(), QString(), QString(), QProcessEnvironment());
        QVERIFY(!locator.locate().has_value());
    }

    void probeReadsTheMockBackend()
    {
        QString error;
        const auto info = BackendLocator::probe(QStringLiteral(MOCK_RB_PATH), &error);
        QVERIFY2(info.has_value(), qPrintable(error));
        QCOMPARE(info->protocol, 1);
        QVERIFY(info->version.startsWith(QStringLiteral("RevBayes")));
    }

    void probeReportsAMissingProgram()
    {
        QString error;
        QVERIFY(!BackendLocator::probe(QStringLiteral("/definitely/not/here/rb"), &error, 2000).has_value());
        QVERIFY2(error.contains(QStringLiteral("could not start")), qPrintable(error));
    }

#if !defined(Q_OS_WIN)
    void probeReportsABackendThatFailsOrPrintsNonsense()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString failing = dir.path() + "/failing";
        {
            QFile f(failing);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nexit 3\n");
        }
        QFile::setPermissions(failing, QFile::permissions(failing) | QFileDevice::ExeOwner);

        QString error;
        QVERIFY(!BackendLocator::probe(failing, &error, 5000).has_value());
        QVERIFY2(error.contains(QStringLiteral("too old")), qPrintable(error));

        const QString chatty = dir.path() + "/chatty";
        {
            QFile f(chatty);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\necho hello world\n");
        }
        QFile::setPermissions(chatty, QFile::permissions(chatty) | QFileDevice::ExeOwner);
        QVERIFY(!BackendLocator::probe(chatty, &error, 5000).has_value());
        QVERIFY2(error.contains(QStringLiteral("unexpected")), qPrintable(error));
    }
#endif
};

QTEST_APPLESS_MAIN(TstBackendLocator)
#include "tst_backendlocator.moc"
