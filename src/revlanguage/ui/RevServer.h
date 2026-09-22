/**
 * @file
 * Declaration of RevServer, the backend behind `rb --server`.
 *
 * `rb --server` lets a graphical front end (for example RevStudio) drive the interpreter
 * through a pipe. The wire format is newline-delimited JSON (one object per line, UTF-8)
 * on the process's stdin and stdout; see GUI_Implementation_Note.md, section 6.4, for the
 * protocol. stderr is not part of the protocol.
 *
 * STATUS: phase 0 stub. It implements the framing, the channel takeover and the commands
 * `hello`, `ping`, `interrupt` and `shutdown`. Every other command of protocol 1 answers with
 * error code `not_implemented`. The interpreter is not driven yet (phase 1).
 *
 * @brief Declaration of RevServer
 *
 * @license GPL version 3
 */

#ifndef RevServer_H
#define RevServer_H

#include <cstddef>
#include <mutex>
#include <string>

#include "nlohmann-json.h"

namespace RevLanguage {

    class RevServer {

    public:
        static constexpr int        protocol_version = 1;                                   //!< Wire protocol version
        static constexpr std::size_t max_request_bytes = 16 * 1024 * 1024;                  //!< Requests larger than this are rejected

        explicit                    RevServer(const std::string& server_name);              //!< e.g. "RevBayes 1.4.2-preview"
                                    RevServer(const RevServer&) = delete;
        RevServer&                  operator=(const RevServer&) = delete;
                                    ~RevServer(void);

        bool                        takeOverChannels(void);                                 //!< Move the protocol to a private duplicate of stdout
        int                         run(void);                                              //!< Serve requests until shutdown, EOF or a protocol error; returns the exit code

        static nlohmann::json       serverInfo(const std::string& server_name);            //!< What `rb --server-info` prints

    private:
        void                        handleLine(const std::string& line);                   //!< Parse and dispatch one request line
        void                        send(const nlohmann::json& message);                   //!< Write one event; thread safe
        void                        sendError(const nlohmann::json& request_id,
                                              const std::string& code,
                                              const std::string& message);                 //!< Write an `error` event
        static void                 addReplyTo(nlohmann::json& event, const nlohmann::json& request_id);
        static std::string          currentDirectoryUtf8(void);

        std::string                 server_name;
        int                         protocol_fd;                                            //!< Private duplicate of the original stdout, -1 until taken over
        std::mutex                  write_mutex;
        bool                        said_hello;
        bool                        stop_requested;
        int                         exit_code;
    };

}

#endif
