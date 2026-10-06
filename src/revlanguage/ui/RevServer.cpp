#include "RevServer.h"

#include "Completion.h"
#include "Parser.h"
#include "RbException.h"
#include "RbHelpDatabase.h"
#include "RbHelpRenderer.h"
#include "RbHelpSystem.h"
#include "RbSettings.h"
#include "RbUtil.h"
#include "Workspace.h"
#include "WorkspaceSnapshot.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#if defined (_WIN32)
#   include <fcntl.h>
#   include <io.h>
#   include <process.h>
#else
#   include <csignal>
#   include <unistd.h>
#endif

using namespace RevLanguage;

using json = nlohmann::json;


namespace {

    int processId(void)
    {
#       if defined (_WIN32)
        return _getpid();
#       else
        return int( getpid() );
#       endif
    }

    /** Write all of the text to the file descriptor. Returns false if the peer is gone. */
    bool writeAll(int fd, const std::string& text)
    {
        size_t offset = 0;
        while ( offset < text.size() )
        {
#           if defined (_WIN32)
            int n = _write( fd, text.data() + offset, static_cast<unsigned int>( text.size() - offset ) );
#           else
            ssize_t n = ::write( fd, text.data() + offset, text.size() - offset );
#           endif
            if ( n < 0 && errno == EINTR )
            {
                continue;
            }
            if ( n <= 0 )
            {
                return false;
            }
            offset += static_cast<size_t>( n );
        }
        return true;
    }

    std::string toUtf8(const std::filesystem::path& p)
    {
        // u8string() is std::string before C++20 and std::u8string from C++20 on; this works for both.
        auto s = p.u8string();
        return std::string( reinterpret_cast<const char*>( s.data() ), s.size() );
    }

    const char* streamName(OutputCapture::Stream which)
    {
        switch ( which )
        {
            case OutputCapture::Stream::RBOut:  return "rbout";
            case OutputCapture::Stream::Stdout: return "stdout";
            case OutputCapture::Stream::Stderr: return "stderr";
        }
        return "stderr";
    }

    // File-static, not a RevServer member: startWatchdog() runs on a DETACHED thread, so whatever it reads must
    // have static storage duration. There is exactly one RevServer per process, so this is equivalent to a member
    // in every way that matters here, without the dangling-pointer risk of a detached thread outliving `this`.
    std::atomic<bool> interpreterLoopFinished{ false };

}


RevServer::RevServer(const std::string& n) :
    server_name( n ),
    protocol_fd( -1 ),
    said_hello( false ),
    exit_code( 0 ),
    stop_requested( false ),
    busy( false ),
    ask_pending( false ),
    ask_id( 0 ),
    ask_answered( false ),
    ask_answer_value( false ),
    next_ask_id( 0 ),
    current_submit_id( nullptr ),
    variables_revision( 0 ),
    inspect_capture( nullptr ),
    inspect_truncated( false )
{

}


void RevServer::requestStop(void)
{
    {
        std::lock_guard<std::mutex> lock( queue_mutex );
        stop_requested = true;
    }
    queue_cv.notify_all();
    ask_cv.notify_all();   // wake a requestAsk() blocked mid-submit, if any; see its wait predicate
}


RevServer::~RevServer(void)
{
    // The protocol descriptor is deliberately left open: the process is about to exit, and closing it early could
    // truncate an event that is still being written by another thread.
    if ( reader_thread.joinable() )
    {
        reader_thread.detach();   // run() already tried to join; this is only reached if run() was never called
    }
}


/**
 * Take over the process's standard channels.
 *
 * The protocol travels on a private duplicate of the ORIGINAL stdout. File descriptor 1 is then redirected to
 * stderr, so anything that still writes to stdout behind our back (printf, third-party libraries) can never
 * corrupt the protocol; it shows up on stderr instead. Design verified in a Linux probe, see section 1 of
 * GUI_Implementation_Note.md. std::cout/std::cerr themselves are handled separately, by OutputCapture.
 *
 * On Windows both standard streams are switched to binary mode, otherwise "\n" would be rewritten to "\r\n" and
 * Ctrl-Z would look like end of input. (Windows path unverified as of phase 0.)
 */
