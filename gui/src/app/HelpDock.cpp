#include "app/HelpDock.h"

#include "backend/Session.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace revstudio {

HelpDock::HelpDock(Session* session, QWidget* parent)
    : QWidget(parent), session_(session), topicEdit_(new QLineEdit(this)), backButton_(new QToolButton(this)),
      forwardButton_(new QToolButton(this)), browser_(new QTextBrowser(this))
{
    topicEdit_->setObjectName(QStringLiteral("helpTopic"));
    topicEdit_->setPlaceholderText(tr("Topic (empty for the index)"));
    topicEdit_->setClearButtonEnabled(true);

    backButton_->setObjectName(QStringLiteral("helpBack"));
    backButton_->setText(QStringLiteral("◀"));
    backButton_->setToolTip(tr("Back"));
    backButton_->setAutoRaise(true);
    backButton_->setEnabled(false);

    forwardButton_->setObjectName(QStringLiteral("helpForward"));
    forwardButton_->setText(QStringLiteral("▶"));
    forwardButton_->setToolTip(tr("Forward"));
    forwardButton_->setAutoRaise(true);
    forwardButton_->setEnabled(false);

    browser_->setObjectName(QStringLiteral("helpBrowser"));
    browser_->setOpenLinks(false);   // plain text, no real navigable anchors; setSource()'s own fetch is unused
    browser_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    browser_->setPlaceholderText(tr("Press F1 on a name in the editor, or type a topic above."));

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(backButton_);
    topRow->addWidget(forwardButton_);
    topRow->addWidget(topicEdit_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(topRow);
    layout->addWidget(browser_);

    connect(topicEdit_, &QLineEdit::returnPressed, this, [this] { showTopic(topicEdit_->text().trimmed()); });
    connect(backButton_, &QToolButton::clicked, this, &HelpDock::goBack);
    connect(forwardButton_, &QToolButton::clicked, this, &HelpDock::goForward);
    connect(session_, &Session::helpReceived, this, &HelpDock::onHelpReceived);
}

void HelpDock::showTopic(const QString& topic)
{
    if (historyIndex_ >= 0 && historyIndex_ < history_.size() - 1)
    {
        history_.erase(history_.begin() + historyIndex_ + 1, history_.end());
    }
    if (historyIndex_ < 0 || history_.at(historyIndex_) != topic)
    {
        history_.append(topic);
        historyIndex_ = history_.size() - 1;
    }
    requestAndShow(topic);
    updateNavButtons();
}

void HelpDock::goBack()
{
    if (historyIndex_ > 0)
    {
        --historyIndex_;
        requestAndShow(history_.at(historyIndex_));
        updateNavButtons();
    }
}

void HelpDock::goForward()
{
    if (historyIndex_ + 1 < history_.size())
    {
        ++historyIndex_;
        requestAndShow(history_.at(historyIndex_));
        updateNavButtons();
    }
}

void HelpDock::requestAndShow(const QString& topic)
{
    topicEdit_->setText(topic);
    pendingRequestId_ = session_->requestHelp(topic);
    if (pendingRequestId_ < 0)
    {
        browser_->setPlainText(tr("The backend is not ready."));
    }
}

void HelpDock::onHelpReceived(qint64 requestId, const QString& topic, bool found, const QString& text)
{
    if (requestId != pendingRequestId_)
    {
        return;   // the dock navigated again before this reply arrived; it is no longer what should be shown
    }
    browser_->setPlainText(found ? text : tr("No help found for \"%1\".").arg(topic));
}

void HelpDock::updateNavButtons()
{
    backButton_->setEnabled(historyIndex_ > 0);
    forwardButton_->setEnabled(historyIndex_ + 1 < history_.size());
}

} // namespace revstudio
