#include "RevServer.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <iostream>
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

}


RevServer::RevServer(const std::string& n) :
    server_name( n ),
    protocol_fd( -1 ),
    said_hello( false ),
    stop_requested( false ),
    exit_code( 0 )
{

}


RevServer::~RevServer(void)
{
    // The protocol descriptor is deliberately left open: the process is about to exit, and closing it early could
    // truncate an event that is still being written by another thread once the server gains one (phase 1).
}


/**
 * Take over the process's standard channels.
 *
 * The protocol travels on a private duplicate of the ORIGINAL stdout. File descriptor 1 is then redirected to
 * stderr, so anything that still writes to stdout behind our back (printf, third-party libraries, std::cout) can
 * never corrupt the protocol; it shows up on stderr instead. Design verified in a Linux probe, see section 1 of
 * GUI_Implementation_Note.md.
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
 * Serve requests until `shutdown`, end of input, or a protocol error.
 * Returns the process exit code: 0 for a normal end, 2 for a protocol error, 1 if the channels were not taken over.
 */
int RevServer::run(void)
{
    if ( protocol_fd < 0 )
    {
        return 1;
    }

    std::string line;
    while ( not stop_requested and std::getline( std::cin, line ) )
    {
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

    return exit_code;
}


nlohmann::json RevServer::serverInfo(const std::string& server_name)
{
    json info;
    info["protocol"] = protocol_version;
    info["version"]  = server_name;
    info["features"] = json::array();     // phase 1 adds "interrupt", "complete", "help", "inspect", "functions"
    return info;
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
            stop_requested = true;
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
        // Nothing can be running in the phase 0 stub.
        json reply;
        reply["ev"]       = "ack";
        reply["of"]       = "interrupt";
        reply["was_busy"] = false;
        addReplyTo( reply, id );
        send( reply );
    }
    else if ( cmd == "shutdown" )
    {
        json reply;
        reply["ev"]     = "bye";
        reply["reason"] = "shutdown";
        addReplyTo( reply, id );
        send( reply );
        stop_requested = true;
    }
    // commands that exist in protocol 1 but are not implemented by the phase 0 stub ------------------------------
    else if ( cmd == "submit" or cmd == "snapshot" or cmd == "inspect" or cmd == "complete" or cmd == "help"
              or cmd == "set" or cmd == "answer" )
    {
        sendError( id, "not_implemented", "'" + cmd + "' is not implemented by this backend yet (phase 0 stub)" );
    }
    else
    {
        sendError( id, "unknown_command", "unknown command '" + cmd + "'" );
    }
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
        stop_requested = true;
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
