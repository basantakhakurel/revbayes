#include "Completion.h"
#include "FunctionTable.h"
#include "OutputCapture.h"
#include "RevClient.h"
#include "RevLanguageMain.h"
#include "RevServer.h"
#include "RbVersion.h"
#include "RlCommandLineOutputStream.h"
#include "RlFunction.h"
#include "RlUserInterface.h"
#include "Parser.h"
#include "Workspace.h"
#include "RbSettings.h"
#include "ArgumentRule.h"
#include "ArgumentRules.h"
#include "Environment.h"
#include "RandomNumberFactory.h"
#include "RandomNumberGenerator.h"
#include "RevPtr.h"
#include "StringUtilities.h"
#include "TypeSpec.h"
#include "boost/algorithm/string/trim.hpp"

#ifdef RB_MPI
#include <mpi.h>
#endif

extern "C" {
#include "linenoise.h"
}

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "RbException.h"
//#define ctrl(C) ((C) - '@')

const char* default_prompt = (const char*) "> ";
const char* incomplete_prompt = (const char*) "+ ";
const char* prompt = default_prompt;

using namespace RevLanguage;

namespace fs = std::filesystem;

/**
 * linenoise tab-completion callback: a thin adapter over RevLanguage::complete() (Completion.h), which holds the
 * actual logic so it can be shared with `rb --server`'s `complete` command (GUI_Implementation_Note.md, task/
 * defect C1). linenoise always passes the buffer already truncated at the cursor, so the completed text and the
 * cursor are the same thing here.
 */
void completeOnTab(const char *buf, linenoiseCompletions *lc)
{
    std::string buffer( buf );
    std::size_t replace_from = 0;
    std::vector<RevLanguage::CompletionItem> items =
        RevLanguage::complete( buffer, buffer.size(), RevLanguage::Workspace::userWorkspacePtr(), replace_from );

    for ( auto& item : items )
    {
        linenoiseAddCompletion( lc, item.text.c_str() );
    }
}

/**
 * Print out function signatures
 * @param buf The buffer into which we print
 * @param len The length
 * @param c The character
 * @return  Returns the status
 */
int printFunctionParameters(const char *buf, size_t len, char c)
{
    std::string cmd = buf;
    RevLanguage::ParserInfo pi = RevLanguage::Parser::getParser().checkCommand(cmd, RevLanguage::Workspace::userWorkspacePtr());
    if ( Workspace::globalWorkspace().existsFunction(pi.function_name) )
    {

        std::vector<Function *> functions = Workspace::globalWorkspace().getFunctionTable().findFunctions(pi.function_name);
        
        for (std::vector<Function *>::iterator it = functions.begin(); it != functions.end(); ++it)
        {
            Function *f = *it;
            std::cout << "\n\r" + f->getReturnType().getType() + " " + pi.function_name + " (";

            const RevLanguage::ArgumentRules& argRules = (*it)->getArgumentRules();
            for (size_t i = 0; i < argRules.size(); i++)
            {
                std::cout << argRules[i].getArgumentLabel();
                if (i < argRules.size() - 1) {
                    std::cout << ", ";
                }
            }

        }
        
        
        std::cout << ")\n\r";
        linenoiceSetCursorPos(0);
        std::cout << prompt << buf;
        std::cout.flush();
    }
    return 0;
}