bool RevServer::takeOverChannels(void)
{
    if ( protocol_fd >= 0 )
    {
        return true;
    }

    std::cout.flush();
    std::fflush( stdout );

#   if defined (_WIN32)
    _setmode( _fileno( stdin ), _O_BINARY );
    _setmode( _fileno( stdout ), _O_BINARY );
    protocol_fd = _dup( _fileno( stdout ) );
    if ( protocol_fd >= 0 )
    {
        _setmode( protocol_fd, _O_BINARY );
        if ( _dup2( _fileno( stderr ), _fileno( stdout ) ) != 0 )
        {
            protocol_fd = -1;
        }
    }
#   else
    // A vanished front end must give write() an EPIPE error, not kill the process with SIGPIPE.
    std::signal( SIGPIPE, SIG_IGN );

    protocol_fd = ::dup( STDOUT_FILENO );
    if ( protocol_fd >= 0 && ::dup2( STDERR_FILENO, STDOUT_FILENO ) < 0 )
    {
        ::close( protocol_fd );
        protocol_fd = -1;
    }
#   endif

    if ( protocol_fd < 0 )
    {
        std::cerr << "rb --server: could not take over the standard streams (errno " << errno << ")." << std::endl;
        return false;
    }

    return true;
}


/**
 * Starts the reader thread, then becomes the interpreter loop: wait for a queued command, run it, repeat, until
 * `stop_requested` is set (by a `shutdown` command or by end of input) and the queue is empty. Returns the process
 * exit code: 0 for a normal end, 2 for a protocol error, 1 if the channels were not taken over.
 */
int RevServer::run(void)
{
    if ( protocol_fd < 0 )
    {
        return 1;
    }

    reader_thread = std::thread( &RevServer::readerThreadMain, this );

    while ( true )
    {
        PendingCommand item;
        {
            std::unique_lock<std::mutex> lock( queue_mutex );
            queue_cv.wait( lock, [this] { return not command_queue.empty() or stop_requested; } );

            if ( command_queue.empty() )
            {
                if ( stop_requested )
                {
                    break;
                }
                continue;   // spurious wake-up
            }

            item = std::move( command_queue.front() );
            command_queue.pop();
        }

        if ( item.cmd == "submit" )
        {
            processSubmit( item.id, item.text );
        }
        else if ( item.cmd == "snapshot" )
        {
            processSnapshot( item.id, item.what );
        }
        else if ( item.cmd == "inspect" )
        {
            processInspect( item.id, item.what );
        }
        else if ( item.cmd == "complete" )
        {
            processComplete( item.id, item.text, item.cursor );
        }
        else if ( item.cmd == "help" )
        {
            processHelp( item.id, item.what );
        }
    }

    interpreterLoopFinished = true;

    if ( reader_thread.joinable() )
    {
        reader_thread.join();
    }

    return exit_code;
}


nlohmann::json RevServer::serverInfo(const std::string& server_name)
{
    json info;
    info["protocol"] = protocol_version;
    info["version"]  = server_name;
    // "interrupt" is deliberately not listed: it already answers (ack{was_busy}), but only real cancellation of
    // a running submit (phase 3's C4, a core interrupt flag) would make it a capability worth a GUI building on.
    info["features"] = { "submit", "snapshot", "complete", "help", "inspect", "functions" };
    return info;
}


void RevServer::readerThreadMain(void)
{
    std::string line;
    while ( true )
    {
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            if ( stop_requested )
            {
                break;
            }
        }

        if ( not std::getline( std::cin, line ) )
        {
            break;   // end of input
        }

        if ( not line.empty() and line.back() == '\r' )
        {
            line.pop_back();
        }
        if ( line.empty() )
        {
            continue;
        }

        handleLine( line );
    }

    // Either end of input or a `shutdown` already handled below. Either way, make sure the interpreter loop wakes
    // up and eventually returns even if it is idle right now; if it is mid-submit, run() will notice once that
    // finishes (there is no way to interrupt it without a core change, see GUI_Implementation_Note.md section 4.4).
    requestStop();
    startWatchdog();
}


