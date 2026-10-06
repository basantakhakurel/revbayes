#ifndef REVCLIENT_H
#define	REVCLIENT_H

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <filesystem>


//struct tabCompletionInfo{
//        int startPos;
//        unsigned int specialMatchType;
//        std::vector<std::string> completions;
//        std::vector<std::string> matchingCompletions;
//        std::string compMatch;
//    };

namespace RevClient
{
    int   interpret(const std::string& command);

    void  execute_file(const std::filesystem::path& filename, bool echo, bool continue_on_error);
    void  shutdown();
    void  startInterpreter();
    void  startJupyterInterpreter();

    //! `rb --server`: backend for graphical front ends; returns the exit code. `cmd_line_options` (`-o key=value`,
    //! repeatable) and `seed` (`-s`) are applied here, AFTER the protocol channel is taken over, rather than by
    //! main() before dispatching here as every other mode does it: applying them is what first constructs
    //! RbSettings::userSettings(), which reads ~/.RevBayes.ini and can print straight to std::cout for a key it
    //! does not recognise (RbSettings.cpp) -- on fd 1, before the redirect, that would corrupt the protocol's
    //! first bytes. Found empirically (GUI_Implementation_Note.md task S11).
    int   startServer( const std::vector<std::string>& cmd_line_options, std::optional<std::uint64_t> seed );
}
    

//    std::set<std::string>       getDefaultCompletions();
//    std::vector<std::string>    getFileList(const std::string &path);
//        tabCompletionInfo           getTabCompleteInfo(const char *buf);
//        void                        setTabCompletionInfo(const char *buf);
    

#endif	/* REVCLIENT_H */

