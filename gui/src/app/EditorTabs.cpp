#include "app/EditorTabs.h"

#include "app/ConsoleWidget.h"
#include "app/RevString.h"
#include "app/ScriptEditor.h"
#include "app/Settings.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QTextBlock>
#include <QTextDocument>

namespace revstudio {

EditorTabs::EditorTabs(ConsoleWidget* console, Settings* settings, QWidget* parent)
    : QTabWidget(parent), console_(console), settings_(settings), currentFont_(settings_->editorFont())
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);

    connect(this, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTabWithPrompt(index); });
    connect(this, &QTabWidget::currentChanged, this, [this](int index)
    {
        if (ScriptEditor* editor = editorAt(index))
        {
            emit currentCursorPositionChanged(editor->cursorLine(), editor->cursorColumn());
        }
    });
}

ScriptEditor* EditorTabs::newDocument()
{
    auto* editor = new ScriptEditor(this);
    editor->applyFont(currentFont_);
    editor->setProperty("untitledTitle", tr("Untitled %1").arg(++untitledCounter_));
    connectEditorSignals(editor);
    const int index = addTab(editor, titleFor(editor));
    setCurrentIndex(index);
    editor->setFocus();
    return editor;
}

bool EditorTabs::openFile(const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty())
    {
        for (int i = 0; i < count(); ++i)
        {
            ScriptEditor* existing = editorAt(i);
            if (existing && QFileInfo(existing->filePath()).canonicalFilePath() == canonical)
            {
                setCurrentIndex(i);
                return true;
            }
        }
    }

    auto* editor = new ScriptEditor(this);
    editor->applyFont(currentFont_);
    QString error;
    if (!editor->loadFromFile(path, &error))
    {
        delete editor;
        QMessageBox::warning(this, tr("Open File"), tr("Could not open \"%1\":\n%2").arg(path, error));
        return false;
    }
    connectEditorSignals(editor);
    const int index = addTab(editor, titleFor(editor));
    setCurrentIndex(index);
    editor->setFocus();
    settings_->addRecentFile(path);
    return true;
}

bool EditorTabs::openFileAtLine(const QString& path, int line)
{
    if (!openFile(path))
    {
        return false;
    }
    if (ScriptEditor* editor = currentEditor())
    {
        editor->goToLine(line);
    }
    return true;
}

bool EditorTabs::saveCurrent()
{
    ScriptEditor* editor = currentEditor();
    if (!editor)
    {
        return false;
    }
    if (editor->isUntitled())
    {
        return saveCurrentAs();
    }
    QString error;
    if (!editor->saveToFile(editor->filePath(), &error))
    {
        QMessageBox::warning(this, tr("Save File"), tr("Could not save \"%1\":\n%2").arg(editor->filePath(), error));
        return false;
    }
    updateTabTitle(currentIndex());
    return true;
}

bool EditorTabs::saveCurrentAs()
{
    ScriptEditor* editor = currentEditor();
    if (!editor)
    {
        return false;
    }
    const QString startPath = editor->isUntitled() ? QString() : editor->filePath();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save File As"), startPath,
                                                       tr("Rev Scripts (*.Rev);;All Files (*)"));
    if (path.isEmpty())
    {
        return false;
    }
    QString error;
    if (!editor->saveToFile(path, &error))
    {
        QMessageBox::warning(this, tr("Save File"), tr("Could not save \"%1\":\n%2").arg(path, error));
        return false;
    }
    updateTabTitle(currentIndex());
    settings_->addRecentFile(path);
    return true;
}