void RevServer::startWatchdog(void)
{
    std::thread( []
    {
        for ( int waited = 0; waited < watchdog_timeout_ms; waited += 100 )
        {
            if ( interpreterLoopFinished.load() )
            {
                return;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }
        if ( not interpreterLoopFinished.load() )
        {
            // The interpreter thread is stuck (almost certainly mid-submit, with no way to interrupt it yet).
            // _Exit, not exit: skip C++ destructors and atexit handlers entirely, since the stuck main thread may
            // hold locks or be in the middle of mutating state that those would touch.
            std::_Exit( 1 );
        }
    } ).detach();
}


void RevServer::handleLine(const std::string& line)
{
    if ( line.size() > max_request_bytes )
    {
        sendError( nullptr, "bad_request", "request too large" );
        return;
    }

    json request = json::parse( line, nullptr, /* allow_exceptions */ false );
    if ( request.is_discarded() )
    {
        sendError( nullptr, "bad_json", "the request is not valid JSON" );
        return;
    }
    if ( not request.is_object() )
    {
        sendError( nullptr, "bad_request", "a request must be a JSON object" );
        return;
    }

    json id = request.contains( "id" ) ? request["id"] : json( nullptr );

    if ( not request.contains( "cmd" ) or not request["cmd"].is_string() )
    {
        sendError( id, "bad_request", "a request needs a string field 'cmd'" );
        return;
    }
    const std::string cmd = request["cmd"].get<std::string>();

    // hello ---------------------------------------------------------------------------------------------------
    if ( cmd == "hello" )
    {
        if ( said_hello )
        {
            sendError( id, "bad_request", "hello was already received" );
            return;
        }
        if ( not request.contains( "protocol" ) or not request["protocol"].is_number_integer() )
        {
            sendError( id, "bad_request", "hello needs an integer field 'protocol'" );
            return;
        }
        if ( request["protocol"].get<long long>() > protocol_version )
        {
            sendError( id, "protocol_unsupported", "server supports protocol " + std::to_string( protocol_version ) );
            exit_code = 2;
            requestStop();
            return;
        }

        said_hello = true;

        json reply;
        reply["ev"]       = "hello";
        reply["protocol"] = protocol_version;
        reply["server"]   = server_name;
        reply["pid"]      = processId();
        reply["cwd"]      = currentDirectoryUtf8();
        reply["features"] = serverInfo( server_name )["features"];
        addReplyTo( reply, id );
        send( reply );
        return;
    }

    if ( not said_hello )
    {
        sendError( id, "bad_request", "hello must be the first request" );
        return;
    }

    // commands available in every state ------------------------------------------------------------------------
    if ( cmd == "ping" )
    {
        json reply;
        reply["ev"] = "pong";
        addReplyTo( reply, id );
        send( reply );
    }
    else if ( cmd == "interrupt" )
    {
        // Reports whether the interpreter was busy, but cannot actually stop it yet: that needs a cooperative
        // interrupt flag inside the core analysis loops (GUI_Implementation_Note.md, section 4.4 / phase 3, C4).
        json reply;
        reply["ev"]       = "ack";
        reply["of"]       = "interrupt";
        reply["was_busy"] = busy.load();
        addReplyTo( reply, id );
        send( reply );
    }
    else if ( cmd == "answer" )
    {
        long long pending_id = 0;
        bool have_pending = false;
        {
            std::lock_guard<std::mutex> lock( ask_mutex );
            have_pending = ask_pending;
            pending_id   = ask_id;
        }
        if ( not have_pending )
        {
            sendError( id, "bad_request", "no ask is pending" );
            return;
        }
        if ( not request.contains( "re" ) or not request["re"].is_number_integer()
             or request["re"].get<long long>() != pending_id )
        {
            sendError( id, "bad_request", "answer's 're' does not match the pending ask" );
            return;
        }
        if ( not request.contains( "value" ) or not request["value"].is_boolean() )
        {
            sendError( id, "bad_request", "answer needs a boolean field 'value'" );
            return;
        }

        bool stale = false;
        {
            std::lock_guard<std::mutex> lock( ask_mutex );
            // Re-checked under the lock: requestAsk() could have given up (requestStop(), see its wait predicate)
            // between the read above and here, in which case this answer is now stale and must not resurrect it.
            if ( not ask_pending or ask_id != pending_id )
            {
                stale = true;
            }
            else
            {
                ask_answer_value = request["value"].get<bool>();
                ask_answered     = true;
            }
        }
        if ( stale )
        {
            sendError( id, "bad_request", "no ask is pending" );
            return;
        }
        ask_cv.notify_all();
    }
    else if ( cmd == "shutdown" )
    {
        json reply;
        reply["ev"]     = "bye";
        reply["reason"] = "shutdown";
        addReplyTo( reply, id );
        send( reply );

        requestStop();
        startWatchdog();
    }
    // submit --------------------------------------------------------------------------------------------------
    else if ( cmd == "submit" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        if ( not request.contains( "text" ) or not request["text"].is_string() )
        {
            sendError( id, "bad_request", "submit needs a string field 'text'" );
            return;
        }

        busy = true;   // set before releasing control to the queue, so a second submit sent immediately after
                       // (before the main thread gets to it) is rejected here rather than queued twice.
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            command_queue.push( PendingCommand{ id, "submit", request["text"].get<std::string>(), std::string() } );
        }
        queue_cv.notify_one();
    }
    // snapshot ------------------------------------------------------------------------------------------------
    else if ( cmd == "snapshot" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        const std::string what = request.contains( "what" ) and request["what"].is_string()
                                      ? request["what"].get<std::string>() : "variables";
        if ( what != "variables" and what != "functions" and what != "all" )
        {
            sendError( id, "bad_request", "snapshot what='" + what + "' is not a recognised kind" );
            return;
        }

        busy = true;   // see the matching comment on submit, above
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            command_queue.push( PendingCommand{ id, "snapshot", std::string(), what } );
        }
        queue_cv.notify_one();
    }
    // inspect -------------------------------------------------------------------------------------------------
    else if ( cmd == "inspect" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        if ( not request.contains( "name" ) or not request["name"].is_string() )
        {
            sendError( id, "bad_request", "inspect needs a string field 'name'" );
            return;
        }

        busy = true;
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            command_queue.push( PendingCommand{ id, "inspect", std::string(), request["name"].get<std::string>() } );
        }
        queue_cv.notify_one();
    }
    // complete ------------------------------------------------------------------------------------------------
    else if ( cmd == "complete" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        if ( not request.contains( "buffer" ) or not request["buffer"].is_string() )
        {
            sendError( id, "bad_request", "complete needs a string field 'buffer'" );
            return;
        }
        const std::string buffer = request["buffer"].get<std::string>();
        std::size_t cursor = buffer.size();   // default: end of the buffer (section 6.4)
        if ( request.contains( "cursor" ) )
        {
            if ( not request["cursor"].is_number_integer() or request["cursor"].get<long long>() < 0 )
            {
                sendError( id, "bad_request", "complete's 'cursor' must be a non-negative integer" );
                return;
            }
            cursor = static_cast<std::size_t>( request["cursor"].get<long long>() );
        }

        busy = true;
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            command_queue.push( PendingCommand{ id, "complete", buffer, std::string(), cursor } );
        }
        queue_cv.notify_one();
    }
    // help ----------------------------------------------------------------------------------------------------
    else if ( cmd == "help" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        if ( not request.contains( "topic" ) or not request["topic"].is_string() )
        {
            sendError( id, "bad_request", "help needs a string field 'topic'" );
            return;
        }

        busy = true;
        {
            std::lock_guard<std::mutex> lock( queue_mutex );
            command_queue.push( PendingCommand{ id, "help", std::string(), request["topic"].get<std::string>() } );
        }
        queue_cv.notify_one();
    }
    // commands that exist in protocol 1 but are not implemented yet -------------------------------------------
    else if ( cmd == "set" )
    {
        if ( busy.load() )
        {
            sendError( id, "busy", "a command is running" );
            return;
        }
        sendError( id, "not_implemented", "'" + cmd + "' is not implemented by this backend yet" );
    }
    else
    {
        sendError( id, "unknown_command", "unknown command '" + cmd + "'" );
    }
}


