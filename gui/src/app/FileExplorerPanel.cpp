#include "app/FileExplorerPanel.h"

#include "app/ConsoleWidget.h"
#include "app/EditorTabs.h"
#include "app/RevString.h"
#include "app/Settings.h"
#include "backend/Session.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace revstudio {

/** Text filtering (recursive, matching QSortFilterProxyModel semantics) plus "show hidden files", off by
 *  default -- section 6.7: "hides dotfiles by default". */
class FileExplorerFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit FileExplorerFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent)
    {
        setRecursiveFilteringEnabled(true);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
    }

    void setShowHidden(bool show)
    {
        showHidden_ = show;
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        auto* fsModel = static_cast<QFileSystemModel*>(sourceModel());
        if (!showHidden_ && fsModel->fileName(fsModel->index(sourceRow, 0, sourceParent)).startsWith(QLatin1Char('.')))
        {
            return false;
        }
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }

private:
    bool showHidden_ = false;
};

FileExplorerPanel::FileExplorerPanel(Session* session, ConsoleWidget* console, EditorTabs* editor,
                                      Settings* settings, QWidget* parent)
    : QWidget(parent), session_(session), console_(console), editor_(editor), settings_(settings),
      model_(new QFileSystemModel(this)), proxy_(new FileExplorerFilterProxyModel(this)), tree_(new QTreeView(this)),
      filterEdit_(new QLineEdit(this)), showHiddenCheck_(new QCheckBox(tr("Hidden files"), this))
{
    // Hidden entries must still come from the source model -- the proxy above is what actually hides them by
    // default, and it needs them present to be able to show them again once the toggle is checked.
    model_->setObjectName(QStringLiteral("explorerModel"));   // for tests: DontWatchForChanges has no visible effect
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs | QDir::Hidden);
    model_->setOption(QFileSystemModel::DontWatchForChanges, !settings_->explorerWatchForChanges());
    proxy_->setSourceModel(model_);

    tree_->setObjectName(QStringLiteral("explorerTree"));
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(false);
    for (int column = 1; column < model_->columnCount(); ++column)
    {
        tree_->hideColumn(column);   // GUI_Implementation_Note.md 6.7: "only the name column visible by default"
    }
    tree_->setDragEnabled(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);

    filterEdit_->setObjectName(QStringLiteral("explorerFilter"));
    filterEdit_->setPlaceholderText(tr("Filter"));
    filterEdit_->setClearButtonEnabled(true);

    showHiddenCheck_->setObjectName(QStringLiteral("explorerShowHidden"));
    // Its own last state is the persisted "default" (G7/Preferences) -- set directly on the proxy rather than
    // relying on setChecked()'s toggled() signal, which would stay silent here if the remembered value happens
    // to already match the checkbox's own compile-time-default (unchecked).
    const bool showHiddenByDefault = settings_->explorerShowHiddenByDefault();
    showHiddenCheck_->setChecked(showHiddenByDefault);
    proxy_->setShowHidden(showHiddenByDefault);

    auto* toolbarRow = new QHBoxLayout;
    toolbarRow->addWidget(filterEdit_);
    toolbarRow->addWidget(showHiddenCheck_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(toolbarRow);
    layout->addWidget(tree_);

    connect(filterEdit_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(showHiddenCheck_, &QCheckBox::toggled, proxy_, &FileExplorerFilterProxyModel::setShowHidden);
    connect(showHiddenCheck_, &QCheckBox::toggled, this, [this](bool show)
    {
        settings_->setExplorerShowHiddenByDefault(show);
    });
    connect(tree_, &QTreeView::doubleClicked, this, &FileExplorerPanel::onDoubleClicked);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &FileExplorerPanel::showContextMenu);

    connect(session_, &Session::ready, this, [this](const QString&) { setRootPath(session_->cwd()); });
    connect(session_, &Session::submitFinished, this, [this](qint64, const QString&, const QJsonObject& done)
    {
        if (done.contains(QStringLiteral("cwd")))
        {
            setRootPath(done.value(QStringLiteral("cwd")).toString());
        }
    });
    connect(session_, &Session::variablesChanged, this, [this](const QJsonArray&, const QString& cwd)
    {
        setRootPath(cwd);
    });
}

void FileExplorerPanel::applyPreferences()
{
    model_->setOption(QFileSystemModel::DontWatchForChanges, !settings_->explorerWatchForChanges());
}

void FileExplorerPanel::setRootPath(const QString& path)
{
    if (path.isEmpty() || path == currentRoot_)
    {
        return;
    }
    currentRoot_ = path;
    tree_->setRootIndex(proxy_->mapFromSource(model_->setRootPath(path)));
}

void FileExplorerPanel::onDoubleClicked(const QModelIndex& proxyIndex)
{
    const QString path = pathAt(proxyIndex);
    if (path.isEmpty() || QFileInfo(path).isDir())
    {
        return;   // a directory: the view's own default double-click (expand/collapse) already handles this
    }
    editor_->openFile(path);
}

void FileExplorerPanel::showContextMenu(const QPoint& position)
{
    const QModelIndex index = tree_->indexAt(position);
    const QString path = pathAt(index);
    if (path.isEmpty())
    {
        return;
    }
    const QFileInfo info(path);
    const QString workingDirectory = info.isDir() ? path : info.absolutePath();

    QMenu menu(this);
    if (isRevFile(path))
    {
        menu.addAction(tr("Run (source)"), this, [this, path]
        {
            console_->submitText(QStringLiteral("source(%1)").arg(RevString::quote(path)));
        });
        menu.addSeparator();
    }
    menu.addAction(tr("Set as Working Directory"), this, [this, workingDirectory]
    {
        console_->submitText(QStringLiteral("setwd(%1)").arg(RevString::quote(workingDirectory)));
    });
    menu.addAction(tr("Reveal in File Manager"), this, [workingDirectory]
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(workingDirectory));
    });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path] { QGuiApplication::clipboard()->setText(path); });
    menu.addAction(tr("Copy as Rev String"), this, [path] { QGuiApplication::clipboard()->setText(RevString::quote(path)); });
    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, [this]
    {
        // The commonly suggested way to force QFileSystemModel to re-read a directory: its own
        // QFileSystemWatcher should already keep it current, but this covers the "just in case" button press.
        const QString root = currentRoot_;
        currentRoot_.clear();
        setRootPath(root);
    });
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

QString FileExplorerPanel::pathAt(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid())
    {
        return QString();
    }
    return model_->filePath(proxy_->mapToSource(proxyIndex));
}

bool FileExplorerPanel::isRevFile(const QString& path)
{
    return path.endsWith(QStringLiteral(".Rev"), Qt::CaseInsensitive);
}

} // namespace revstudio
