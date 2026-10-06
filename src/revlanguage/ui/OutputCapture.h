/**
 * @file
 * Declaration of OutputCapture: RAII redirection of every output channel the interpreter can write to, so
 * `rb --server` can forward it as protocol `output` events (GUI_Implementation_Note.md, section 6.3/6.4).
 *
 * @license GPL version 3
 */

#ifndef OutputCapture_H
#define OutputCapture_H

#include <functional>
#include <memory>
#include <streambuf>
#include <string>

class UserInterfaceOutputStream;

namespace RevLanguage {

    /**
     * Captures three channels for as long as the object is alive:
     *   - RBOUT / UserInterface::output   -> installs a UserInterfaceOutputStream subclass ("rbout")
     *   - std::cout                        -> its streambuf is swapped for one that forwards ("stdout")
     *   - std::cerr                        -> swapped for one that forwards ("stderr")
     *
     * The sink is called synchronously, on whatever thread produced the output (in `rb --server`, that is always
     * the interpreter thread: RevServer never touches these channels from its reader thread).
     *
     * The destructor restores the original UserInterface output stream and the original std::cout/std::cerr
     * streambufs, in that order, before anything referenced by the capture is destroyed. Restoring the streambufs
     * is not optional: a probe during phase 0 (see GUI_Implementation_Note.md, section 1 and Appendix C) showed the
     * process segfaults at exit if a swapped-in streambuf is destroyed while std::cout/std::cerr still point at it,
     * because libstdc++ flushes through the (by then dangling) rdbuf pointer during static destruction.
     *
     * `rb --server`'s own quit path (Parser's quitRequestHandler, see Parser.h) calls std::_Exit() directly from
     * deep in the call stack (not plain exit(): that runs static-duration destructors, including std::cin's, which
     * can deadlock against a reader thread still blocked inside std::getline(std::cin, ...) -- found empirically
     * while testing this). _Exit() does not unwind local objects and does not run static-duration destructors at
     * all, so an OutputCapture instance still on the stack at that point is simply never destroyed. That is safe:
     * its target streambuffers stay alive along with it, so nothing else (there is nothing else, since _Exit()
     * skips global cleanup entirely) can dereference a dangling pointer. Its destructor's restore step just never
     * fires, which does not matter because the process is terminating regardless.
     */
    class OutputCapture {

    public:
        enum class Stream { RBOut, Stdout, Stderr };

        //! sink(text, which) receives raw, possibly partial chunks of text as they are written; a caller that wants
        //! whole lines should buffer and split on '\n' itself.
        explicit                    OutputCapture(std::function<void(const std::string&, Stream)> sink);
                                    OutputCapture(const OutputCapture&) = delete;
        OutputCapture&              operator=(const OutputCapture&) = delete;
                                    ~OutputCapture(void);

    private:
        class ForwardingStreamBuf;
        class RboutSink;

        std::unique_ptr<RboutSink>            rbout_sink;
        UserInterfaceOutputStream*             previous_output_stream;   //!< not owned; restored, never deleted

        std::unique_ptr<ForwardingStreamBuf>  cout_buf;
        std::unique_ptr<ForwardingStreamBuf>  cerr_buf;
        std::streambuf*                        previous_cout_buf;
        std::streambuf*                        previous_cerr_buf;
    };

}

#endif