/**
 * Runs one submit on the interpreter thread: joins it with any continuation left over from an earlier
 * "incomplete" submit, calls the real parser, streams whatever it prints as `output` events (via OutputCapture,
 * which is already installed by the time run() is reachable -- see RevClient::startServer), then reports `done`
 * and a `variables` event.
 */
void RevServer::processSubmit(const json& id, const std::string& text)
{
    current_submit_id = id;

    std::string command = continuation_buffer.empty() ? text : continuation_buffer + "\n" + text;
    continuation_buffer.clear();

    const auto start = std::chrono::steady_clock::now();
    int rc = 2;
    try
    {
        rc = Parser::getParser().processCommand( command, Workspace::userWorkspacePtr() );
    }
    catch ( RbException& e )
    {
        // Everything inside processCommand's own call graph is documented to catch RbException itself; this is
        // cheap insurance against a code path that does not, so a bug in the interpreter cannot silently take the
        // whole backend down with it (GUI_Implementation_Note.md, risk R5).
        std::ostringstream msg;
        e.print( msg );
        emitOutput( msg.str() + "\n", OutputCapture::Stream::Stderr );
    }
    catch ( std::exception& e )
    {
        emitOutput( std::string( "Internal error: " ) + e.what() + "\n", OutputCapture::Stream::Stderr );
    }
    catch ( ... )
    {
        emitOutput( "Internal error: unknown exception\n", OutputCapture::Stream::Stderr );
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start ).count();

    std::string status;
    if ( rc == 1 )
    {
        continuation_buffer = command;   // processCommand left the accumulated partial text in `command`
        status = "incomplete";
    }
    else if ( rc == 0 )
    {
        status = "ok";
    }
    else
    {
        status = "error";
    }

    json done;
    done["ev"]         = "done";
    done["status"]     = status;
    done["rc"]         = rc;
    done["cwd"]        = currentDirectoryUtf8();
    done["elapsed_ms"] = static_cast<long long>( elapsed_ms );
    addReplyTo( done, id );
    send( done );

    current_submit_id = json( nullptr );

    sendVariablesEvent();   // the documented done -> variables sequencing (section 6.4); no `re`, this is a broadcast

    busy = false;
}


