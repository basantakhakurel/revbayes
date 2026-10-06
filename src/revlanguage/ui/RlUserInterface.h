/**
 * @file
 * This file contains the declaration of UserInterface, which is
 * the base class for different user interfaces.
 *
 * @brief Declaration of UserInterface
 *
 * (c) Copyright 2009-
 * @date Last modified: $Date$
 * @author The RevBayes Development Core Team
 * @license GPL version 3
 * @extends Frame
 * @package parser
 * @version 1.0
 * @since version 1.0 2009-09-02
 *
 * $Id$
 */

#ifndef UserInterface_H
#define UserInterface_H

#include "RlUserInterfaceOutputStream.h"

#include <functional>
#include <iostream>
#include <sstream>

namespace RevLanguage {

#define RBOUT(m) RevLanguage::UserInterface::userInterface().output((m))

class UserInterface {

    public:
        bool                        ask(std::string msg);                                       //!< Ask user a question
        bool                        initialize(void) { return true; }                           //!< Initialize interface
        void                        output(std::string msg);                                    //!< Display message from string
        void                        output(std::string msg, const bool hasPadding);             //!< Display message from string with control of padding
        void                        output(std::ostringstream msg);                             //!< Display message from stringstream
        void                        setOutputStream(UserInterfaceOutputStream *o);              //!< Set the output stream for the current interface
        UserInterfaceOutputStream*  getOutputStream(void) const { return output_stream; }        //!< Get the current output stream (not owned; used by rb --server to save/restore it, see OutputCapture)

        //! Installed by `rb --server` (RevServer.cpp, via RevClient::startServer) so a yes/no question can be
        //! relayed over the protocol instead of ask() reading std::cin directly, which in server mode is the same
        //! stream the reader thread is concurrently reading requests from -- a real hazard (two threads reading
        //! one stream), not just an inconvenience: it is exactly the class of bug that turned out to be a genuine
        //! deadlock for quit()'s exit() call, see Parser.h's quitRequestHandler and OutputCapture.h's class comment
        //! for that precedent. Default (nullptr) preserves the original std::cin-based behaviour exactly. Unlike
        //! quitRequestHandler, this hook must return a value (ask()'s caller uses it immediately), so it is a
        //! member here rather than a free function: the handler is expected to BLOCK the calling (interpreter)
        //! thread until an answer is available, not just fire an event and return immediately.
        void                        setAskHandler(std::function<bool(const std::string&)> handler) { ask_handler = std::move( handler ); }

        static UserInterface&       userInterface(void)                                         //!< Get the user interface
		                               {
		                               static UserInterface theInterface = UserInterface();
		                               return theInterface;
		                               }

    protected:
                                    UserInterface(void);                                        //!< Prevent construction
                                    UserInterface(const UserInterface& x);                      //!< Prevent copy construction
        virtual                    ~UserInterface(void) {}                                      //!< Destructor
        UserInterface&              operator=(const UserInterface& w) { return (*this); }       //!< Prevent assignment

        int                         process_id;
        UserInterfaceOutputStream*  output_stream;
        std::function<bool(const std::string&)> ask_handler;                                    //!< See setAskHandler(); empty (falsy) by default
};
    
}

#endif

