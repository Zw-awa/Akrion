#include "runpackage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace akrion {
namespace {

constexpr quint32 kLocalHeaderSignature = 0x04034b50U;
constexpr quint32 kCentralHeaderSignature = 0x02014b50U;
constexpr quint32 kEndOfCentralDirectorySignature = 0x06054b50U;
constexpr quint16 kZipVersion = 20;
constexpr quint16 kUtf8Flag = 0x0800U;
constexpr quint16 kStoredMethod = 0;
constexpr quint16 kDosEpochDate = 0x0021U;
constexpr quint64 kMaxZip32Value = std::numeric_limits<quint32>::max();
constexpr qsizetype kCopyBufferSize = 1024 * 1024;
constexpr qint64 kMaxCentralDirectorySize = 64 * 1024 * 1024;
constexpr quint16 kMaxArchiveEntries = 4096;
constexpr quint32 kMaxMetadataSize = 16 * 1024 * 1024;

const QStringList& requiredRunFiles() {
    static const QStringList files {
        QStringLiteral("manifest.json"),
        QStringLiteral("config.json"),
        QStringLiteral("serial.raw"),
        QStringLiteral("frames.ndjson"),
        QStringLiteral("events.ndjson"),
        QStringLiteral("summary.json"),
    };
    return files;
}

PackageResult failure(PackageError error, const QString& message) {
    PackageResult result;
    result.error = error;
    result.message = message;
    return result;
}

void appendU16(QByteArray& target, quint16 value) {
    target.append(static_cast<char>(value & 0xffU));
    target.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendU32(QByteArray& target, quint32 value) {
    target.append(static_cast<char>(value & 0xffU));
    target.append(static_cast<char>((value >> 8U) & 0xffU));
    target.append(static_cast<char>((value >> 16U) & 0xffU));
    target.append(static_cast<char>((value >> 24U) & 0xffU));
}

quint16 readU16(const QByteArray& source, qsizetype offset) {
    const auto* data = reinterpret_cast<const uchar*>(source.constData() + offset);
    return static_cast<quint16>(data[0])
        | static_cast<quint16>(static_cast<quint16>(data[1]) << 8U);
}

quint32 readU32(const QByteArray& source, qsizetype offset) {
    const auto* data = reinterpret_cast<const uchar*>(source.constData() + offset);
    return static_cast<quint32>(data[0])
        | (static_cast<quint32>(data[1]) << 8U)
        | (static_cast<quint32>(data[2]) << 16U)
        | (static_cast<quint32>(data[3]) << 24U);
}

bool writeAll(QIODevice& device, const QByteArray& data) {
    qsizetype written = 0;
    while (written < data.size()) {
        const qint64 count = device.write(data.constData() + written, data.size() - written);
        if (count <= 0) {
            return false;
        }
        written += static_cast<qsizetype>(count);
    }
    return true;
}

bool readExact(QFile& file, qint64 count, QByteArray& data) {
    if (count < 0 || count > std::numeric_limits<qsizetype>::max()) {
        return false;
    }
    data = file.read(count);
    return data.size() == count;
}

constexpr std::array<quint32, 256> makeCrcTable() {
    std::array<quint32, 256> table {};
    for (quint32 index = 0; index < table.size(); ++index) {
        quint32 value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
        }
        table[index] = value;
    }
    return table;
}

class Crc32 final {
public:
    void add(const QByteArray& data) {
        static constexpr auto table = makeCrcTable();
        for (const char byte : data) {
            const auto index = static_cast<quint8>(m_value ^ static_cast<quint8>(byte));
            m_value = table[index] ^ (m_value >> 8U);
        }
    }

