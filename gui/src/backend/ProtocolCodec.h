#ifndef REVSTUDIO_PROTOCOLCODEC_H
#define REVSTUDIO_PROTOCOLCODEC_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

// Wire format of the RevBayes backend protocol (GUI_Implementation_Note.md, section 6.4): one JSON object per line,
// UTF-8. Nothing here knows about processes or widgets, so it is fully unit-testable.

namespace revstudio {

/** Splits a byte stream into lines. Tolerates CRLF and lines split across reads, and caps the size of one line. */
class LineFramer
{
public:
    static constexpr qsizetype defaultMaxLineBytes = 16 * 1024 * 1024;

    explicit LineFramer(qsizetype maxLineBytes = defaultMaxLineBytes) : maxLineBytes_(maxLineBytes) {}

    void append(const QByteArray& data);

    /** Next complete line without its terminator (a trailing '\r' is dropped). False if none is complete yet. */
    bool nextLine(QByteArray* line);

    /** True if a line longer than the limit was dropped since the last call; the flag is cleared. */
    bool takeOverflow();

    /** Bytes received but not yet returned as a line. */
    qsizetype pendingBytes() const { return buffer_.size(); }

private:
    qsizetype  maxLineBytes_;
    QByteArray buffer_;
    bool       discarding_ = false;   // inside an oversized line: drop bytes up to the next newline
    bool       overflow_   = false;
};

namespace protocol {

constexpr int version = 1;

/** One request line, including the trailing newline: {"cmd": ..., "id": ..., <fields>}. */
QByteArray encodeRequest(const QString& cmd, qint64 id, const QJsonObject& fields = QJsonObject());

/** Parses one line. Returns nullopt (and fills *error) unless it is a JSON object. */
std::optional<QJsonObject> parseMessage(const QByteArray& line, QString* error = nullptr);

} // namespace protocol
} // namespace revstudio

#endif
