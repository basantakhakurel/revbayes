/**
 * @file
 * Declaration of RevLanguage::complete(), the tab-completion logic shared by the terminal client's linenoise
 * callback (RevClient.cpp) and `rb --server`'s `complete` command (RevServer.cpp).
 *
 * Extracted (GUI_Implementation_Note.md, defect/task C1) from what used to be RevClient.cpp's completeOnTab() and
 * getDefaultCompletions(), which were pure logic -- built on Parser::checkCommand -- wrapped around linenoise's
 * callback types. This file has no linenoise dependency, so it can be called from a context (the server) that has
 * no terminal at all, and the two callers now share one implementation instead of the server needing its own copy.
 *
 * @license GPL version 3
 */

#ifndef Completion_H
#define Completion_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace RevLanguage {

    class Environment;

    //! One completion candidate. `text` is the FULL replacement text for the completed region (not just the
    //! typed prefix plus a suffix), matching the protocol's "Completion item" (GUI_Implementation_Note.md,
    //! section 6.4): `kind` is one of "function", "variable", "type", "argument" or "file".
    struct CompletionItem
    {
        std::string text;
        std::string kind;
    };

    //! Returns the completions for `buffer` at `cursor` (a byte offset, clamped to [0, buffer.size()] -- only the
    //! text up to the cursor is ever considered, matching linenoise's own behaviour of only completing the text
    //! before the caret). `env` is the environment to complete variable/type names against (in practice always
    //! Workspace::userWorkspacePtr()). Sets *replace_from to the byte offset in `buffer` from which the returned
    //! items' `text` replaces the existing content -- a caller inserting item.text at the cursor must first erase
    //! buffer[replace_from:cursor].
    std::vector<CompletionItem> complete(const std::string& buffer, std::size_t cursor,
                                          const std::shared_ptr<Environment>& env, std::size_t& replace_from);

}

#endif