    [[nodiscard]] quint32 value() const noexcept { return m_value ^ 0xffffffffU; }

private:
    quint32 m_value = 0xffffffffU;
};

struct FileDigest {
    QString path;
    QByteArray encodedPath;
    QString absolutePath;
    quint64 size = 0;
    quint32 crc32 = 0;
    QByteArray sha256;
};

struct ArchiveEntry : FileDigest {
    QByteArray inlineData;
    quint32 localHeaderOffset = 0;
};

struct ZipEntry {
    QString path;
    QByteArray encodedPath;
    quint16 flags = 0;
    quint16 method = 0;
    quint32 crc32 = 0;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
    quint32 localHeaderOffset = 0;
    quint64 dataOffset = 0;
    quint32 externalAttributes = 0;
};

bool isHexDigest(const QString& value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        const bool digit = code >= '0' && code <= '9';
        const bool lower = code >= 'a' && code <= 'f';
        const bool upper = code >= 'A' && code <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

bool isReservedWindowsName(const QString& component) {
    const QString base = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> fixed {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
    };
    if (fixed.contains(base)) {
        return true;
    }
    if (base.size() == 4 && (base.startsWith(QStringLiteral("COM"))
                             || base.startsWith(QStringLiteral("LPT")))) {
        return base.at(3) >= QLatin1Char('1') && base.at(3) <= QLatin1Char('9');
    }
    return false;
}

bool validateEntryPath(const QByteArray& encodedPath, QString& path, QString& error) {
    if (encodedPath.isEmpty() || encodedPath.size() > 4096 || encodedPath.contains('\0')) {
        error = QStringLiteral("Archive entry has an empty, oversized, or NUL-containing path");
        return false;
    }
    path = QString::fromUtf8(encodedPath);
    if (path.toUtf8() != encodedPath) {
        error = QStringLiteral("Archive entry path is not valid UTF-8");
        return false;
    }
    if (path.contains(QLatin1Char('\\')) || path.startsWith(QLatin1Char('/'))
        || path.endsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) {
        error = QStringLiteral("Archive entry path is absolute, ambiguous, or a directory");
        return false;
    }

    const QStringList components = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& component : components) {
        if (component.isEmpty() || component == QStringLiteral(".") || component == QStringLiteral("..")
            || component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))
            || isReservedWindowsName(component)) {
            error = QStringLiteral("Archive entry contains an unsafe path component: %1").arg(path);
            return false;
        }
        for (const QChar character : component) {
            if (character.unicode() < 0x20U || QStringLiteral("<>:\"|?*").contains(character)) {
                error = QStringLiteral("Archive entry contains characters unsafe on Windows: %1").arg(path);
                return false;
            }
        }
    }
    if (QDir::cleanPath(path) != path) {
        error = QStringLiteral("Archive entry path is not normalized: %1").arg(path);
        return false;
    }
    return true;
}

bool pathsEqual(const QString& left, const QString& right) {
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

bool pathIsInside(const QString& candidate, const QString& directory) {
    QString normalizedDirectory = QDir::cleanPath(directory);
    if (!normalizedDirectory.endsWith(QLatin1Char('/'))) {
        normalizedDirectory += QLatin1Char('/');
    }
#ifdef Q_OS_WIN
    return candidate.startsWith(normalizedDirectory, Qt::CaseInsensitive);
#else
    return candidate.startsWith(normalizedDirectory);
#endif
}

PackageResult readCompletedRunManifest(const QString& manifestPath) {
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot read run manifest: %1").arg(file.errorString()));
    }
    if (file.size() > kMaxMetadataSize) {
        return failure(PackageError::InvalidRun, QStringLiteral("Run manifest is unexpectedly large"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(PackageError::InvalidRun,
                       QStringLiteral("Run manifest is not valid JSON: %1").arg(parseError.errorString()));
    }
    const QString status = document.object().value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("completed") && status != QStringLiteral("interrupted")
        && status != QStringLiteral("recovered")) {
        return failure(PackageError::InvalidRun,
                       QStringLiteral("Run status must be completed, interrupted, or recovered"));
    }
    return {};
}

PackageResult digestFile(const QString& absolutePath, FileDigest& digest) {
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot read %1: %2").arg(absolutePath, file.errorString()));
    }
    if (file.size() < 0 || static_cast<quint64>(file.size()) >= kMaxZip32Value) {
        return failure(PackageError::Unsupported,
                       QStringLiteral("ZIP64 is required for file: %1").arg(digest.path));
    }

    Crc32 crc;
    QCryptographicHash sha(QCryptographicHash::Sha256);
    quint64 size = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kCopyBufferSize);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return failure(PackageError::Io,
                           QStringLiteral("Cannot read %1: %2").arg(absolutePath, file.errorString()));
        }
        crc.add(chunk);
        sha.addData(chunk);
        size += static_cast<quint64>(chunk.size());
    }
    digest.size = size;
    digest.crc32 = crc.value();
    digest.sha256 = sha.result();
    return {};
}

FileDigest digestBytes(const QString& path, const QByteArray& data) {
    Crc32 crc;
    crc.add(data);
    FileDigest digest;
    digest.path = path;
    digest.encodedPath = path.toUtf8();
    digest.size = static_cast<quint64>(data.size());
    digest.crc32 = crc.value();
    digest.sha256 = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    return digest;
}

QByteArray buildPackageManifest(const QList<ArchiveEntry>& entries) {
    QJsonArray files;
    for (const ArchiveEntry& entry : entries) {
        QJsonObject file;
        file.insert(QStringLiteral("path"), entry.path);
        file.insert(QStringLiteral("size"), static_cast<qint64>(entry.size));
        file.insert(QStringLiteral("sha256"), QString::fromLatin1(entry.sha256.toHex()));
        files.append(file);
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), QString::fromLatin1(RunPackage::packageSchema));
    manifest.insert(QStringLiteral("format"), QStringLiteral("zip"));
    manifest.insert(QStringLiteral("compression"), QStringLiteral("store"));
    manifest.insert(QStringLiteral("files"), files);
    QByteArray data = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!data.endsWith('\n')) {
        data.append('\n');
    }
    return data;
}

