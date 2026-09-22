#ifndef REVSTUDIO_MAINWINDOW_H
#define REVSTUDIO_MAINWINDOW_H

#include <QMainWindow>

class QLabel;
class QPlainTextEdit;

namespace revstudio {

class Session;

/**
 * Phase 0 main window: a status bar, a log view and the menus every later phase needs (Quit, About, About Qt).
 * It exists to exercise the whole path end to end (platform plugin, style, SVG icon, backend process) in the
 * packaging spike. The docks, editor and panels of GUI_Implementation_Note.md, section 6, arrive in phase 2.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Session* session, QWidget* parent = nullptr);

    void    log(const QString& text);
    QString logText() const;

private:
    void updateStatus();
    void showAbout();

    Session*        session_;
    QPlainTextEdit* log_;
    QLabel*         status_;
};

} // namespace revstudio

#endif
