#include "backend/BackendLocator.h"

#include "backend/ProtocolCodec.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>

namespace revstudio {

BackendLocator::BackendLocator(const QString& explicitPath,
                               const QString& settingsPath,
                               const QString& applicationDir,
                               const QProcessEnvironment& environment)
    : explicitPath_(explicitPath),
      settingsPath_(settingsPath),
      applicationDir_(applicationDir),
      environment_(environment)
{
}

QString BackendLocator::executableName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("rb.exe");
#else
    return QStringLiteral("rb");
#endif
}

QList<BackendLocator::Candidate> BackendLocator::candidates() const
{
    QList<Candidate> result;

    if (!explicitPath_.isEmpty())
    {
        result.append({QDir::cleanPath(explicitPath_), QStringLiteral("--rb")});
        return result;                      // an explicit choice is never second-guessed
    }

    if (!settingsPath_.isEmpty())
    {
        result.append({QDir::cleanPath(settingsPath_), QStringLiteral("settings")});
    }

    const QString fromEnvironment = environment_.value(QStringLiteral("REVBAYES_EXECUTABLE"));
    if (!fromEnvironment.isEmpty())
    {
        result.append({QDir::cleanPath(fromEnvironment), QStringLiteral("REVBAYES_EXECUTABLE")});
    }

    if (!applicationDir_.isEmpty())
    {
        result.append({QDir::cleanPath(applicationDir_ + QLatin1Char('/') + executableName()),
                       QStringLiteral("next to the GUI")});
        result.append({QDir::cleanPath(applicationDir_ + QStringLiteral("/../bin/") + executableName()),
                       QStringLiteral("../bin")});
    }

    const QStringList directories = environment_.value(QStringLiteral("PATH"))
                                        .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString& directory : directories)
    {
        result.append({QDir::cleanPath(directory + QLatin1Char('/') + executableName()), QStringLiteral("PATH")});
    }

    return result;
}

std::optional<BackendLocator::Candidate> BackendLocator::locate() const
{
    const QList<Candidate> all = candidates();
    for (const Candidate& candidate : all)
    {
        const QFileInfo info(candidate.path);
        if (info.isFile() && info.isExecutable())
        {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<BackendInfo> BackendLocator::probe(const QString& path, QString* error, int timeoutMs)
{
    const auto fail = [error](const QString& message) -> std::optional<BackendInfo>
    {
        if (error)
        {
            *error = message;
        }
        return std::nullopt;
    };

    QProcess process;
    process.start(path, {QStringLiteral("--server-info")});
    if (!process.waitForStarted(timeoutMs))
    {
        return fail(QStringLiteral("could not start '%1': %2").arg(path, process.errorString()));
    }
    if (!process.waitForFinished(timeoutMs))
    {
        process.kill();
        process.waitForFinished(1000);
        return fail(QStringLiteral("'%1 --server-info' did not finish within %2 ms").arg(path).arg(timeoutMs));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        return fail(QStringLiteral("'%1 --server-info' failed (exit code %2); this rb may be too old to have --server")
                        .arg(path).arg(process.exitCode()));
    }

    QString parseError;
    const auto object = protocol::parseMessage(process.readAllStandardOutput().trimmed(), &parseError);
    if (!object || !object->value(QStringLiteral("protocol")).isDouble()
        || !object->value(QStringLiteral("version")).isString())
    {
        return fail(QStringLiteral("'%1 --server-info' printed something unexpected (%2)")
                        .arg(path, object ? QStringLiteral("missing protocol or version") : parseError));
    }

    BackendInfo info;
    info.protocol = object->value(QStringLiteral("protocol")).toInt();
    info.version  = object->value(QStringLiteral("version")).toString();
    const QJsonArray features = object->value(QStringLiteral("features")).toArray();
    for (const QJsonValue& feature : features)
    {
        info.features.append(feature.toString());
    }
    return info;
}

} // namespace revstudio