QByteArray localHeader(const ArchiveEntry& entry) {
    QByteArray header;
    header.reserve(30 + entry.encodedPath.size());
    appendU32(header, kLocalHeaderSignature);
    appendU16(header, kZipVersion);
    appendU16(header, kUtf8Flag);
    appendU16(header, kStoredMethod);
    appendU16(header, 0);
    appendU16(header, kDosEpochDate);
    appendU32(header, entry.crc32);
    appendU32(header, static_cast<quint32>(entry.size));
    appendU32(header, static_cast<quint32>(entry.size));
    appendU16(header, static_cast<quint16>(entry.encodedPath.size()));
    appendU16(header, 0);
    header.append(entry.encodedPath);
    return header;
}

QByteArray centralHeader(const ArchiveEntry& entry) {
    QByteArray header;
    header.reserve(46 + entry.encodedPath.size());
    appendU32(header, kCentralHeaderSignature);
    appendU16(header, kZipVersion);
    appendU16(header, kZipVersion);
    appendU16(header, kUtf8Flag);
    appendU16(header, kStoredMethod);
    appendU16(header, 0);
    appendU16(header, kDosEpochDate);
    appendU32(header, entry.crc32);
    appendU32(header, static_cast<quint32>(entry.size));
    appendU32(header, static_cast<quint32>(entry.size));
    appendU16(header, static_cast<quint16>(entry.encodedPath.size()));
    appendU16(header, 0);
    appendU16(header, 0);
    appendU16(header, 0);
    appendU16(header, 0);
    appendU32(header, 0);
    appendU32(header, entry.localHeaderOffset);
    header.append(entry.encodedPath);
    return header;
}

PackageResult writeEntry(QSaveFile& archive, ArchiveEntry& entry) {
    if (archive.pos() < 0 || static_cast<quint64>(archive.pos()) >= kMaxZip32Value
        || entry.encodedPath.size() > std::numeric_limits<quint16>::max()) {
        return failure(PackageError::Unsupported, QStringLiteral("ZIP64 is required for this archive"));
    }
    entry.localHeaderOffset = static_cast<quint32>(archive.pos());
    if (!writeAll(archive, localHeader(entry))) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot write archive header: %1").arg(archive.errorString()));
    }

    if (!entry.absolutePath.isEmpty()) {
        QFile source(entry.absolutePath);
        if (!source.open(QIODevice::ReadOnly)) {
            return failure(PackageError::Io,
                           QStringLiteral("Cannot reopen %1: %2").arg(entry.path, source.errorString()));
        }
        Crc32 crc;
        QCryptographicHash sha(QCryptographicHash::Sha256);
        quint64 size = 0;
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(kCopyBufferSize);
            if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
                return failure(PackageError::Io,
                               QStringLiteral("Cannot read %1: %2").arg(entry.path, source.errorString()));
            }
            if (!writeAll(archive, chunk)) {
                return failure(PackageError::Io,
                               QStringLiteral("Cannot write archive data: %1").arg(archive.errorString()));
            }
            crc.add(chunk);
            sha.addData(chunk);
            size += static_cast<quint64>(chunk.size());
        }
        if (size != entry.size || crc.value() != entry.crc32 || sha.result() != entry.sha256) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run file changed while it was being packed: %1").arg(entry.path));
        }
    } else if (!writeAll(archive, entry.inlineData)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot write package manifest: %1").arg(archive.errorString()));
    }
    return {};
}

} // namespace