/**
 * Runs one on-demand `snapshot` on the interpreter thread. `what` is "variables", "functions" or "all"; handleLine()
 * already rejected anything else as bad_request.
 */
void RevServer::processSnapshot(const json& id, const std::string& what)
{
    if ( what == "functions" or what == "all" )
    {
        sendFunctionsEvent( id );
    }
    if ( what == "variables" or what == "all" )
    {
        sendVariablesEvent( id );
    }
    busy = false;
}


void RevServer::sendVariablesEvent(const json& reply_to)
{
    json variables;
    variables["ev"]   = "variables";
    variables["rev"]  = ++variables_revision;
    variables["cwd"]  = currentDirectoryUtf8();
    variables["rows"] = WorkspaceSnapshot::variableRows( *Workspace::userWorkspacePtr() );
    addReplyTo( variables, reply_to );
    send( variables );
}


void RevServer::sendFunctionsEvent(const json& reply_to)
{
    json functions;
    functions["ev"]   = "functions";
    functions["rows"] = WorkspaceSnapshot::functionRows( Workspace::userWorkspace().getFunctionTable() );
    addReplyTo( functions, reply_to );
    send( functions );
}


/**
 * Runs `structure(name, verbose=TRUE)` (Func_structure.cpp) on the interpreter thread, with its one RBOUT call
 * captured by emitOutput() into `text` instead of streamed, then sends it as one `inspection` event. An error
 * (for example `name` not existing) is whatever the parser/RBOUT already reports for it -- the same thing that
 * would appear in a terminal -- not a distinct error path, matching the `inspection` event's schema (6.4), which
 * has no status field of its own.
 */
