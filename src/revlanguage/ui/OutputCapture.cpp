#include "OutputCapture.h"

#include "RlUserInterface.h"
#include "RlUserInterfaceOutputStream.h"

using namespace RevLanguage;


/** A std::streambuf that forwards every character written through it to a sink, tagged with a fixed Stream. */
class OutputCapture::ForwardingStreamBuf : public std::streambuf {

public:
    ForwardingStreamBuf(std::function<void(const std::string&, Stream)> sink, Stream which) :
        sink( std::move( sink ) ),
        which( which )
    {

    }

protected:
    int_type overflow(int_type ch) override
    {
        if ( ch != traits_type::eof() )
        {
            char c = traits_type::to_char_type( ch );
            sink( std::string( 1, c ), which );
        }
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        // The common case (a whole write() / operator<< in one call): forward it as a single chunk instead of
        // going through overflow() one byte at a time.
        if ( n > 0 )
        {
            sink( std::string( s, static_cast<std::size_t>( n ) ), which );
        }
        return n;
    }

private:
    std::function<void(const std::string&, Stream)> sink;
    Stream                                           which;
};


/** Forwards RBOUT / UserInterface::output() calls to the sink, tagged Stream::RBOut. */
class OutputCapture::RboutSink : public UserInterfaceOutputStream {

public:
    explicit RboutSink(std::function<void(const std::string&, Stream)> sink) : sink( std::move( sink ) ) {}

    void output(const std::string& text) const override
    {
        sink( text, Stream::RBOut );
    }

    void outputEndOfLine(void) const override
    {
        sink( "\n", Stream::RBOut );
    }

private:
    std::function<void(const std::string&, Stream)> sink;
};


OutputCapture::OutputCapture(std::function<void(const std::string&, Stream)> sink)
{
    rbout_sink = std::make_unique<RboutSink>( sink );
    previous_output_stream = UserInterface::userInterface().getOutputStream();
    UserInterface::userInterface().setOutputStream( rbout_sink.get() );

    cout_buf = std::make_unique<ForwardingStreamBuf>( sink, Stream::Stdout );
    cerr_buf = std::make_unique<ForwardingStreamBuf>( sink, Stream::Stderr );
    previous_cout_buf = std::cout.rdbuf( cout_buf.get() );
    previous_cerr_buf = std::cerr.rdbuf( cerr_buf.get() );
}


OutputCapture::~OutputCapture(void)
{
    // Restore std::cout/std::cerr BEFORE the forwarding streambufs are destroyed by the member destructors that run
    // right after this body: the opposite order is the crash found in the phase 0 probe (see the class comment).
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf( previous_cout_buf );
    std::cerr.rdbuf( previous_cerr_buf );

    UserInterface::userInterface().setOutputStream( previous_output_stream );
}