PackageResult RunPackage::pack(const QString& runDirectory, const QString& archivePath) {
    if (runDirectory.trimmed().isEmpty() || archivePath.trimmed().isEmpty()) {
        return failure(PackageError::InvalidArgument,
                       QStringLiteral("Run directory and archive path are required"));
    }

    const QFileInfo sourceInfo(runDirectory);
    if (!sourceInfo.exists() || !sourceInfo.isDir() || sourceInfo.isSymLink()) {
        return failure(PackageError::InvalidArgument,
                       QStringLiteral("Run directory does not exist or is not a regular directory"));
    }
    const QString sourcePath = QDir::cleanPath(sourceInfo.canonicalFilePath());
    const QString outputPath = QDir::cleanPath(QFileInfo(archivePath).absoluteFilePath());
    if (QFileInfo::exists(outputPath)) {
        return failure(PackageError::DestinationExists,
                       QStringLiteral("Archive already exists: %1").arg(outputPath));
    }
    if (pathsEqual(outputPath, sourcePath) || pathIsInside(outputPath, sourcePath)) {
        return failure(PackageError::InvalidArgument,
                       QStringLiteral("Archive must be written outside the run directory"));
    }

    PackageResult manifestCheck = readCompletedRunManifest(QDir(sourcePath).filePath(QStringLiteral("manifest.json")));
    if (!manifestCheck) {
        return manifestCheck;
    }

    QList<ArchiveEntry> entries;
    QSet<QString> caseFoldedPaths;
    QSet<QString> exactPaths;
    QDirIterator iterator(sourcePath,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run directory contains a symbolic link: %1")
                               .arg(QDir(sourcePath).relativeFilePath(info.filePath())));
        }
        if (info.isDir()) {
            continue;
        }
        if (!info.isFile()) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run directory contains a non-regular file: %1")
                               .arg(QDir(sourcePath).relativeFilePath(info.filePath())));
        }

        ArchiveEntry entry;
        entry.path = QDir::fromNativeSeparators(QDir(sourcePath).relativeFilePath(info.filePath()));
        entry.encodedPath = entry.path.toUtf8();
        entry.absolutePath = info.absoluteFilePath();
        QString pathError;
        QString decodedPath;
        if (!validateEntryPath(entry.encodedPath, decodedPath, pathError)) {
            return failure(PackageError::InvalidRun, pathError);
        }
        if (entry.path.compare(QString::fromLatin1(packageManifestName), Qt::CaseInsensitive) == 0) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run directory uses reserved file name: %1")
                               .arg(QString::fromLatin1(packageManifestName)));
        }
        const QString pathKey = entry.path.toCaseFolded();
        if (caseFoldedPaths.contains(pathKey)) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run directory has paths that collide by case: %1").arg(entry.path));
        }
        caseFoldedPaths.insert(pathKey);
        exactPaths.insert(entry.path);
        PackageResult digestResult = digestFile(entry.absolutePath, entry);
        if (!digestResult) {
            return digestResult;
        }
        entries.append(std::move(entry));
    }

    for (const QString& required : requiredRunFiles()) {
        if (!exactPaths.contains(required)) {
            return failure(PackageError::InvalidRun,
                           QStringLiteral("Run directory is missing required file: %1").arg(required));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const ArchiveEntry& left, const ArchiveEntry& right) {
        return left.encodedPath < right.encodedPath;
    });
    if (entries.size() + 1 > kMaxArchiveEntries) {
        return failure(PackageError::Unsupported, QStringLiteral("Run contains too many files for this backend"));
    }

    const QByteArray manifestData = buildPackageManifest(entries);
    ArchiveEntry packageManifest;
    static_cast<FileDigest&>(packageManifest) = digestBytes(QString::fromLatin1(packageManifestName), manifestData);
    packageManifest.inlineData = manifestData;
    entries.append(std::move(packageManifest));

    const QString outputParent = QFileInfo(outputPath).absolutePath();
    if (!QDir().mkpath(outputParent)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot create archive directory: %1").arg(outputParent));
    }
    QSaveFile archive(outputPath);
    archive.setDirectWriteFallback(false);
    if (!archive.open(QIODevice::WriteOnly)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot create archive: %1").arg(archive.errorString()));
    }

    for (ArchiveEntry& entry : entries) {
        PackageResult writeResult = writeEntry(archive, entry);
        if (!writeResult) {
            archive.cancelWriting();
            return writeResult;
        }
    }
    if (archive.pos() < 0 || static_cast<quint64>(archive.pos()) >= kMaxZip32Value) {
        archive.cancelWriting();
        return failure(PackageError::Unsupported, QStringLiteral("ZIP64 is required for this archive"));
    }
    const quint32 centralOffset = static_cast<quint32>(archive.pos());
    for (const ArchiveEntry& entry : entries) {
        if (!writeAll(archive, centralHeader(entry))) {
            archive.cancelWriting();
            return failure(PackageError::Io,
                           QStringLiteral("Cannot write ZIP directory: %1").arg(archive.errorString()));
        }
    }
    const quint64 centralSize64 = static_cast<quint64>(archive.pos()) - centralOffset;
    if (centralSize64 >= kMaxZip32Value) {
        archive.cancelWriting();
        return failure(PackageError::Unsupported, QStringLiteral("ZIP64 is required for this archive"));
    }

    QByteArray endRecord;
    appendU32(endRecord, kEndOfCentralDirectorySignature);
    appendU16(endRecord, 0);
    appendU16(endRecord, 0);
    appendU16(endRecord, static_cast<quint16>(entries.size()));
    appendU16(endRecord, static_cast<quint16>(entries.size()));
    appendU32(endRecord, static_cast<quint32>(centralSize64));
    appendU32(endRecord, centralOffset);
    appendU16(endRecord, 0);
    if (!writeAll(archive, endRecord) || !archive.commit()) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot finalize archive: %1").arg(archive.errorString()));
    }

    PackageResult result;
    result.outputPath = outputPath;
    for (auto iterator = entries.cbegin(); iterator != entries.cend() - 1; ++iterator) {
        result.files.append(iterator->path);
        result.totalBytes += iterator->size;
    }
    return result;
}