void RevServer::processInspect(const json& id, const std::string& name)
{
    std::string text;
    inspect_capture   = &text;
    inspect_truncated = false;

    std::string command = "structure(" + name + ", verbose=TRUE)";
    try
    {
        Parser::getParser().processCommand( command, Workspace::userWorkspacePtr() );
    }
    catch ( RbException& e )
    {
        std::ostringstream msg;
        e.print( msg );
        text += msg.str();
    }
    catch ( ... )
    {
        text += "<error inspecting '" + name + "'>";
    }

    inspect_capture = nullptr;

    json event;
    event["ev"]        = "inspection";
    event["name"]      = name;
    event["text"]      = text;
    event["truncated"] = inspect_truncated;
    addReplyTo( event, id );
    send( event );

    busy = false;
}


/**
 * Builds a `completions` event from Completion.h's complete() (shared with the terminal client's tab completion,
 * GUI_Implementation_Note.md task C1) against the user workspace.
 */
void RevServer::processComplete(const json& id, const std::string& buffer, std::size_t cursor)
{
    std::size_t replace_from = 0;
    std::vector<CompletionItem> items = RevLanguage::complete( buffer, cursor, Workspace::userWorkspacePtr(), replace_from );

    json rows = json::array();
    for ( auto& item : items )
    {
        json row;
        row["text"] = item.text;
        row["kind"] = item.kind;
        rows.push_back( std::move( row ) );
    }

    json event;
    event["ev"]           = "completions";
    event["replace_from"] = static_cast<long long>( replace_from );
    event["items"]        = std::move( rows );
    addReplyTo( event, id );
    send( event );

    busy = false;
}


/**
 * Builds a `help` event for `topic` from RbHelpSystem/RbHelpDatabase/HelpRenderer directly (Appendix B of
 * GUI_Implementation_Note.md), the same APIs Func_help.cpp calls -- not by running `help(topic)` through the
 * parser and capturing its RBOUT output the way processInspect() does for structure(), since that would leave no
 * clean way to tell "found" apart from "not found" other than pattern-matching Func_help's message text. An empty
 * topic mirrors Func_help.cpp's own default (help() with no argument): the generic resource listing, not a
 * not-found reply.
 */
void RevServer::processHelp(const json& id, const std::string& topic)
{
    std::string text;
    bool found = false;

    if ( topic.empty() )
    {
        text =
            "\nRevBayes help resources:\n\n"
            "  - Find specific help for topics with the `?` and `help` commands.\n"
            "    > # examples\n"
            "    > ?dnNormal\n"
            "    > help(\"mcmc\")\n\n"
            "  - List functions and datatypes with the `ls()` command.\n"
            "    > # examples\n"
            "    > ls(all=true)\n"
            "    > ls(all=true, filter=\"distribution\")\n\n"
            "  - Join the RevBayes users forum.\n"
            "        https://groups.google.com/d/forum/revbayes-users\n\n"
            "  - Visit the RevBayes website.\n"
            "        https://revbayes.com/tutorials\n";
        found = true;
    }
    else
    {
        RevBayesCore::RbHelpSystem& hs = RevBayesCore::RbHelpSystem::getHelpSystem();
        RevBayesCore::RbHelpDatabase& hd = RevBayesCore::RbHelpDatabase::getHelpDatabase();
        const std::string& help_title = hd.getHelpString( topic, "name" );
        const std::size_t width = RbSettings::userSettings().getLineWidth() - RevBayesCore::RbUtils::PAD.size();

        if ( hs.isHelpAvailableForQuery( topic ) )
        {
            RevBayesCore::HelpRenderer renderer;
            text  = renderer.renderHelp( hs.getHelp( topic ), width );
            found = true;
        }
        else if ( not help_title.empty() )
        {
            RevBayesCore::HelpRenderer renderer;
            text  = renderer.renderHelp( hd, topic, width );
            found = true;
        }
    }

    json event;
    event["ev"]    = "help";
    event["topic"] = topic;
    event["found"] = found;
    event["text"]  = text;
    addReplyTo( event, id );
    send( event );

    busy = false;
}


