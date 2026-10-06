#ifndef REVSTUDIO_HELPDOCK_H
#define REVSTUDIO_HELPDOCK_H

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QTextBrowser;
class QToolButton;

namespace revstudio {

class Session;

/**
 * Q8 (GUI_Implementation_Note.md, section 6.10): the backend's `help` reply for a topic, shown plain and
 * monospaced in a `QTextBrowser`, with a topic box and back/forward history. F1 (the word under the caret) is
 * MainWindow's own action, scoped to the editor -- see its own comment on why the console input does not get
 * one too.
 *
 * History is tracked manually (a `QStringList` plus an index, ordinary browser-history semantics: navigating
 * to a new topic truncates anything past the current position) rather than through QTextBrowser's own
 * setSource()/loadResource(), which assumes synchronous resource loading -- ours is a round trip over the
 * protocol pipe, not a fit for that API, so `browser_` is used purely as a plain-text display here.
 */
class HelpDock : public QWidget
{
    Q_OBJECT

public:
    explicit HelpDock(Session* session, QWidget* parent = nullptr);

    /** Looks up `topic` ("" for the general index) and pushes it onto the back/forward history, truncating
     *  anything that was ahead of the current position (the usual browser-navigation behavior). A repeat of
     *  the current topic (the user just pressing Enter again) does not push a duplicate entry. */
    void showTopic(const QString& topic);

private:
    void goBack();
    void goForward();
    void requestAndShow(const QString& topic);
    void onHelpReceived(qint64 requestId, const QString& topic, bool found, const QString& text);
    void updateNavButtons();

    Session* session_;

    QLineEdit*    topicEdit_;
    QToolButton*  backButton_;
    QToolButton*  forwardButton_;
    QTextBrowser* browser_;

    QStringList history_;
    int         historyIndex_ = -1;
    qint64      pendingRequestId_ = -1;
};

} // namespace revstudio

#endif
