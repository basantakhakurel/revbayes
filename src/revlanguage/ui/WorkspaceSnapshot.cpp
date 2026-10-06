#include "WorkspaceSnapshot.h"

#include "ArgumentRule.h"
#include "ArgumentRules.h"
#include "DagNode.h"
#include "Environment.h"
#include "FunctionTable.h"
#include "RbException.h"
#include "RevObject.h"
#include "RevVariable.h"
#include "RlFunction.h"

#include <ostream>
#include <streambuf>

using json = nlohmann::json;
using namespace RevLanguage;


namespace {

    /** Thrown by BoundedStreamBuf once its character limit is reached, to unwind out of printValue immediately
     *  instead of letting it keep iterating (e.g. over a huge container) just to produce output nobody keeps. */
    struct SummaryTruncated {};

    /** Appends into `target` up to `limit` characters, then throws SummaryTruncated instead of accepting more. */
    class BoundedStreamBuf : public std::streambuf {

    public:
        BoundedStreamBuf(std::string& target, std::size_t limit) : target( target ), limit( limit ) {}

    protected:
        int_type overflow(int_type ch) override
        {
            if ( ch == traits_type::eof() )
            {
                return ch;
            }
            if ( target.size() >= limit )
            {
                throw SummaryTruncated{};
            }
            target.push_back( traits_type::to_char_type( ch ) );
            return ch;
        }

        std::streamsize xsputn(const char* s, std::streamsize n) override
        {
            if ( n < 0 )
            {
                return 0;
            }
            const std::size_t room = target.size() < limit ? limit - target.size() : 0;
            const std::size_t take = static_cast<std::size_t>( n ) < room ? static_cast<std::size_t>( n ) : room;
            target.append( s, take );
            if ( take < static_cast<std::size_t>( n ) )
            {
                throw SummaryTruncated{};
            }
            return n;
        }

    private:
        std::string& target;
        std::size_t  limit;
    };


    /** The Rev-language name of an element variable's parent, e.g. "x" for "x[1]"; empty if `name` has no "[". */
    std::string parentName(const std::string& name)
    {
        const auto bracket = name.find( '[' );
        return bracket == std::string::npos ? std::string() : name.substr( 0, bracket );
    }

}


std::string WorkspaceSnapshot::boundedSummary(const RevObject& obj, std::size_t max_chars, bool* truncated)
{
    if ( truncated )
    {
        *truncated = false;
    }

    std::string result;
    result.reserve( max_chars );
    BoundedStreamBuf buf( result, max_chars );
    std::ostream stream( &buf );
    // Without this, std::ostream's formatted-output operators (the "o << x" calls inside printValue) catch ANY
    // exception the streambuf throws, set badbit, and -- since exceptions() defaults to goodbit -- silently
    // swallow it instead of letting it propagate. That defeats both halves of the point of throwing here: the
    // caller (a printValue that loops over a container's elements) would keep calling operator<< as a no-op for
    // every remaining element instead of stopping, so a huge container would still cost O(n) time even though it
    // no longer costs O(n) space; and boundedSummary() below would never see SummaryTruncated at all. Found
    // empirically: without this line, a 200000-element vector's summary was silently cut to exactly max_chars
    // with no ellipsis and no `truncated` flag, instead of the exception unwinding the print loop immediately.
    stream.exceptions( std::ios_base::badbit );

    try
    {
        obj.printValue( stream, true );
    }
    catch ( SummaryTruncated& )
    {
        if ( truncated )
        {
            *truncated = true;
        }
        result += "…";   // ellipsis
    }
    catch ( ... )
    {
        // A type's printValue is not documented to be exception-free; treat any other failure the same way a
        // failed getRevObject() is treated below, rather than losing the whole snapshot over one bad value.
        return "<error printing value>";
    }

    return result;
}


nlohmann::json WorkspaceSnapshot::variableRows(Environment& env)
{
    json rows = json::array();

    for ( const auto& entry : env.getVariableTable() )
    {
        const std::string& name = entry.first;
        const RevPtr<RevVariable>& var = entry.second;

        json row;
        row["name"] = name;

        json flags = json::array();
        if ( var->isReferenceVariable() )
        {
            flags.push_back( "reference" );
        }
        if ( var->isHiddenVariable() )
        {
            flags.push_back( "hidden" );
        }
        if ( var->isElementVariable() )
        {
            flags.push_back( "element" );
            const std::string parent = parentName( name );
            if ( not parent.empty() )
            {
                row["parent"] = parent;
            }
        }
        if ( name == "args" )
        {
            // Created by RevLanguageMain::startRevLanguageEnvironment on every start-up (GUI_Implementation_Note.md,
            // section 4.12), not by anything the user asked for; isHiddenVariable() does not cover it.
            flags.push_back( "system" );
        }
        row["flags"] = flags;

        try
        {
            RevObject& obj = var->getRevObject();   // follows references; may build a pending vector variable; may throw

            row["type"] = obj.getType();

            if ( obj.isModelObject() and obj.hasDagNode() )
            {
                RevBayesCore::DagNode* node = obj.getDagNode();
                if ( node->isClamped() )
                {
                    row["kind"] = "clamped";
                }
                else if ( node->isStochastic() )
                {
                    row["kind"] = "stochastic";
                }
                else if ( node->isConstant() )
                {
                    row["kind"] = "constant";
                }
                else
                {
                    row["kind"] = "deterministic";
                }
                row["children"] = node->getNumberOfChildren();
            }
            else
            {
                row["kind"] = "workspace";
            }

            bool truncated = false;
            row["summary"] = boundedSummary( obj, 120, &truncated );
            if ( truncated )
            {
                row["summary_truncated"] = true;
            }
        }
        catch ( RbException& e )
        {
            row["kind"]    = "unknown";
            row["type"]    = "";
            row["summary"] = "<" + e.getMessage() + ">";
        }
        catch ( ... )
        {
            row["kind"]    = "unknown";
            row["type"]    = "";
            row["summary"] = "<error inspecting variable>";
        }

        rows.push_back( std::move( row ) );
    }

    return rows;
}


nlohmann::json WorkspaceSnapshot::functionRows(const FunctionTable& table)
{
    json rows = json::array();

    for ( const auto& entry : table )
    {
        const std::string& name = entry.first;
        const Function* func = entry.second;

        std::string signature = func->getReturnType().getType() + " " + name + "(";
        const ArgumentRules& rules = func->getArgumentRules();
        for ( std::size_t i = 0; i < rules.size(); i++ )
        {
            signature += rules[i].getArgumentLabel();
            if ( i + 1 < rules.size() )
            {
                signature += ", ";
            }
        }
        signature += ")";

        json row;
        row["name"]      = name;
        row["signature"] = signature;
        rows.push_back( std::move( row ) );
    }

    return rows;
}