void RevServer::emitOutput(const std::string& text, OutputCapture::Stream which)
{
    if ( text.empty() )
    {
        return;
    }

    if ( inspect_capture != nullptr )
    {
        // processInspect() is running: accumulate into its buffer instead of streaming, bounded to
        // max_inspect_bytes (section 6.3). structure() (Func_structure.cpp) builds its whole text internally,
        // in one std::ostringstream, before the one RBOUT call that reaches here, so this bounds only the wire
        // size of `inspection.text`, not structure()'s own compute cost -- unlike WorkspaceSnapshot's bounded
        // summaries, which stop the underlying print loop early (see boundedSummary()'s comment).
        if ( inspect_capture->size() < max_inspect_bytes )
        {
            const std::size_t room = max_inspect_bytes - inspect_capture->size();
            if ( text.size() <= room )
            {
                inspect_capture->append( text );
            }
            else
            {
                inspect_capture->append( text, 0, room );
                inspect_truncated = true;
            }
        }
        else
        {
            inspect_truncated = true;
        }
        return;
    }

    json event;
    event["ev"]     = "output";
    event["stream"] = streamName( which );
    event["text"]   = text;
    if ( not current_submit_id.is_null() )
    {
        event["id"] = current_submit_id;
    }
    send( event );
}


void RevServer::reportQuit(const std::string& reason)
{
    json quit;
    quit["ev"]     = "quit";
    quit["reason"] = reason;
    send( quit );

    json bye;
    bye["ev"]     = "bye";
    bye["reason"] = "quit";
    send( bye );
}


/**
 * Sends an `ask` event, then blocks the calling thread (the interpreter thread: this is only ever reached from the
 * UserInterface::ask() hook, itself only ever called from within processSubmit()'s call into the parser) until
 * either handleLine()'s `answer` branch delivers a value, or requestStop() fires -- EOF, `shutdown`, a dead peer,
 * or an unsupported protocol version. The latter case must not hang the interpreter thread forever (D6/D7,
 * GUI_Implementation_Note.md section 6.3): declining (false) is the safe default, matching what a user closing the
 * dialog without answering would mean.
 */
bool RevServer::requestAsk(const std::string& question)
{
    long long id;
    {
        std::lock_guard<std::mutex> lock( ask_mutex );
        id           = ++next_ask_id;
        ask_id       = id;
        ask_pending  = true;
        ask_answered = false;
    }

    json event;
    event["ev"]       = "ask";
    event["id"]       = id;
    event["question"] = question;
    send( event );

    bool answer = false;
    {
        std::unique_lock<std::mutex> lock( ask_mutex );
        ask_cv.wait( lock, [this] { return ask_answered or stop_requested.load(); } );
        if ( ask_answered )
        {
            answer = ask_answer_value;
        }
        ask_pending = false;
    }
    return answer;
}


void RevServer::send(const json& message)
{
    // Invalid UTF-8 (in output or file names) becomes U+FFFD instead of throwing.
    std::string text = message.dump( -1, ' ', false, json::error_handler_t::replace );
    text.push_back( '\n' );

    std::lock_guard<std::mutex> lock( write_mutex );
    if ( not writeAll( protocol_fd, text ) )
    {
        // The front end is gone; there is nobody left to talk to.
        requestStop();
    }
}


void RevServer::sendError(const json& request_id, const std::string& code, const std::string& message)
{
    json event;
    event["ev"]      = "error";
    event["code"]    = code;
    event["message"] = message;
    addReplyTo( event, request_id );
    send( event );
}


void RevServer::addReplyTo(json& event, const json& request_id)
{
    if ( not request_id.is_null() )
    {
        event["re"] = request_id;
    }
}


std::string RevServer::currentDirectoryUtf8(void)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::current_path( ec );
    return ec ? std::string() : toUtf8( p );
}
