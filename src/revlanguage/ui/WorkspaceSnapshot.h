/**
 * @file
 * Declaration of WorkspaceSnapshot: builds the `rows` of a protocol `variables` event from an Environment's
 * variable table (GUI_Implementation_Note.md, sections 6.4 and 6.6).
 *
 * Interpreter thread only: reads RevPtr<RevVariable>/RevObject/DagNode, none of which are safe to touch from
 * another thread while the interpreter may be running (section 4.1).
 *
 * @license GPL version 3
 */

#ifndef WorkspaceSnapshot_H
#define WorkspaceSnapshot_H

#include "nlohmann-json.h"

#include <cstddef>

namespace RevLanguage {

    class Environment;
    class FunctionTable;
    class RevObject;

    namespace WorkspaceSnapshot {

        //! Builds one row per entry of env's variable table, in the shape documented in section 6.4:
        //! name, type, kind (constant/deterministic/stochastic/clamped/workspace/unknown), summary (bounded,
        //! see boundedSummary below), summary_truncated (only present if true), children (only present for a
        //! DAG node), parent (only present for an element variable such as "x[1]"), flags (reference/hidden/
        //! element/system -- possibly empty).
        //!
        //! Building a row never throws: a variable whose value cannot be inspected (getRevObject() failing, for
        //! example an incompletely built vector variable) gets kind "unknown" and a summary describing why,
        //! rather than aborting the whole snapshot.
        nlohmann::json variableRows(Environment& env);

        //! Builds one row per entry of a function table -- one row per overload, since FunctionTable is a
        //! multimap and the same name can have several (section 6.4's "User-defined functions (name,
        //! signature)"). `signature` is "returnType name(arg1, arg2)" (argument labels only, no types), the same
        //! format the terminal client's printFunctionParameters (RevClient.cpp) shows on a function-hint keypress.
        nlohmann::json functionRows(const FunctionTable& table);

        //! Prints obj's value with RevObject::printValue, stopping (not just truncating the RESULT, but actually
        //! stopping the underlying print loop) once max_chars characters have been written, so a huge container
        //! costs bounded time as well as bounded space to summarize (GUI_Implementation_Note.md, section 4.6: do
        //! not call printValue on an unbounded container). Sets *truncated (if given) to whether it stopped early.
        std::string boundedSummary(const RevObject& obj, std::size_t max_chars, bool* truncated = nullptr);

    }

}

#endif
