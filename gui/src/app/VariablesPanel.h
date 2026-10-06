#ifndef REVSTUDIO_VARIABLESPANEL_H
#define REVSTUDIO_VARIABLESPANEL_H

#include <QJsonArray>
#include <QWidget>

class QCheckBox;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QPoint;
class QStandardItemModel;
class QTreeView;

namespace revstudio {

class ConsoleWidget;
class Session;
class VariablesFilterProxyModel;
class VariablesModel;

/**
 * G4: the variables dock (GUI_Implementation_Note.md, section 6.6) -- a filterable tree bound to VariablesModel,
 * a details pane fed by `inspect` on selection, a context menu, and a second "Functions" tab. Does not own the
 * Session or the ConsoleWidget it is given (same borrowing pattern as everywhere else); routes every action that
 * runs Rev code through the console (`submitText`) rather than calling Session::submit() directly, so `clear(x)`
 * from this panel's context menu shows up in the console and its history exactly as if typed -- matching what
 * section 6.7 specifies for the (not yet built) file explorer's "Set as working directory".
 */
class VariablesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit VariablesPanel(Session* session, ConsoleWidget* console, QWidget* parent = nullptr);

private:
    void onSelectionChanged();
    void onDoubleClicked(const QModelIndex& proxyIndex);
    void showContextMenu(const QPoint& position);
    void refreshFunctions(const QJsonArray& rows);
    QString nameAt(const QModelIndex& proxyIndex) const;

    Session*       session_;
    ConsoleWidget* console_;

    VariablesModel*             model_;
    VariablesFilterProxyModel*  proxy_;
    QTreeView*                  tree_;
    QLineEdit*                  filterEdit_;
    QCheckBox*                  showWorkspaceCheck_;
    QCheckBox*                  showHiddenCheck_;
    QPlainTextEdit*             details_;

    QStandardItemModel* functionsModel_;

    qint64 pendingInspectId_ = -1;
};

} // namespace revstudio

#endif
