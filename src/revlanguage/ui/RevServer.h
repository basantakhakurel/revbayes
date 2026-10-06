/**
 * @file
 * Declaration of RevServer, the backend behind `rb --server`.
 *
 * `rb --server` lets a graphical front end (for example RevStudio) drive the interpreter
 * through a pipe. The wire format is newline-delimited JSON (one object per line, UTF-8)
 * on the process's stdin and stdout; see GUI_Implementation_Note.md, section 6.4, for the
 * protocol. stderr is not part of the protocol.
 *
 * STATUS: phase 1, through S9. `hello`, `ping`, `interrupt`, `shutdown`, `submit` (including continuation and
 * output streaming), `snapshot` (`what: "variables"`, `"functions"` or `"all"`, via WorkspaceSnapshot),
 * `ask`/`answer` (requestAsk()), `inspect` (processInspect(), capped at max_inspect_bytes), `complete`
 * (processComplete(), via Completion.h -- shared with the terminal client) and `help` (processHelp(), via
 * RbHelpSystem/RbHelpDatabase) are implemented. Only `set` still answers `not_implemented`. Real interruption of a
 * running `submit` needs a core interrupt flag (phase 3); `interrupt` today only reports whether the interpreter
 * was busy.
 *
 * Threading: a background "reader" thread owns stdin and answers everything that does not touch the interpreter
 * (hello, ping, interrupt, shutdown, and errors) directly. Anything that does touch the interpreter (submit,
 * snapshot) is handed to the main thread's request queue instead; the main thread is the only thread that ever
 * calls into the Rev parser or the workspace, matching the single-threaded, global-state nature of the
 * interpreter (GUI_Implementation_Note.md, section 4.1).
 *
 * @brief Declaration of RevServer
 *
 * @license GPL version 3
 */

#ifndef RevServer_H
#define RevServer_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "OutputCapture.h"
#include "nlohmann-json.h"

namespace RevLanguage {

    class RevServer {

    public:
        static constexpr int        protocol_version = 1;                                   //!< Wire protocol version
        static constexpr std::size_t max_request_bytes = 16 * 1024 * 1024;                  //!< Requests larger than this are rejected
        static constexpr std::size_t max_inspect_bytes = 64 * 1024;                          //!< `inspection.text` cap (section 6.3)
        static constexpr int         watchdog_timeout_ms = 10000;                            //!< See startWatchdog()

        explicit                    RevServer(const std::string& server_name);              //!< e.g. "RevBayes 1.4.2-preview"
                                    RevServer(const RevServer&) = delete;
        RevServer&                  operator=(const RevServer&) = delete;
                                    ~RevServer(void);

        bool                        takeOverChannels(void);                                 //!< Move the protocol to a private duplicate of stdout
        int                         run(void);                                              //!< Serve requests until shutdown, EOF or a protocol error; returns the exit code

        //! Forward one chunk of interpreter output as an `output` event, tagged with whichever `submit` is
        //! currently executing (if any) -- UNLESS processInspect() is running, in which case the text is
        //! accumulated into its capture buffer instead (bounded to max_inspect_bytes) rather than streamed.
        //! Called synchronously by OutputCapture's sink; interpreter thread only.
        void                        emitOutput(const std::string& text, OutputCapture::Stream which);

        //! Sends `quit` then `bye`. Called by the quitRequestHandler hook installed on RevLanguage::Parser (see
        //! Parser.h) when Rev code calls quit(); the hook returns afterwards and lets the existing
        //! RevClient::shutdown()+exit() path run, so this method must not itself try to stop anything.
        void                        reportQuit(const std::string& reason);

        //! Sends an `ask` event and BLOCKS the calling thread until a matching `answer` request arrives on the
        //! reader thread, or the server is shutting down / stdin hits EOF. Called by the UserInterface::ask()
        //! hook (see RlUserInterface.h) installed by RevClient::startServer(); always runs on the main/interpreter
        //! thread, never the reader thread, since that is the only thread that ever calls into Rev code. Returns
        //! the answered value, or false (decline) if no answer ever arrives.
        bool                        requestAsk(const std::string& question);

        static nlohmann::json       serverInfo(const std::string& server_name);            //!< What `rb --server-info` prints

    private:
        //! One request queued for the main/interpreter thread. `cmd` is "submit", "snapshot", "inspect",
        //! "complete" or "help". Which of `text`/`what`/`cursor` is meaningful depends on `cmd`: submit uses
        //! `text` (the code); snapshot uses `what` ("variables"/"functions"/"all"); inspect uses `what` (the
        //! variable name); help uses `what` (the topic); complete uses `text` (the buffer) and `cursor`. Kept as
        //! one small struct rather than a variant, reusing fields across commands: the set of commands that need
        //! the interpreter is small, and every one of them needs at most two pieces of request data.
        struct PendingCommand { nlohmann::json id; std::string cmd; std::string text; std::string what; std::size_t cursor = 0; };

