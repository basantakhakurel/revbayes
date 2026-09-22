#include "backend/ProtocolCodec.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace revstudio {

void LineFramer::append(const QByteArray& data)
{
    buffer_.append(data);

    // Inside an oversized line: throw bytes away until its newline arrives, then resume normally.
    while (discarding_)
    {
        const qsizetype newline = buffer_.indexOf('\n');
        if (newline < 0)
        {
            buffer_.clear();
            return;
        }
        buffer_.remove(0, newline + 1);
        discarding_ = false;
    }
}

bool LineFramer::nextLine(QByteArray* line)
{
    for (;;)
    {
        const qsizetype newline = buffer_.indexOf('\n');

        if (newline < 0)
        {
            if (buffer_.size() > maxLineBytes_)
            {
                buffer_.clear();
                discarding_ = true;
                overflow_   = true;
            }
            return false;
        }

        if (newline > maxLineBytes_)
        {
            buffer_.remove(0, newline + 1);
            overflow_ = true;
            continue;                       // the next line may be fine
        }

        QByteArray text = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        if (text.endsWith('\r'))
        {
            text.chop(1);
        }
        *line = text;
        return true;
    }
}

bool LineFramer::takeOverflow()
{
    const bool was = overflow_;
    overflow_ = false;
    return was;
}

namespace protocol {

QByteArray encodeRequest(const QString& cmd, qint64 id, const QJsonObject& fields)
{
    QJsonObject object = fields;
    object.insert(QStringLiteral("cmd"), cmd);
    object.insert(QStringLiteral("id"), id);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<QJsonObject> parseMessage(const QByteArray& line, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);

    if (parseError.error != QJsonParseError::NoError)
    {
        if (error)
        {
            *error = parseError.errorString();
        }
        return std::nullopt;
    }
    if (!document.isObject())
    {
        if (error)
        {
            *error = QStringLiteral("not a JSON object");
        }
        return std::nullopt;
    }
    return document.object();
}

} // namespace protocol
} // namespace revstudio
