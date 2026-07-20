#pragma once

#include "types.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>

#include <optional>

namespace akrion::core {

struct DecodeError {
    QString code;
    QString message;
    quint64 lineNumber = 0;
    quint64 byteOffset = 0;
    QByteArray rawExcerpt;
};

struct DecodeBatch {
    QVector<Frame> frames;
    QVector<DecodeError> errors;
};

class NdjsonFrameDecoder final {
public:
    explicit NdjsonFrameDecoder(qsizetype maximumLineBytes = 1024 * 1024);

    DecodeBatch feed(QByteArrayView bytes,
                     std::optional<quint64> hostReceiveTimeUs = std::nullopt);
    DecodeBatch finish(std::optional<quint64> hostReceiveTimeUs = std::nullopt);
    void reset();

    qsizetype bufferedBytes() const { return m_buffer.size(); }
    quint64 lineNumber() const { return m_lineNumber; }
    quint64 consumedBytes() const { return m_consumedBytes; }

    static bool decodeLine(QByteArrayView line, Frame* frame, QString* error = nullptr);

private:
    void decodeBufferedLine(QByteArray line, quint64 offset,
                            std::optional<quint64> hostReceiveTimeUs, DecodeBatch* batch);

    QByteArray m_buffer;
    qsizetype m_maximumLineBytes;
    quint64 m_lineNumber = 0;
    quint64 m_consumedBytes = 0;
    quint64 m_lineStartOffset = 0;
    quint64 m_discardedBytesOnLine = 0;
    bool m_discardingOversizedLine = false;
};

} // namespace akrion::core