namespace {

struct ManifestDigest {
    quint64 size = 0;
    QByteArray sha256;
};

struct ParsedArchive {
    QList<ZipEntry> entries;
    QHash<QString, ManifestDigest> expected;
};

PackageResult parseCentralDirectory(QFile& archive, QList<ZipEntry>& entries) {
    if (archive.size() < 22) {
        return failure(PackageError::InvalidArchive, QStringLiteral("File is too small to be a ZIP archive"));
    }
    const qint64 tailSize = std::min<qint64>(archive.size(), 22 + 65535);
    const qint64 tailStart = archive.size() - tailSize;
    if (!archive.seek(tailStart)) {
        return failure(PackageError::Io, QStringLiteral("Cannot seek in archive"));
    }
    QByteArray tail;
    if (!readExact(archive, tailSize, tail)) {
        return failure(PackageError::Io, QStringLiteral("Cannot read ZIP directory footer"));
    }

    qsizetype endOffsetInTail = -1;
    for (qsizetype offset = tail.size() - 22; offset >= 0; --offset) {
        if (readU32(tail, offset) != kEndOfCentralDirectorySignature) {
            continue;
        }
        const quint16 commentLength = readU16(tail, offset + 20);
        if (tailStart + offset + 22 + commentLength == archive.size()) {
            endOffsetInTail = offset;
            break;
        }
    }
    if (endOffsetInTail < 0) {
        return failure(PackageError::InvalidArchive, QStringLiteral("ZIP end record is missing or malformed"));
    }

    const QByteArray endRecord = tail.mid(endOffsetInTail, 22);
    const quint16 diskNumber = readU16(endRecord, 4);
    const quint16 centralDisk = readU16(endRecord, 6);
    const quint16 entriesOnDisk = readU16(endRecord, 8);
    const quint16 entryCount = readU16(endRecord, 10);
    const quint32 centralSize = readU32(endRecord, 12);
    const quint32 centralOffset = readU32(endRecord, 16);
    if (diskNumber != 0 || centralDisk != 0 || entriesOnDisk != entryCount) {
        return failure(PackageError::Unsupported, QStringLiteral("Multi-disk ZIP archives are not supported"));
    }
    if (entryCount == std::numeric_limits<quint16>::max()
        || centralSize == std::numeric_limits<quint32>::max()
        || centralOffset == std::numeric_limits<quint32>::max()) {
        return failure(PackageError::Unsupported, QStringLiteral("ZIP64 archives require the miniz backend"));
    }
    if (entryCount == 0 || entryCount > kMaxArchiveEntries || centralSize > kMaxCentralDirectorySize) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("ZIP central directory has an invalid size or entry count"));
    }
    const quint64 centralEnd = static_cast<quint64>(centralOffset) + centralSize;
    const quint64 endRecordOffset = static_cast<quint64>(tailStart + endOffsetInTail);
    if (centralEnd != endRecordOffset || centralEnd > static_cast<quint64>(archive.size())) {
        return failure(PackageError::InvalidArchive, QStringLiteral("ZIP central directory bounds are invalid"));
    }
    if (!archive.seek(centralOffset)) {
        return failure(PackageError::Io, QStringLiteral("Cannot seek to ZIP central directory"));
    }

    QSet<QString> pathKeys;
    entries.reserve(entryCount);
    for (quint16 index = 0; index < entryCount; ++index) {
        QByteArray header;
        if (!readExact(archive, 46, header)) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP central directory is truncated"));
        }
        if (readU32(header, 0) != kCentralHeaderSignature) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP central directory signature is invalid"));
        }
        const quint16 flags = readU16(header, 8);
        const quint16 method = readU16(header, 10);
        const quint32 crc = readU32(header, 16);
        const quint32 compressedSize = readU32(header, 20);
        const quint32 uncompressedSize = readU32(header, 24);
        const quint16 nameLength = readU16(header, 28);
        const quint16 extraLength = readU16(header, 30);
        const quint16 commentLength = readU16(header, 32);
        const quint16 startDisk = readU16(header, 34);
        const quint32 externalAttributes = readU32(header, 38);
        const quint32 localOffset = readU32(header, 42);
        if (nameLength == 0 || nameLength > 4096) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP entry path length is invalid"));
        }
        if ((flags & ~kUtf8Flag) != 0U) {
            return failure(PackageError::Unsupported,
                           QStringLiteral("Encrypted or data-descriptor ZIP entries are not supported"));
        }
        if (method != kStoredMethod) {
            return failure(PackageError::Unsupported,
                           QStringLiteral("Compressed ZIP entries require the miniz backend"));
        }
        if (compressedSize == std::numeric_limits<quint32>::max()
            || uncompressedSize == std::numeric_limits<quint32>::max()
            || localOffset == std::numeric_limits<quint32>::max()) {
            return failure(PackageError::Unsupported,
                           QStringLiteral("ZIP64 entries require the miniz backend"));
        }
        if (compressedSize != uncompressedSize || startDisk != 0) {
            return failure(PackageError::InvalidArchive, QStringLiteral("Stored ZIP entry metadata is inconsistent"));
        }

        QByteArray encodedPath;
        if (!readExact(archive, nameLength, encodedPath)) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP entry path is truncated"));
        }
        if (static_cast<quint64>(archive.pos()) + extraLength + commentLength > centralEnd
            || !archive.seek(archive.pos() + extraLength + commentLength)) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP entry metadata exceeds directory bounds"));
        }

        ZipEntry entry;
        QString pathError;
        if (!validateEntryPath(encodedPath, entry.path, pathError)) {
            return failure(PackageError::InvalidArchive, pathError);
        }
        const QString pathKey = entry.path.toCaseFolded();
        if (pathKeys.contains(pathKey)) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP contains duplicate or case-colliding path: %1").arg(entry.path));
        }
        pathKeys.insert(pathKey);

        const quint32 unixMode = externalAttributes >> 16U;
        if ((unixMode & 0170000U) == 0120000U) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP symbolic links are not allowed: %1").arg(entry.path));
        }
        entry.encodedPath = encodedPath;
        entry.flags = flags;
        entry.method = method;
        entry.crc32 = crc;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.localHeaderOffset = localOffset;
        entry.externalAttributes = externalAttributes;
        entries.append(std::move(entry));
    }
    if (static_cast<quint64>(archive.pos()) != centralEnd) {
        return failure(PackageError::InvalidArchive, QStringLiteral("ZIP central directory size is inconsistent"));
    }

    struct Span {
        quint64 begin = 0;
        quint64 end = 0;
    };
    QList<Span> spans;
    spans.reserve(entries.size());
    for (ZipEntry& entry : entries) {
        if (!archive.seek(entry.localHeaderOffset)) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP local header offset is invalid: %1").arg(entry.path));
        }
        QByteArray local;
        if (!readExact(archive, 30, local) || readU32(local, 0) != kLocalHeaderSignature) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP local header is missing: %1").arg(entry.path));
        }
        const quint16 localFlags = readU16(local, 6);
        const quint16 localMethod = readU16(local, 8);
        const quint32 localCrc = readU32(local, 14);
        const quint32 localCompressedSize = readU32(local, 18);
        const quint32 localUncompressedSize = readU32(local, 22);
        const quint16 localNameLength = readU16(local, 26);
        const quint16 localExtraLength = readU16(local, 28);
        if (localFlags != entry.flags || localMethod != entry.method || localCrc != entry.crc32
            || localCompressedSize != entry.compressedSize
            || localUncompressedSize != entry.uncompressedSize) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP local and central metadata differ: %1").arg(entry.path));
        }
        QByteArray localName;
        if (!readExact(archive, localNameLength, localName) || localName != entry.encodedPath) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP local entry path differs from central directory"));
        }
        const quint64 dataOffset = static_cast<quint64>(entry.localHeaderOffset) + 30U
            + localNameLength + localExtraLength;
        const quint64 dataEnd = dataOffset + entry.compressedSize;
        if (dataOffset < entry.localHeaderOffset || dataEnd < dataOffset || dataEnd > centralOffset) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("ZIP entry data bounds are invalid: %1").arg(entry.path));
        }
        entry.dataOffset = dataOffset;
        spans.append({entry.localHeaderOffset, dataEnd});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& left, const Span& right) {
        return left.begin < right.begin;
    });
    for (qsizetype index = 1; index < spans.size(); ++index) {
        if (spans.at(index).begin < spans.at(index - 1).end) {
            return failure(PackageError::InvalidArchive, QStringLiteral("ZIP entries overlap"));
        }
    }
    return {};
}