        void                        readerThreadMain(void);                                 //!< Runs on the background reader thread
        void                        handleLine(const std::string& line);                    //!< Parse and dispatch one request line (reader thread)
        void                        processSubmit(const nlohmann::json& id, const std::string& text);  //!< Runs a queued submit (main/interpreter thread)
        void                        processSnapshot(const nlohmann::json& id, const std::string& what); //!< Runs a queued snapshot (main/interpreter thread)
        //! Runs `structure(name, verbose=TRUE)` with its RBOUT output captured (rather than streamed as `output`
        //! events) into one `inspection` event, capped at max_inspect_bytes (main/interpreter thread).
        void                        processInspect(const nlohmann::json& id, const std::string& name);
        //! Builds a `completions` event from Completion.h's complete() against the current workspace
        //! (main/interpreter thread -- not because completion itself touches mutable interpreter state in a way
        //! that is unsafe to read concurrently, but to keep `complete`'s busy/Idle-only rule (section 6.4)
        //! implemented the same way as every other command that needs the interpreter, through one queue).
        void                        processComplete(const nlohmann::json& id, const std::string& buffer, std::size_t cursor);
        //! Builds a `help` event from RbHelpSystem/RbHelpDatabase for `topic` (main/interpreter thread; see
        //! processComplete()'s comment on why this is queued at all despite not touching the workspace).
        void                        processHelp(const nlohmann::json& id, const std::string& topic);
        //! Sets stop_requested and wakes every thread that might be waiting on that fact: queue_cv (the interpreter
        //! loop in run()) and ask_cv (a requestAsk() call blocked mid-submit, if any). Centralised so every site
        //! that can decide to stop the server -- EOF, `shutdown`, an unsupported protocol version, a dead peer --
        //! automatically also breaks a pending ask instead of leaving requestAsk() to hang forever.
        void                        requestStop(void);
        //! Builds and sends a `variables` event from the current workspace (interpreter thread only). `reply_to`,
        //! if not null, is attached as `re` -- used for an on-demand `snapshot` reply; omitted for the automatic
        //! broadcast that follows every submit's `done` (section 6.4).
        void                        sendVariablesEvent(const nlohmann::json& reply_to = nlohmann::json( nullptr ));
        //! Builds and sends a `functions` event listing the user workspace's function table (interpreter thread
        //! only); see WorkspaceSnapshot::functionRows(). `reply_to` is always a `snapshot` request's id: unlike
        //! `variables`, there is no automatic broadcast of this one.
        void                        sendFunctionsEvent(const nlohmann::json& reply_to);
        //! Force-exit if the interpreter thread does not return within watchdog_timeout_ms of a stop request. Runs
        //! on a detached thread that checks a file-static flag (not a member): there is exactly one RevServer per
        //! process, and a detached thread must not read a member of an object whose lifetime it does not control.
        static void                 startWatchdog(void);

        void                        send(const nlohmann::json& message);                    //!< Write one event; thread safe
        void                        sendError(const nlohmann::json& request_id,
                                              const std::string& code,
                                              const std::string& message);                 //!< Write an `error` event
        static void                 addReplyTo(nlohmann::json& event, const nlohmann::json& request_id);
        static std::string          currentDirectoryUtf8(void);

        std::string                 server_name;
        int                         protocol_fd;                                            //!< Private duplicate of the original stdout, -1 until taken over
        std::mutex                  write_mutex;                                             //!< Guards writes to protocol_fd
        bool                        said_hello;                                              //!< Reader thread only
        int                         exit_code;                                               //!< Main thread only, read by run() at the end

        // Handoff between the reader thread and the main (interpreter) thread.
        std::thread                 reader_thread;
        std::mutex                  queue_mutex;                                             //!< Guards command_queue and busy
        std::condition_variable     queue_cv;
        std::queue<PendingCommand>  command_queue;                                         //!< At most one entry in practice: busy rejects a second interpreter-bound command before it is queued
        std::atomic<bool>           stop_requested;                                          //!< Atomic (not queue_mutex-guarded) so ask_cv's wait predicate can read it without taking queue_mutex, see requestStop()
        std::atomic<bool>           busy;                                                    //!< A submit is queued or executing; read by the reader thread, written by both

        // Handoff for a nested `ask` mid-submit: the interpreter thread (inside requestAsk()) blocks here until the
        // reader thread's `answer` handling in handleLine() delivers a value, or requestStop() fires. Kept separate
        // from queue_mutex/queue_cv, which hand off a whole command, not a nested request-inside-a-submit.
        std::mutex                  ask_mutex;
        std::condition_variable     ask_cv;
        bool                        ask_pending;                                             //!< Guarded by ask_mutex; true from requestAsk() until answered or abandoned
        long long                   ask_id;                                                   //!< Guarded by ask_mutex; the pending ask's id, valid only while ask_pending
        bool                        ask_answered;                                             //!< Guarded by ask_mutex; set by handleLine()'s `answer` branch
        bool                        ask_answer_value;                                         //!< Guarded by ask_mutex; valid only once ask_answered
        long long                   next_ask_id;                                              //!< Interpreter thread only: requestAsk() is never re-entrant (one interpreter thread, one submit at a time)

        // Main (interpreter) thread only: no locking needed, since only that thread ever touches these.
        std::string                 continuation_buffer;                                   //!< Continuation state: the accumulated text of an "incomplete" statement
        nlohmann::json              current_submit_id;                                        //!< The `id` of the submit currently executing, or null
        long long                   variables_revision;                                       //!< Incremented on every `variables` event sent
        std::string*                inspect_capture;                                          //!< Non-null only during processInspect(): emitOutput() appends here instead of streaming
        bool                        inspect_truncated;                                        //!< Set once *inspect_capture has reached max_inspect_bytes
    };

}

#endif
