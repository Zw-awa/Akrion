#include "frame_decoder.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace akrion::core {
namespace {

QByteArray excerpt(QByteArrayView line) {
    constexpr qsizetype maximumExcerpt = 256;
    return QByteArray(line.first(qMin(line.size(), maximumExcerpt)));
}

} // namespace

NdjsonFrameDecoder::NdjsonFrameDecoder(qsizetype maximumLineBytes)
    : m_maximumLineBytes(qMax<qsizetype>(256, maximumLineBytes)) {}

DecodeBatch NdjsonFrameDecoder::feed(QByteArrayView bytes,
                                     std::optional<quint64> hostReceiveTimeUs) {
    DecodeBatch batch;
    m_consumedBytes += static_cast<quint64>(bytes.size());
    m_buffer.append(bytes.data(), bytes.size());

    while (true) {
        const auto newline = m_buffer.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        const auto offset = m_lineStartOffset;
        m_lineStartOffset += m_discardedBytesOnLine + static_cast<quint64>(newline + 1);
        ++m_lineNumber;
        if (m_discardingOversizedLine) {
            m_discardingOversizedLine = false;
            m_discardedBytesOnLine = 0;
            continue;
        }
        if (line.size() > m_maximumLineBytes) {
            batch.errors.append({QStringLiteral("line_too_long"),
                                 QStringLiteral("NDJSON line exceeds %1 bytes")
                                     .arg(m_maximumLineBytes),
                                 m_lineNumber, offset, excerpt(line)});
            continue;
        }
        decodeBufferedLine(std::move(line), offset, hostReceiveTimeUs, &batch);
    }

    if (!m_discardingOversizedLine && m_buffer.size() > m_maximumLineBytes) {
        batch.errors.append({QStringLiteral("line_too_long"),
                             QStringLiteral("NDJSON line exceeds %1 bytes")
                                 .arg(m_maximumLineBytes),
                             m_lineNumber + 1, m_lineStartOffset, excerpt(m_buffer)});
        m_discardedBytesOnLine += static_cast<quint64>(m_buffer.size());
        m_buffer.clear();
        m_discardingOversizedLine = true;
    } else if (m_discardingOversizedLine && m_buffer.size() > m_maximumLineBytes) {
        m_discardedBytesOnLine += static_cast<quint64>(m_buffer.size());
        m_buffer.clear();
    }
    return batch;
}

DecodeBatch NdjsonFrameDecoder::finish(std::optional<quint64> hostReceiveTimeUs) {
    DecodeBatch batch;
    if (m_discardingOversizedLine) {
        m_buffer.clear();
        m_discardingOversizedLine = false;
        m_discardedBytesOnLine = 0;
        return batch;
    }
    if (!m_buffer.trimmed().isEmpty()) {
        ++m_lineNumber;
        batch.errors.append({QStringLiteral("truncated_line"),
                             QStringLiteral("stream ended before the final newline"),
                             m_lineNumber, m_lineStartOffset, excerpt(m_buffer)});
        Q_UNUSED(hostReceiveTimeUs)
    }
    m_buffer.clear();
    return batch;
}

void NdjsonFrameDecoder::reset() {
    m_buffer.clear();
    m_lineNumber = 0;
    m_consumedBytes = 0;
    m_lineStartOffset = 0;
    m_discardedBytesOnLine = 0;
    m_discardingOversizedLine = false;
}

bool NdjsonFrameDecoder::decodeLine(QByteArrayView line, Frame* frame, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(QByteArray(line), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("invalid JSON at byte %1: %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("frame must be a JSON object");
        return false;
    }
    return frameFromJson(document.object(), frame, error);
}

void NdjsonFrameDecoder::decodeBufferedLine(QByteArray line, quint64 offset,
                                             std::optional<quint64> hostReceiveTimeUs,
                                             DecodeBatch* batch) {
    if (line.endsWith('\r')) line.chop(1);
    if (line.trimmed().isEmpty()) return;
    Frame frame;
    QString error;
    if (!decodeLine(line, &frame, &error)) {
        batch->errors.append({QStringLiteral("invalid_frame"), error, m_lineNumber, offset,
                              excerpt(line)});
        return;
    }
    if (hostReceiveTimeUs) frame.hostReceiveTimeUs = hostReceiveTimeUs;
    batch->frames.append(std::move(frame));
}

} // namespace akrion::core