PackageResult readEntryBytes(QFile& archive, const ZipEntry& entry, quint64 maximumSize,
                             QByteArray& data) {
    if (entry.uncompressedSize > maximumSize || entry.uncompressedSize > std::numeric_limits<qsizetype>::max()) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Archive metadata file is unexpectedly large: %1").arg(entry.path));
    }
    if (!archive.seek(static_cast<qint64>(entry.dataOffset))) {
        return failure(PackageError::Io, QStringLiteral("Cannot seek to archive entry: %1").arg(entry.path));
    }
    if (!readExact(archive, static_cast<qint64>(entry.uncompressedSize), data)) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Archive entry is truncated: %1").arg(entry.path));
    }
    Crc32 crc;
    crc.add(data);
    if (crc.value() != entry.crc32) {
        return failure(PackageError::Integrity,
                       QStringLiteral("ZIP CRC32 check failed: %1").arg(entry.path));
    }
    return {};
}

PackageResult parsePackageManifest(QFile& archive, const QList<ZipEntry>& entries,
                                   QHash<QString, ManifestDigest>& expected) {
    const QString manifestName = QString::fromLatin1(RunPackage::packageManifestName);
    const auto manifestIterator = std::find_if(entries.cbegin(), entries.cend(), [&](const ZipEntry& entry) {
        return entry.path == manifestName;
    });
    if (manifestIterator == entries.cend()) {
        return failure(PackageError::InvalidArchive, QStringLiteral("Package manifest is missing"));
    }

    QByteArray manifestData;
    PackageResult readResult = readEntryBytes(archive, *manifestIterator, 4 * 1024 * 1024, manifestData);
    if (!readResult) {
        return readResult;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Package manifest is not valid JSON: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString() != QString::fromLatin1(RunPackage::packageSchema)) {
        return failure(PackageError::Unsupported, QStringLiteral("Package schema major version is unsupported"));
    }
    if (!root.value(QStringLiteral("files")).isArray()) {
        return failure(PackageError::InvalidArchive, QStringLiteral("Package manifest files list is missing"));
    }

    QSet<QString> expectedKeys;
    for (const QJsonValue value : root.value(QStringLiteral("files")).toArray()) {
        if (!value.isObject()) {
            return failure(PackageError::InvalidArchive, QStringLiteral("Package file record is not an object"));
        }
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        const qint64 size = object.value(QStringLiteral("size")).toInteger(-1);
        const QString digest = object.value(QStringLiteral("sha256")).toString();
        QString decodedPath;
        QString pathError;
        if (!validateEntryPath(path.toUtf8(), decodedPath, pathError) || decodedPath != path
            || path.compare(manifestName, Qt::CaseInsensitive) == 0) {
            return failure(PackageError::InvalidArchive,
                           pathError.isEmpty() ? QStringLiteral("Package manifest lists a reserved path") : pathError);
        }
        if (size < 0 || !isHexDigest(digest)) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("Package file size or SHA-256 is invalid: %1").arg(path));
        }
        const QString key = path.toCaseFolded();
        if (expectedKeys.contains(key)) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("Package manifest contains duplicate path: %1").arg(path));
        }
        expectedKeys.insert(key);
        expected.insert(path, {static_cast<quint64>(size), QByteArray::fromHex(digest.toLatin1())});
    }

    if (expected.size() + 1 != entries.size()) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Package manifest does not cover every ZIP entry"));
    }
    QSet<QString> actualPaths;
    for (const ZipEntry& entry : entries) {
        actualPaths.insert(entry.path);
        if (entry.path == manifestName) {
            continue;
        }
        const auto digestIterator = expected.constFind(entry.path);
        if (digestIterator == expected.cend() || digestIterator->size != entry.uncompressedSize) {
            return failure(PackageError::Integrity,
                           QStringLiteral("Package manifest size does not match ZIP entry: %1").arg(entry.path));
        }
    }
    for (auto iterator = expected.cbegin(); iterator != expected.cend(); ++iterator) {
        if (!actualPaths.contains(iterator.key())) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("Package manifest references a missing ZIP entry: %1").arg(iterator.key()));
        }
    }
    for (const QString& required : requiredRunFiles()) {
        if (!actualPaths.contains(required)) {
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("Package is missing required run file: %1").arg(required));
        }
    }
    return {};
}

