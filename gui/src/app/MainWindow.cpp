#include "app/MainWindow.h"

#include "backend/Session.h"
#include "revstudio_version.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>

namespace revstudio {

MainWindow::MainWindow(Session* session, QWidget* parent)
    : QMainWindow(parent), session_(session), log_(new QPlainTextEdit(this)), status_(new QLabel(this))
{
    setWindowTitle(tr("RevStudio %1").arg(QStringLiteral(REVSTUDIO_VERSION)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/revstudio.svg")));
    resize(900, 600);

    log_->setReadOnly(true);
    log_->setPlaceholderText(tr("RevStudio %1 (phase 0 skeleton)").arg(QStringLiteral(REVSTUDIO_VERSION)));
    setCentralWidget(log_);

    statusBar()->addWidget(status_);

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* quit = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About RevStudio"), this, &MainWindow::showAbout);
    helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);      // the usual LGPL courtesy (note, 5.4)

    connect(session_, &Session::stateChanged, this, &MainWindow::updateStatus);
    connect(session_, &Session::ready, this, [this](const QString& version)
    {
        log(tr("Backend ready: %1 (working directory %2)").arg(version, session_->cwd()));
    });
    connect(session_, &Session::output, this, [this](const QString& stream, const QString& text, qint64)
    {
        log(QStringLiteral("[%1] %2").arg(stream, text.trimmed()));
    });
    connect(session_, &Session::backendStderr, this, [this](const QString& text)
    {
        log(QStringLiteral("[stderr] %1").arg(text));
    });
    connect(session_, &Session::protocolWarning, this, [this](const QString& text)
    {
        log(QStringLiteral("[protocol] %1").arg(text));
    });
    connect(session_, &Session::failed, this, [this](const QString& reason)
    {
        log(tr("Backend failed: %1").arg(reason));
    });

    updateStatus();
}

void MainWindow::log(const QString& text)
{
    log_->appendPlainText(text);
}

QString MainWindow::logText() const
{
    return log_->toPlainText();
}

void MainWindow::updateStatus()
{
    QString text;
    switch (session_->state())
    {
    case Session::State::NotStarted: text = tr("Backend: not started"); break;
    case Session::State::Starting:   text = tr("Backend: starting..."); break;
    case Session::State::Ready:      text = tr("Idle  |  %1").arg(session_->serverVersion()); break;
    case Session::State::Busy:       text = tr("Running..."); break;
    case Session::State::Closing:    text = tr("Backend: shutting down..."); break;
    case Session::State::Closed:     text = tr("Backend: closed"); break;
    case Session::State::Failed:     text = tr("Backend: FAILED"); break;
    }
    status_->setText(text);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About RevStudio"),
                       tr("<b>RevStudio %1</b><br>A front end for RevBayes.<br><br>"
                          "Built with Qt %2. Licensed under the GNU General Public License, version 3.")
                           .arg(QStringLiteral(REVSTUDIO_VERSION), QString::fromLatin1(qVersion())));
}

} // namespace revstudio
