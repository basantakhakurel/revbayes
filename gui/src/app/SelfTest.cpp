#include "app/SelfTest.h"

#include "backend/BackendLocator.h"
#include "backend/ProtocolCodec.h"
#include "backend/Session.h"
#include "revstudio_version.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QTextStream>

namespace revstudio {

SelfTest::SelfTest(Session* session, const Options& options, QObject* parent)
    : QObject(parent), session_(session), options_(options)
{
    overallTimer_.setSingleShot(true);
    variablesTimer_.setSingleShot(true);

    connect(&overallTimer_, &QTimer::timeout, this, [this]
    {
        failStep(tr("timed out after %1 ms").arg(options_.timeoutMs));
    });
    connect(&variablesTimer_, &QTimer::timeout, this, [this]
    {
        if (step_ == Step::Variables)
        {
            failStep(tr("no `variables` event followed `done`"));
        }
    });

    connect(session_, &Session::ready, this, [this](const QString& version)
    {
        if (step_ != Step::Handshake)
        {
            return;
        }
        report(QStringLiteral("handshake"), tr("ok (%1)").arg(version));
        step_ = Step::Ping;
        session_->ping();
    });

    connect(session_, &Session::pongReceived, this, [this](qint64)
    {
        if (step_ != Step::Ping)
        {
            return;
        }
        report(QStringLiteral("ping"), QStringLiteral("ok"));
        step_ = Step::Submit;
        collectedOutput_.clear();
        session_->submit(QStringLiteral("1+1"));
    });

    connect(session_, &Session::output, this, [this](const QString&, const QString& text, qint64)
    {
        if (step_ == Step::Submit)
        {
            collectedOutput_ += text;
        }
    });

    connect(session_, &Session::backendError, this, [this](qint64, const QString& code, const QString& message)
    {
        if (step_ == Step::Submit && code == QLatin1String("not_implemented"))
        {
            report(QStringLiteral("submit"), tr("skipped (the backend does not implement submit yet)"));
            startShutdown();
            return;
        }
        failStep(tr("unexpected error event %1: %2").arg(code, message));
    });

    connect(session_, &Session::submitFinished, this, [this](qint64, const QString& status, const QJsonObject&)
    {
        if (step_ != Step::Submit)
        {
            return;
        }
        if (status != QLatin1String("ok") || !collectedOutput_.contains(QLatin1Char('2')))
        {
            failStep(tr("`1+1` gave status '%1' and output '%2'").arg(status, collectedOutput_.trimmed()));
            return;
        }
        report(QStringLiteral("submit"), tr("ok (output '%1')").arg(collectedOutput_.trimmed()));
        step_ = Step::Variables;
        variablesTimer_.start(3000);
    });

    connect(session_, &Session::variablesChanged, this, [this](const QJsonArray& rows, const QString&)
    {
        if (step_ != Step::Variables)
        {
            return;
        }
        variablesTimer_.stop();
        report(QStringLiteral("variables"), tr("ok (%1 rows)").arg(rows.size()));
        startShutdown();
    });

    connect(session_, &Session::closed, this, [this](int exitCode)
    {
        if (step_ == Step::Shutdown)
        {
            if (exitCode == 0)
            {
                report(QStringLiteral("shutdown"), QStringLiteral("ok (exit code 0)"));
                finish(true);
            }
            else
            {
                failStep(tr("backend exited with code %1").arg(exitCode));
            }
        }
        else if (!finished_)
        {
            failStep(tr("backend exited (code %1) during '%2'").arg(exitCode).arg(stepName(step_)));
        }
    });

    connect(session_, &Session::failed, this, [this](const QString& reason)
    {
        failStep(reason);
    });
}

void SelfTest::start()
{
    overallTimer_.start(options_.timeoutMs);

    lines_ << tr("RevStudio %1, Qt %2, platform '%3'")
                  .arg(QStringLiteral(REVSTUDIO_VERSION), QString::fromLatin1(qVersion()), QGuiApplication::platformName());

    // icon: the resource is embedded AND the SVG plugins can turn it into pixels. Sampling the centre pixel proves
    // something was actually drawn (an unloadable icon is a silent, blank one).
    step_ = Step::Icon;
    const QString iconPath = QStringLiteral(":/icons/revstudio.svg");
    if (!QFile::exists(iconPath))
    {
        failStep(tr("the resource %1 is not embedded in the executable").arg(iconPath));
        return;
    }
    const QImage iconImage = QIcon(iconPath).pixmap(32, 32).toImage();
    if (iconImage.isNull() || qAlpha(iconImage.pixel(16, 16)) == 0)
    {
        failStep(tr("the SVG icon rendered as blank (are the Qt SVG plugins deployed?)"));
        return;
    }
    report(QStringLiteral("icon"), QStringLiteral("ok"));

    // locate
    step_ = Step::Locate;
    const BackendLocator locator(options_.rbPath, QString(), QCoreApplication::applicationDirPath());
    const auto found = locator.locate();
    if (!found)
    {
        QStringList tried;
        for (const auto& candidate : locator.candidates())
        {
            tried << QStringLiteral("%1 (%2)").arg(candidate.path, candidate.origin);
        }
        failStep(tr("no backend found; tried: %1").arg(tried.isEmpty() ? tr("nothing") : tried.join(QStringLiteral(", "))));
        return;
    }
    report(QStringLiteral("locate"), tr("ok (%1, via %2)").arg(found->path, found->origin));

    // probe
    step_ = Step::Probe;
    QString error;
    const auto info = BackendLocator::probe(found->path, &error);
    if (!info)
    {
        failStep(error);
        return;
    }
    if (info->protocol != protocol::version)
    {
        failStep(tr("backend speaks protocol %1, this RevStudio needs %2").arg(info->protocol).arg(protocol::version));
        return;
    }
    report(QStringLiteral("probe"), tr("ok (%1, protocol %2)").arg(info->version).arg(info->protocol));

    // handshake (continues in the connections above)
    step_ = Step::Handshake;
    session_->start(found->path, options_.workingDirectory);
}

void SelfTest::startShutdown()
{
    step_ = Step::Shutdown;
    session_->shutdown();
}

void SelfTest::report(const QString& step, const QString& result)
{
    lines_ << QStringLiteral("selftest: %1: %2").arg(step, result);
}

void SelfTest::failStep(const QString& detail)
{
    if (finished_)
    {
        return;
    }
    lines_ << QStringLiteral("selftest: %1: FAILED: %2").arg(stepName(step_), detail);
    finish(false);
}

void SelfTest::finish(bool ok)
{
    if (finished_)
    {
        return;
    }
    finished_ = true;
    overallTimer_.stop();
    variablesTimer_.stop();

    if (!ok)
    {
        const QStringList tail = session_->stderrTail();
        if (!tail.isEmpty())
        {
            lines_ << QStringLiteral("selftest: last lines from the backend's stderr:");
            for (const QString& line : tail)
            {
                lines_ << QStringLiteral("    ") + line;
            }
        }
        session_->kill();
    }
    lines_ << (ok ? QStringLiteral("selftest: PASSED") : QStringLiteral("selftest: FAILED"));

    const QString text = lines_.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QTextStream(stdout) << text << Qt::flush;

    if (!options_.reportPath.isEmpty())
    {
        QFile file(options_.reportPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            file.write(text.toUtf8());
        }
    }

    emit finished(ok ? 0 : 1);
}

QString SelfTest::stepName(Step step)
{
    switch (step)
    {
    case Step::Icon:      return QStringLiteral("icon");
    case Step::Locate:    return QStringLiteral("locate");
    case Step::Probe:     return QStringLiteral("probe");
    case Step::Handshake: return QStringLiteral("handshake");
    case Step::Ping:      return QStringLiteral("ping");
    case Step::Submit:    return QStringLiteral("submit");
    case Step::Variables: return QStringLiteral("variables");
    case Step::Shutdown:  return QStringLiteral("shutdown");
    case Step::Done:      return QStringLiteral("done");
    }
    return QStringLiteral("?");
}

} // namespace revstudio