PackageResult parseArchive(QFile& archive, ParsedArchive& parsed) {
    PackageResult directoryResult = parseCentralDirectory(archive, parsed.entries);
    if (!directoryResult) {
        return directoryResult;
    }
    return parsePackageManifest(archive, parsed.entries, parsed.expected);
}

PackageResult verifyEntry(QFile& archive, const ZipEntry& entry, const ManifestDigest& expected,
                          const QString& extractionRoot, QByteArray* capture) {
    if (!archive.seek(static_cast<qint64>(entry.dataOffset))) {
        return failure(PackageError::Io, QStringLiteral("Cannot seek to archive entry: %1").arg(entry.path));
    }

    std::unique_ptr<QSaveFile> output;
    if (!extractionRoot.isEmpty()) {
        const QString outputPath = QDir(extractionRoot).filePath(entry.path);
        const QString outputParent = QFileInfo(outputPath).absolutePath();
        if (!QDir().mkpath(outputParent)) {
            return failure(PackageError::Io,
                           QStringLiteral("Cannot create extraction directory: %1").arg(outputParent));
        }
        output = std::make_unique<QSaveFile>(outputPath);
        output->setDirectWriteFallback(false);
        if (!output->open(QIODevice::WriteOnly)) {
            return failure(PackageError::Io,
                           QStringLiteral("Cannot create extracted file: %1").arg(output->errorString()));
        }
    }

    Crc32 crc;
    QCryptographicHash sha(QCryptographicHash::Sha256);
    quint64 remaining = entry.uncompressedSize;
    if (capture != nullptr) {
        capture->clear();
        capture->reserve(static_cast<qsizetype>(entry.uncompressedSize));
    }
    while (remaining > 0) {
        const qint64 requested = static_cast<qint64>(std::min<quint64>(remaining, kCopyBufferSize));
        const QByteArray chunk = archive.read(requested);
        if (chunk.size() != requested) {
            if (output) {
                output->cancelWriting();
            }
            return failure(PackageError::InvalidArchive,
                           QStringLiteral("Archive entry is truncated: %1").arg(entry.path));
        }
        if (output && !writeAll(*output, chunk)) {
            output->cancelWriting();
            return failure(PackageError::Io,
                           QStringLiteral("Cannot write extracted file: %1").arg(output->errorString()));
        }
        if (capture != nullptr) {
            capture->append(chunk);
        }
        crc.add(chunk);
        sha.addData(chunk);
        remaining -= static_cast<quint64>(chunk.size());
    }
    if (crc.value() != entry.crc32 || expected.size != entry.uncompressedSize
        || sha.result() != expected.sha256) {
        if (output) {
            output->cancelWriting();
        }
        return failure(PackageError::Integrity,
                       QStringLiteral("Package integrity check failed: %1").arg(entry.path));
    }
    if (output && !output->commit()) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot finalize extracted file: %1").arg(output->errorString()));
    }
    return {};
}

