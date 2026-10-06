#ifndef REVSTUDIO_EDITORTABS_H
#define REVSTUDIO_EDITORTABS_H

#include <QFont>
#include <QTabWidget>

namespace revstudio {

class ConsoleWidget;
class ScriptEditor;
class Settings;

/**
 * G6: the editor tabs (GUI_Implementation_Note.md, section 6.1's mockup -- "the editor tabs are the central
 * widget" -- and section 6.8). Owns a `ScriptEditor` per tab, dirty-aware tab titles and close prompts, recent
 * files and a simple session restore (which files were open; NOT their unsaved content -- section 6.8's fuller
 * "crash recovery of unsaved buffers" is trimmed from this v1, the same kind of scope line the task table draws
 * elsewhere, e.g. G1's deferred restart button). Routes "run" actions through the console
 * (`ConsoleWidget::submitText`), not `Session::submit()` directly, matching G4/G5's panels.
 */
class EditorTabs : public QTabWidget
{
    Q_OBJECT

public:
    explicit EditorTabs(ConsoleWidget* console, Settings* settings, QWidget* parent = nullptr);

    ScriptEditor* newDocument();
    bool openFile(const QString& path);   // focuses the existing tab if already open
    /** Q7's error navigation: openFile() plus moving the caret to `line` (1-based) once it is open. */
    bool openFileAtLine(const QString& path, int line);
    bool saveCurrent();
    bool saveCurrentAs();

    /** Prompts to save/discard/cancel for each dirty tab (in order); stops and returns false on the first
     *  Cancel. Used both for closing one tab and (in a loop, from MainWindow's closeEvent) for quitting. */
    bool closeTabWithPrompt(int index);
    bool closeAllWithPrompt();

    void runSelectionOrLine();   // Ctrl+Enter
    void runCurrentFile();       // F5

    QStringList openFilePaths() const;   // for session restore; untitled tabs are not included
    bool hasUnsavedChanges() const;

    /** Q8's F1: the current tab's ScriptEditor::wordUnderCursor(), or "" if there is no current tab. */
    QString currentWordUnderCursor() const;

    /** G7/Preferences: applies to every open tab now, and is remembered for tabs opened after this (newDocument()/
     *  openFile() apply it to each new ScriptEditor as it is created). */
    void applyFontToAllTabs(const QFont& font);

signals:
    void currentCursorPositionChanged(int line, int column);

private:
    ScriptEditor* currentEditor() const;
    ScriptEditor* editorAt(int index) const;
    void updateTabTitle(int index);
    void connectEditorSignals(ScriptEditor* editor);
    QString titleFor(const ScriptEditor* editor) const;
    QString displayNameFor(const ScriptEditor* editor) const;   // titleFor() without the dirty "*" suffix

    ConsoleWidget* console_;
    Settings*      settings_;
    QFont           currentFont_;
    int             untitledCounter_ = 0;
};

} // namespace revstudio

#endif