namespace RevClient
{

int interpret(const std::string& command)
{
    size_t bsz = command.size();
#ifdef RB_MPI
    MPI_Bcast(&bsz, 1, MPI_INT, 0, MPI_COMM_WORLD);
#endif

    char * buffer = new char[bsz+1];
    buffer[bsz] = 0;
    for (int i = 0; i < bsz; i++)
        buffer[i] = command[i];
#ifdef RB_MPI
    MPI_Bcast(buffer, (int)bsz, MPI_CHAR, 0, MPI_COMM_WORLD);
#endif

    std::string tmp = std::string( buffer );

    return RevLanguage::Parser::getParser().processCommand(tmp, RevLanguage::Workspace::userWorkspacePtr());
}

void shutdown()
{
    Workspace::userWorkspace().clear();
    Workspace::globalWorkspace().clear();

#ifdef RB_MPI
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
#endif
}


void execute_file(const fs::path& filename, bool echo, bool continue_on_error)
{
    int rank = 0;
#ifdef RB_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif

    auto& settings = RbSettings::userSettings();
    std::stringstream inFile = RevBayesCore::readFileAsStringStream(filename);

    // Command-processing loop
    std::string commandLine;
    int lineNumber = 0;
    int result = 0;     // result from processing of last command
    while ( inFile.good() )
    {
        // Read a line
        std::string line;
        RevBayesCore::safeGetline(inFile, line);
        // Don't execute an empty line at the end of the file
        if (line.empty() and not inFile.good()) break;

        lineNumber++;
        if ( echo and rank == 0 )
        {
            if ( result == 1 )
            {
                std::cout << incomplete_prompt << line << std:: endl;
            }
            else
            {
                std::cout << default_prompt << line << std::endl;
            }
        }
        
        // If previous result was 1 (append to command), we do this
        if ( result == 1 )
        {
            commandLine += line;
        }
        else
        {
            commandLine = line;
        }

        // Process the line and record result
        result = Parser::getParser().processCommand( commandLine, Workspace::userWorkspacePtr() );
        if ( result == 2 )
        {
            if (not continue_on_error)
                throw RbException() << "Problem processing line " << lineNumber << " in file " << filename;
            else
            {
                std::ostringstream err;
                err<<"Error:\tProblem processing line " << lineNumber << " in file " << filename;
                RBOUT(err.str());
            }
        }
    }
}

/**
 * Main application loop.
 * 
 */
void startInterpreter()
{
    auto& settings = RbSettings::userSettings();

    // If we aren't using MPI, this will be zero.
    // If we are using MPI, it will be zero for the first process.
    int pid = 0;
#ifdef RB_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &pid);
#endif
    
    /* Set tab completion callback */
    linenoiseSetCompletionCallback( completeOnTab );

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    if ( pid == 0 )
    {
        linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
    }
    
    /* callback for printing function signatures on opening bracket*/

    // Currently disabled because
    // (i) it doesn't seem to do anything at the moment: pi.function_name is never set.
    // (ii) it makes parsing go crazy if the '(' is inside a string.

    // linenoiseSetCharacterCallback(printFunctionParameters, '(');

    int result = 0;
    std::string commandLine;
    std::string cmd;

    while (true)
    {
        
        char *line = NULL;
        
        // set prompt
        if (result == 0 || result == 2)
        {
            prompt = default_prompt;
        }
        else //if (result == 1)
        {
            prompt = incomplete_prompt;
        }


        // process command line
        if ( pid == 0 )
        {
            line = linenoise(prompt);
            
            if (!line) 
            {
                // [JS, 2015-11-03]
                // Null input, e.g. if the user entered CTRL-D or CTRL-C.
                // If not handled here, segmentation fault results. Not a dealbreaker, but annoying.
                shutdown();

                exit(0);
            }
            else
            {
                linenoiseHistoryAdd(line);              /* Add to the history. */
                linenoiseHistorySave("history.txt");    /* Save the history on disk. */
           
                cmd = line;
                boost::trim(cmd);

                if (cmd == "clr" || cmd == "clear")
                {
                    linenoiseClearScreen();
                }
                else
                {
                    // interpret Rev statement
                    if (result == 0 || result == 2)
                    {
                        commandLine = cmd;
                    }
                    else //if (result == 1)
                    {
                        commandLine += "\n " + cmd;
                    }
                }
            }
        }
        
        result = interpret(commandLine);

        if (result == 2)
        {
            commandLine.clear();
        }

/* The typed string is returned as a malloc() allocated string by
         * linenoise, so the user needs to free() it. */
        
        if ( pid == 0 )
        {
            free(line);
        }
        
    }
    
    
}

void startJupyterInterpreter()
{
    // If we aren't using MPI, this will be zero.
    // If we are using MPI, it will be zero for the first process.
    int pid = 0;
#ifdef RB_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &pid);
#endif
    
    /* Declare things we need */
    int result = 0;
    std::string commandLine = "";

    for (;;)
    {
        /* Print prompt based on state after previous iteration */
        if ( pid == 0 )
        {
            if (result == 0 || result == 2)
            {
                std::cout << default_prompt ;
            }
            else
            {
                std::cout << incomplete_prompt;
            }

            /* Get the line */
            std::string line = "";
            if (not std::getline(std::cin, line)) return;

            if (result == 0 || result == 2)
            {
                commandLine = line;
            }
            else if (result == 1)
            {
                commandLine += ";" + line;
            }
        }

        result = RevClient::interpret(commandLine);
    }
}


/**
 * Backend for graphical front ends (`rb --server`).
 *
 * Order matters:
 *   1. The protocol channel is taken over BEFORE anything else runs, so nothing printed during start-up (for
 *      example a module loading error) can ever corrupt the protocol.
 *   2. A plain CommandLineOutputStream is used ONLY for the brief start-up window while the interpreter
 *      environment is being built. takeOverChannels() has already redirected fd 1 to stderr, so this output is
 *      not lost, just not sent to the front end as protocol events -- a deliberate simplification, since nothing
 *      the front end could act on happens before this returns, and no `hello` has been answered yet at this point
 *      either.
 *   3. Once the environment is ready, OutputCapture takes over RBOUT/std::cout/std::cerr for the rest of the
 *      process's life, and the quit hook (Parser.h's quitRequestHandler) is installed so a Rev quit() call reports
 *      itself over the protocol (quit, then bye) before the existing RevClient::shutdown()+exit(0) path runs.
 */
int startServer( const std::vector<std::string>& cmd_line_options, std::optional<std::uint64_t> seed )
{
    RevLanguage::RevServer server( "RevBayes " + RbVersion().getVersion() );
    if ( not server.takeOverChannels() )
    {
        return 1;
    }

    // See RevClient.h's comment on this function: RbSettings::userSettings()'s first call (right here) reads
    // ~/.RevBayes.ini and can print a warning for an unrecognised key straight to std::cout; applying a bad -o
    // value does too. Doing this AFTER takeOverChannels() means fd 1 is already redirected to stderr by then, so
    // that warning cannot corrupt the protocol the way it would have landing on the still-unredirected fd 1 if
    // main() had done this before dispatching here, as it does for every other mode.
    RbSettings& settings = RbSettings::userSettings();
    for ( auto& option : cmd_line_options )
    {
        std::vector<std::string> tokens;
        StringUtilities::stringSplit( option, "=", tokens );
        if ( tokens.size() != 2 )
        {
            std::cerr << "Option '" << option << "' must have the form key=value\n";
            return 1;
        }
        settings.setOption( tokens[0], tokens[1], false );
    }
    if ( seed )
    {
        RevBayesCore::RandomNumberGenerator* rng = RevBayesCore::GLOBAL_RNG;
        rng->setSeed( seed.value() );
    }

    RevLanguage::UserInterface::userInterface().setOutputStream( new CommandLineOutputStream() );

    RevLanguageMain rl( /* continue_on_error */ false, /* echo */ false, /* quiet */ true );
    int result = rl.startRevLanguageEnvironment( {}, {}, {} );
    if ( result != 0 )
    {
        return result;
    }

    RevLanguage::OutputCapture capture( [&server](const std::string& text, RevLanguage::OutputCapture::Stream which)
    {
        server.emitOutput( text, which );
    } );

    // quitRequestHandler (declared in Parser.h) is at global scope, like the other flex/parser externs it sits
    // beside there -- not inside namespace RevLanguage.
    quitRequestHandler = [&server]
    {
        server.reportQuit( "quit()" );

        // Terminate here, rather than returning and letting Parser.cpp's own RevClient::shutdown()+exit(0) run:
        // exit() destroys static-duration objects, including std::cin, and that destructor can deadlock against
        // the reader thread if it is still blocked inside std::getline(std::cin, ...) at the time (both contend
        // for the same internal iostream lock; the single-threaded terminal client this exit() path was written
        // for never has a second thread blocked on cin, so it never hit this). _Exit skips all of that -- no C++
        // destructors, no atexit handlers, so RevClient::shutdown()'s workspace clearing is skipped too, which is
        // harmless: the process is terminating either way, and --server already refuses MPI builds, so there is no
        // MPI_Finalize() to lose. Verified empirically: exit(0) here reproduced the deadlock 100% of the time while
        // a client kept stdin open; _Exit(0) does not.
        std::_Exit( 0 );
    };

    // The ask hook (RlUserInterface.h's setAskHandler) is a member of UserInterface itself, unlike
    // quitRequestHandler above: it must return a value to its caller, so "fire an event and return" is not enough
    // here -- the handler blocks the calling (interpreter) thread inside RevServer::requestAsk() until an `answer`
    // arrives (or the server is shutting down, in which case it returns false; see requestAsk()'s comment).
    RevLanguage::UserInterface::userInterface().setAskHandler( [&server](const std::string& question)
    {
        return server.requestAsk( question );
    } );

    return server.run();
}

}