bool EditorTabs::closeTabWithPrompt(int index)
{
    ScriptEditor* editor = editorAt(index);
    if (!editor)
    {
        return true;
    }

    if (editor->isModified())
    {
        setCurrentIndex(index);
        const auto choice = QMessageBox::question(
            this, tr("Unsaved Changes"), tr("Save changes to \"%1\" before closing?").arg(displayNameFor(editor)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (choice == QMessageBox::Cancel)
        {
            return false;
        }
        if (choice == QMessageBox::Save && !saveCurrent())
        {
            return false;   // the save itself failed, or a Save As dialog for an untitled tab was cancelled
        }
    }

    removeTab(index);
    editor->deleteLater();
    return true;
}

bool EditorTabs::closeAllWithPrompt()
{
    while (count() > 0)
    {
        if (!closeTabWithPrompt(0))
        {
            return false;
        }
    }
    return true;
}

void EditorTabs::runSelectionOrLine()
{
    ScriptEditor* editor = currentEditor();
    if (!editor)
    {
        return;
    }
    QTextCursor cursor = editor->textCursor();
    const bool hadSelection = cursor.hasSelection();

    QString text;
    int endBlock = 0;
    bool hasRealContent;
    if (hadSelection)
    {
        text = cursor.selectedText();
        text.replace(QChar(0x2029), QLatin1Char('\n'));   // QTextCursor::selectedText() uses U+2029, not '\n'
        endBlock = editor->document()->findBlock(cursor.selectionEnd()).blockNumber();
        hasRealContent = !text.trimmed().isEmpty();   // an explicit selection runs exactly what was selected
    }
    else
    {
        // Statement-aware, RStudio/VSCode-R style (6.8/Q7, pulled forward from phase 3): a multi-line
        // `for(...) { ... }` runs as one submission no matter which of its lines the cursor is on, instead of
        // sending just that one line (which the backend would then -- correctly -- report as incomplete, or
        // worse, run as a fragment that happens to parse on its own).
        text = editor->currentStatementText(&endBlock);
        // Comments are skipped by default, same convention: a statement made up ENTIRELY of comment and/or
        // blank lines (the common case being the cursor just sitting on one) has nothing to send -- unlike the
        // selection branch above, this is not something the user explicitly asked to run.
        hasRealContent = false;
        for (const QString& line : text.split(QLatin1Char('\n')))
        {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#')))
            {
                hasRealContent = true;
                break;
            }
        }
    }

    if (hasRealContent)
    {
        console_->submitText(text);
    }

    // Ctrl+Enter runs and moves on to the next real statement, skipping blank and comment-only lines, like
    // RStudio/VSCode-R's own Ctrl+Enter -- so repeated presses step through actual code, not decoration -- and
    // stays in the editor rather than following the command to the console (ConsoleWidget::submit() already
    // leaves focus alone for a submission that did not come from its own input; this is the other half, since
    // nothing else would otherwise put focus back in the editor after the run).
    QTextDocument* doc = editor->document();
    const auto isBlankOrComment = [doc](int block)
    {
        const QString trimmed = doc->findBlockByNumber(block).text().trimmed();
        return trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'));
    };
    int nextBlock = endBlock + 1;
    while (nextBlock < doc->blockCount() - 1 && isBlankOrComment(nextBlock))
    {
        ++nextBlock;
    }
    nextBlock = qMin(nextBlock, doc->blockCount() - 1);

    editor->setTextCursor(QTextCursor(doc->findBlockByNumber(nextBlock)));
    editor->setFocus();
    editor->ensureCursorVisible();
}

void EditorTabs::runCurrentFile()
{
    ScriptEditor* editor = currentEditor();
    if (!editor)
    {
        return;
    }
    if (editor->isUntitled())
    {
        console_->submitText(editor->toPlainText());
        editor->setFocus();
        return;
    }
    if (editor->isModified() && !saveCurrent())
    {
        return;
    }
    // source(), not the raw buffer: so a failure reports a real file and line (section 6.8's "error navigation").
    console_->submitText(QStringLiteral("source(%1)").arg(RevString::quote(editor->filePath())));
    editor->setFocus();
}

QStringList EditorTabs::openFilePaths() const
{
    QStringList paths;
    for (int i = 0; i < count(); ++i)
    {
        ScriptEditor* editor = editorAt(i);
        if (editor && !editor->isUntitled())
        {
            paths.append(editor->filePath());
        }
    }
    return paths;
}

bool EditorTabs::hasUnsavedChanges() const
{
    for (int i = 0; i < count(); ++i)
    {
        if (ScriptEditor* editor = editorAt(i); editor && editor->isModified())
        {
            return true;
        }
    }
    return false;
}

QString EditorTabs::currentWordUnderCursor() const
{
    if (ScriptEditor* editor = currentEditor())
    {
        return editor->wordUnderCursor();
    }
    return QString();
}

void EditorTabs::applyFontToAllTabs(const QFont& font)
{
    currentFont_ = font;
    for (int i = 0; i < count(); ++i)
    {
        if (ScriptEditor* editor = editorAt(i))
        {
            editor->applyFont(currentFont_);
        }
    }
}

ScriptEditor* EditorTabs::currentEditor() const
{
    return editorAt(currentIndex());
}

ScriptEditor* EditorTabs::editorAt(int index) const
{
    return qobject_cast<ScriptEditor*>(widget(index));
}

void EditorTabs::updateTabTitle(int index)
{
    if (ScriptEditor* editor = editorAt(index))
    {
        setTabText(index, titleFor(editor));
    }
}

void EditorTabs::connectEditorSignals(ScriptEditor* editor)
{
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor](bool)
    {
        const int index = indexOf(editor);
        if (index >= 0)
        {
            updateTabTitle(index);
        }
    });
    connect(editor, &ScriptEditor::cursorPositionMoved, this, [this, editor](int line, int column)
    {
        if (editor == currentEditor())
        {
            emit currentCursorPositionChanged(line, column);
        }
    });
    connect(editor, &ScriptEditor::externallyModified, this, [this, editor]
    {
        const int index = indexOf(editor);
        if (index < 0)
        {
            return;
        }
        setCurrentIndex(index);
        const auto choice = QMessageBox::question(
            this, tr("File Changed"),
            tr("\"%1\" was changed outside RevStudio. Reload it? Unsaved changes here will be lost.")
                .arg(displayNameFor(editor)),
            QMessageBox::Yes | QMessageBox::No);
        if (choice == QMessageBox::Yes)
        {
            QString error;
            if (!editor->loadFromFile(editor->filePath(), &error))
            {
                QMessageBox::warning(this, tr("Reload File"), tr("Could not reload \"%1\":\n%2").arg(editor->filePath(), error));
            }
        }
    });
}

QString EditorTabs::displayNameFor(const ScriptEditor* editor) const
{
    return editor->isUntitled() ? editor->property("untitledTitle").toString() : QFileInfo(editor->filePath()).fileName();
}

QString EditorTabs::titleFor(const ScriptEditor* editor) const
{
    QString title = displayNameFor(editor);
    if (editor->isModified())
    {
        title += QLatin1Char('*');
    }
    return title;
}

} // namespace revstudio
