#include "Completion.h"

#include "ArgumentRule.h"
#include "ArgumentRules.h"
#include "Environment.h"
#include "FunctionTable.h"
#include "Parser.h"
#include "RbFileManager.h"
#include "RlFunction.h"
#include "Workspace.h"

#include <algorithm>
#include <map>

using namespace RevLanguage;


namespace {

    /** Files in `dir` (a possibly-partial path typed after an opening quote), for file-argument completion. */
    std::vector<CompletionItem> fileCandidates(const RevBayesCore::path& dir)
    {
        std::vector<RevBayesCore::path> filenames;
        RevBayesCore::setStringWithNamesOfFilesInDirectory( RevBayesCore::current_path() / dir, filenames, false );

        std::vector<CompletionItem> items;
        for ( auto& filename : filenames )
        {
            items.push_back( { filename.string(), "file" } );
        }
        return items;
    }

    /** User and global functions, variables and types, one entry per unique name (functions win ties, then
     *  variables, then types -- a name can appear in more than one table, e.g. a type's constructor function
     *  shares the type's name; the original terminal-client logic folded all three into one std::set<std::string>
     *  and so never surfaced that overlap as a visible duplicate, which this preserves). Alphabetical, like the
     *  std::set the original terminal-client logic (getDefaultCompletions) built this from. Hardcoded to the user
     *  and global workspaces, as the original was -- not parameterized over an arbitrary Environment, since
     *  getTypeTable() is only declared on Workspace, not its Environment base. */
    std::vector<CompletionItem> defaultCandidates(void)
    {
        std::map<std::string, std::string> by_name;   // name -> kind; std::map keeps it sorted, like the original std::set

        const FunctionTable& user_functions = Workspace::userWorkspace().getFunctionTable();
        for ( auto it = user_functions.begin(); it != user_functions.end(); ++it )
        {
            by_name.emplace( it->first, "function" );
        }
        std::vector<std::string> function_names;
        user_functions.getFunctionNames( function_names );
        for ( auto& name : function_names )
        {
            by_name.emplace( name, "function" );
        }

        for ( auto& entry : Workspace::userWorkspace().getVariableTable() )
        {
            by_name.emplace( entry.first, "variable" );
        }
        for ( auto& entry : Workspace::globalWorkspace().getVariableTable() )
        {
            by_name.emplace( entry.first, "variable" );
        }

        for ( auto& entry : Workspace::userWorkspace().getTypeTable() )
        {
            by_name.emplace( entry.first, "type" );
        }
        for ( auto& entry : Workspace::globalWorkspace().getTypeTable() )
        {
            by_name.emplace( entry.first, "type" );
        }

        std::vector<CompletionItem> items;
        items.reserve( by_name.size() );
        for ( auto& entry : by_name )
        {
            items.push_back( { entry.first, entry.second } );
        }
        return items;
    }

    bool startsWith(const std::string& text, const std::string& prefix)
    {
        return text.size() >= prefix.size() and text.compare( 0, prefix.size(), prefix ) == 0;
    }

}


std::vector<CompletionItem> RevLanguage::complete(const std::string& buffer, std::size_t cursor,
                                                    const std::shared_ptr<Environment>& env, std::size_t& replace_from)
{
    std::string cmd = buffer.substr( 0, std::min( cursor, buffer.size() ) );

    ParserInfo pi = Parser::getParser().checkCommand( cmd, env );

    if ( pi.inComment )
    {
        replace_from = cmd.size();
        return {};
    }

    std::size_t command_pos = 0;
    std::vector<CompletionItem> candidates;

    if ( pi.inQuote )
    {
        // complete a file path typed after an opening quote
        std::size_t quote_pos = cmd.rfind( '"' );
        command_pos = ( quote_pos == std::string::npos ) ? 0 : quote_pos + 1;
        candidates = fileCandidates( cmd.substr( command_pos ) );
    }
    else
    {
        // find the rightmost expression separator; completion only ever replaces the text after it
        static const std::vector<std::string> separators =
            { " ", "%", "~", "=", "&", "|", "+", "-", "*", "/", "^", "!", ",", "<", ">", ")", "[", "]", "{", "}" };
        for ( auto& s : separators )
        {
            std::size_t pos = cmd.rfind( s );
            if ( pos != std::string::npos )
            {
                command_pos = std::max( command_pos, pos );
            }
        }

        if ( not pi.function_name.empty() )
        {
            if ( not pi.argument_label.empty() )
            {
                // assigning an argument label (`f(arg = `): not sure exactly what should complete here, so --
                // as in the original terminal-client logic -- offer everything
                std::size_t eq_pos = cmd.rfind( '=' );
                command_pos = ( eq_pos == std::string::npos ) ? command_pos : eq_pos + 1;
                candidates = defaultCandidates();
            }
            else
            {
                // inside a call's argument list: complete argument labels of every overload of the function
                std::size_t paren_pos = cmd.rfind( '(' );
                std::size_t comma_pos = cmd.rfind( ',' );
                command_pos = std::max( paren_pos == std::string::npos ? 0 : paren_pos + 1,
                                        comma_pos == std::string::npos ? 0 : comma_pos + 1 );

                std::vector<Function*> overloads = Workspace::globalWorkspace().getFunctionTable().findFunctions( pi.function_name );
                for ( auto* f : overloads )
                {
                    const ArgumentRules& rules = f->getArgumentRules();
                    for ( std::size_t i = 0; i < rules.size(); i++ )
                    {
                        // intentionally not deduplicated: two overloads sharing an argument label (e.g. both
                        // taking "x") offer it twice, matching the original terminal-client behaviour
                        candidates.push_back( { rules[i].getArgumentLabel(), "argument" } );
                    }
                }
            }
        }
        else
        {
            if ( command_pos > 0 )
            {
                command_pos++;
            }
            candidates = defaultCandidates();
        }
    }

    while ( command_pos < cmd.size() and cmd[command_pos] == ' ' )
    {
        command_pos++;
    }

    const std::string prefix = cmd.substr( 0, command_pos );
    const std::string match_against = cmd.substr( command_pos );

    std::vector<CompletionItem> items;
    for ( auto& candidate : candidates )
    {
        if ( startsWith( candidate.text, match_against ) )
        {
            items.push_back( { prefix + candidate.text, candidate.kind } );
        }
    }

    replace_from = command_pos;
    return items;
}