PackageResult processArchive(const QString& archivePath, const QString& extractionRoot) {
    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.exists() || !archiveInfo.isFile() || archiveInfo.isSymLink()) {
        return failure(PackageError::InvalidArgument,
                       QStringLiteral("Archive does not exist or is not a regular file"));
    }
    QFile archive(archiveInfo.absoluteFilePath());
    if (!archive.open(QIODevice::ReadOnly)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot open archive: %1").arg(archive.errorString()));
    }

    ParsedArchive parsed;
    PackageResult parseResult = parseArchive(archive, parsed);
    if (!parseResult) {
        return parseResult;
    }

    QByteArray runManifestData;
    PackageResult result;
    result.outputPath = extractionRoot.isEmpty() ? archiveInfo.absoluteFilePath() : extractionRoot;
    for (const ZipEntry& entry : parsed.entries) {
        if (entry.path == QString::fromLatin1(RunPackage::packageManifestName)) {
            continue;
        }
        const ManifestDigest digest = parsed.expected.value(entry.path);
        QByteArray* capture = nullptr;
        if (entry.path == QStringLiteral("manifest.json")) {
            if (entry.uncompressedSize > kMaxMetadataSize) {
                return failure(PackageError::InvalidArchive, QStringLiteral("Run manifest is unexpectedly large"));
            }
            capture = &runManifestData;
        }
        PackageResult verifyResult = verifyEntry(archive, entry, digest, extractionRoot, capture);
        if (!verifyResult) {
            return verifyResult;
        }
        result.files.append(entry.path);
        result.totalBytes += entry.uncompressedSize;
    }

    QJsonParseError parseError;
    const QJsonDocument runManifest = QJsonDocument::fromJson(runManifestData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !runManifest.isObject()) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Packaged run manifest is invalid JSON: %1").arg(parseError.errorString()));
    }
    const QString status = runManifest.object().value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("completed") && status != QStringLiteral("interrupted")
        && status != QStringLiteral("recovered")) {
        return failure(PackageError::InvalidArchive,
                       QStringLiteral("Packaged run is still recording or has an invalid status"));
    }
    return result;
}

bool removeStagingDirectory(const QString& stagingPath, const QString& parentPath) {
    const QString cleanStaging = QDir::cleanPath(stagingPath);
    const QString cleanParent = QDir::cleanPath(parentPath);
    if (!pathIsInside(cleanStaging, cleanParent)) {
        return false;
    }
    return QDir(cleanStaging).removeRecursively();
}

} // namespace

PackageResult RunPackage::validate(const QString& archivePath) {
    if (archivePath.trimmed().isEmpty()) {
        return failure(PackageError::InvalidArgument, QStringLiteral("Archive path is required"));
    }
    return processArchive(archivePath, {});
}

PackageResult RunPackage::unpack(const QString& archivePath, const QString& destinationDirectory) {
    if (archivePath.trimmed().isEmpty() || destinationDirectory.trimmed().isEmpty()) {
        return failure(PackageError::InvalidArgument,
                       QStringLiteral("Archive path and destination directory are required"));
    }
    const QString destinationPath = QDir::cleanPath(QFileInfo(destinationDirectory).absoluteFilePath());
    if (QFileInfo::exists(destinationPath)) {
        return failure(PackageError::DestinationExists,
                       QStringLiteral("Destination already exists: %1").arg(destinationPath));
    }
    const QFileInfo destinationInfo(destinationPath);
    const QString parentPath = destinationInfo.absolutePath();
    const QString destinationName = destinationInfo.fileName();
    if (destinationName.isEmpty() || destinationName == QStringLiteral(".")
        || destinationName == QStringLiteral("..")) {
        return failure(PackageError::InvalidArgument, QStringLiteral("Destination directory is invalid"));
    }
    if (!QDir().mkpath(parentPath)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot create destination parent: %1").arg(parentPath));
    }

    const QString stagingName = QStringLiteral(".%1.akrion-unpack-%2")
                                    .arg(destinationName,
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingPath = QDir(parentPath).filePath(stagingName);
    if (QFileInfo::exists(stagingPath) || !QDir().mkpath(stagingPath)) {
        return failure(PackageError::Io,
                       QStringLiteral("Cannot create extraction staging directory"));
    }

    PackageResult result = processArchive(archivePath, stagingPath);
    if (!result) {
        removeStagingDirectory(stagingPath, parentPath);
        return result;
    }
    if (QFileInfo::exists(destinationPath)
        || !QDir(parentPath).rename(stagingName, destinationName)) {
        removeStagingDirectory(stagingPath, parentPath);
        return failure(PackageError::Io,
                       QStringLiteral("Cannot atomically publish extracted run: %1").arg(destinationPath));
    }
    result.outputPath = destinationPath;
    return result;
}

} // namespace akrion
