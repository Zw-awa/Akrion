#include "package/runpackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QUuid>

#include <array>
#include <iostream>

namespace {

int failures = 0;

class TestDirectory final {
public:
    TestDirectory() {
        QString root = qEnvironmentVariable("AKRION_TEST_TMP");
        if (root.isEmpty()) {
            root = QDir::tempPath();
        }
        m_path = QDir(root).filePath(QStringLiteral("akrion-runpackage-test-%1")
                                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        m_valid = QDir().mkpath(m_path);
    }

    ~TestDirectory() {
        if (m_valid && !qEnvironmentVariableIsSet("AKRION_KEEP_TEST_ARTIFACTS")) {
            QDir(m_path).removeRecursively();
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return m_valid; }
    [[nodiscard]] QString path() const { return m_path; }

private:
    QString m_path;
    bool m_valid = false;
};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool writeFile(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        std::cerr << "Cannot create parent for " << qPrintable(path) << '\n';
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "Cannot open " << qPrintable(path) << ": "
                  << qPrintable(file.errorString()) << '\n';
        return false;
    }
    if (file.write(data) != data.size()) {
        std::cerr << "Cannot write " << qPrintable(path) << ": "
                  << qPrintable(file.errorString()) << '\n';
        return false;
    }
    return true;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString createRun(const QString& root, const QString& status = QStringLiteral("completed")) {
    const QString run = QDir(root).filePath(QStringLiteral("run-001"));
    QDir().mkpath(run);
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), QStringLiteral("akrion.run/1"));
    manifest.insert(QStringLiteral("run_id"), QStringLiteral("run-001"));
    manifest.insert(QStringLiteral("status"), status);
    writeFile(QDir(run).filePath(QStringLiteral("manifest.json")),
              QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    writeFile(QDir(run).filePath(QStringLiteral("config.json")), QByteArrayLiteral("{}\n"));
    writeFile(QDir(run).filePath(QStringLiteral("serial.raw")), QByteArray("raw\0bytes\n", 10));
    writeFile(QDir(run).filePath(QStringLiteral("frames.ndjson")),
              QByteArrayLiteral("{\"schema\":\"akrion.frame/1\"}\n"));
    writeFile(QDir(run).filePath(QStringLiteral("events.ndjson")), QByteArrayLiteral("\n"));
    writeFile(QDir(run).filePath(QStringLiteral("summary.json")), QByteArrayLiteral("{}\n"));
    writeFile(QDir(run).filePath(QStringLiteral("extensions/source.txt")), QByteArrayLiteral("extension\n"));
    return run;
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

quint32 crc32(const QByteArray& data) {
    static constexpr auto table = makeCrcTable();
    quint32 value = 0xffffffffU;
    for (const char byte : data) {
        value = table[static_cast<quint8>(value ^ static_cast<quint8>(byte))] ^ (value >> 8U);
    }
    return value ^ 0xffffffffU;
}

void appendU16(QByteArray& data, quint16 value) {
    data.append(static_cast<char>(value & 0xffU));
    data.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendU32(QByteArray& data, quint32 value) {
    data.append(static_cast<char>(value & 0xffU));
    data.append(static_cast<char>((value >> 8U) & 0xffU));
    data.append(static_cast<char>((value >> 16U) & 0xffU));
    data.append(static_cast<char>((value >> 24U) & 0xffU));
}

struct RawZipEntry {
    QByteArray name;
    QByteArray data;
    quint32 crc = 0;
    quint32 offset = 0;
};

bool writeStoredZip(const QString& path, const QMap<QString, QByteArray>& sourceEntries) {
    QList<RawZipEntry> entries;
    QByteArray archive;
    for (auto iterator = sourceEntries.cbegin(); iterator != sourceEntries.cend(); ++iterator) {
        RawZipEntry entry;
        entry.name = iterator.key().toUtf8();
        entry.data = iterator.value();
        entry.crc = crc32(entry.data);
        entry.offset = static_cast<quint32>(archive.size());
        appendU32(archive, 0x04034b50U);
        appendU16(archive, 20);
        appendU16(archive, 0x0800U);
        appendU16(archive, 0);
        appendU16(archive, 0);
        appendU16(archive, 0x0021U);
        appendU32(archive, entry.crc);
        appendU32(archive, static_cast<quint32>(entry.data.size()));
        appendU32(archive, static_cast<quint32>(entry.data.size()));
        appendU16(archive, static_cast<quint16>(entry.name.size()));
        appendU16(archive, 0);
        archive.append(entry.name);
        archive.append(entry.data);
        entries.append(entry);
    }

    const quint32 centralOffset = static_cast<quint32>(archive.size());
    for (const RawZipEntry& entry : entries) {
        appendU32(archive, 0x02014b50U);
        appendU16(archive, 20);
        appendU16(archive, 20);
        appendU16(archive, 0x0800U);
        appendU16(archive, 0);
        appendU16(archive, 0);
        appendU16(archive, 0x0021U);
        appendU32(archive, entry.crc);
        appendU32(archive, static_cast<quint32>(entry.data.size()));
        appendU32(archive, static_cast<quint32>(entry.data.size()));
        appendU16(archive, static_cast<quint16>(entry.name.size()));
        appendU16(archive, 0);
        appendU16(archive, 0);
        appendU16(archive, 0);
        appendU16(archive, 0);
        appendU32(archive, 0);
        appendU32(archive, entry.offset);
        archive.append(entry.name);
    }
    const quint32 centralSize = static_cast<quint32>(archive.size()) - centralOffset;
    appendU32(archive, 0x06054b50U);
    appendU16(archive, 0);
    appendU16(archive, 0);
    appendU16(archive, static_cast<quint16>(entries.size()));
    appendU16(archive, static_cast<quint16>(entries.size()));
    appendU32(archive, centralSize);
    appendU32(archive, centralOffset);
    appendU16(archive, 0);
    return writeFile(path, archive);
}

QMap<QString, QByteArray> packagedRunEntries(bool corruptSerialHash) {
    QMap<QString, QByteArray> entries;
    entries.insert(QStringLiteral("manifest.json"),
                   QByteArrayLiteral("{\"schema\":\"akrion.run/1\",\"status\":\"completed\"}\n"));
    entries.insert(QStringLiteral("config.json"), QByteArrayLiteral("{}\n"));
    entries.insert(QStringLiteral("serial.raw"), QByteArrayLiteral("raw-data"));
    entries.insert(QStringLiteral("frames.ndjson"), QByteArrayLiteral("\n"));
    entries.insert(QStringLiteral("events.ndjson"), QByteArrayLiteral("\n"));
    entries.insert(QStringLiteral("summary.json"), QByteArrayLiteral("{}\n"));

    QJsonArray files;
    for (auto iterator = entries.cbegin(); iterator != entries.cend(); ++iterator) {
        QByteArray digest = QCryptographicHash::hash(iterator.value(), QCryptographicHash::Sha256).toHex();
        if (corruptSerialHash && iterator.key() == QStringLiteral("serial.raw")) {
            digest = QByteArray(64, '0');
        }
        QJsonObject file;
        file.insert(QStringLiteral("path"), iterator.key());
        file.insert(QStringLiteral("size"), iterator.value().size());
        file.insert(QStringLiteral("sha256"), QString::fromLatin1(digest));
        files.append(file);
    }
    QJsonObject packageManifest;
    packageManifest.insert(QStringLiteral("schema"), QStringLiteral("akrion.package/1"));
    packageManifest.insert(QStringLiteral("format"), QStringLiteral("zip"));
    packageManifest.insert(QStringLiteral("compression"), QStringLiteral("store"));
    packageManifest.insert(QStringLiteral("files"), files);
    entries.insert(QStringLiteral("package-manifest.json"),
                   QJsonDocument(packageManifest).toJson(QJsonDocument::Indented));
    return entries;
}

void testRoundTrip() {
    TestDirectory temporary;
    check(temporary.isValid(), "temporary directory is available");
    const QString run = createRun(temporary.path());
    const QString archive = QDir(temporary.path()).filePath(QStringLiteral("run-001.akrion"));
    const QString unpacked = QDir(temporary.path()).filePath(QStringLiteral("unpacked"));

    const akrion::PackageResult packed = akrion::RunPackage::pack(run, archive);
    check(packed.ok(), "completed run can be packed");
    check(QFileInfo::exists(archive), "pack creates an archive");
    check(packed.files.size() == 7, "pack reports canonical and extension files");

    const akrion::PackageResult valid = akrion::RunPackage::validate(archive);
    check(valid.ok(), "fresh package validates");
    if (qEnvironmentVariableIsSet("AKRION_KEEP_TEST_ARTIFACTS")) {
        std::cout << "PACKAGE_FIXTURE=" << qPrintable(archive) << '\n';
    }
    const akrion::PackageResult extracted = akrion::RunPackage::unpack(archive, unpacked);
    check(extracted.ok(), "valid package can be unpacked");
    check(!QFileInfo::exists(QDir(unpacked).filePath(QStringLiteral("package-manifest.json"))),
          "package metadata is not copied into canonical run directory");

    const QStringList files {
        QStringLiteral("manifest.json"), QStringLiteral("config.json"),
        QStringLiteral("serial.raw"), QStringLiteral("frames.ndjson"),
        QStringLiteral("events.ndjson"), QStringLiteral("summary.json"),
        QStringLiteral("extensions/source.txt"),
    };
    for (const QString& file : files) {
        check(readFile(QDir(run).filePath(file)) == readFile(QDir(unpacked).filePath(file)),
              qPrintable(QStringLiteral("round trip preserves %1").arg(file)));
    }

    const akrion::PackageResult existing = akrion::RunPackage::unpack(archive, unpacked);
    check(existing.error == akrion::PackageError::DestinationExists,
          "unpack refuses to overwrite an existing directory");
}

void testRejectsRecordingAndNestedOutput() {
    TestDirectory temporary;
    const QString recordingRun = createRun(temporary.path(), QStringLiteral("recording"));
    const QString archive = QDir(temporary.path()).filePath(QStringLiteral("recording.akrion"));
    const akrion::PackageResult recording = akrion::RunPackage::pack(recordingRun, archive);
    check(recording.error == akrion::PackageError::InvalidRun,
          "pack refuses a run that is still recording");

    const QString completedRun = createRun(QDir(temporary.path()).filePath(QStringLiteral("completed-root")));
    const QString nestedArchive = QDir(completedRun).filePath(QStringLiteral("nested.akrion"));
    const akrion::PackageResult nested = akrion::RunPackage::pack(completedRun, nestedArchive);
    check(nested.error == akrion::PackageError::InvalidArgument,
          "pack refuses an output path inside the source run");
}

void testRejectsPathTraversal() {
    TestDirectory temporary;
    const QString archive = QDir(temporary.path()).filePath(QStringLiteral("traversal.akrion"));
    QMap<QString, QByteArray> entries;
    entries.insert(QStringLiteral("../escaped.txt"), QByteArrayLiteral("escape"));
    check(writeStoredZip(archive, entries), "malicious traversal ZIP fixture can be written");
    const akrion::PackageResult result = akrion::RunPackage::validate(archive);
    check(result.error == akrion::PackageError::InvalidArchive,
          "validate rejects parent-directory traversal");
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    const akrion::PackageResult unpacked = akrion::RunPackage::unpack(archive, destination);
    check(!unpacked.ok() && !QFileInfo::exists(destination),
          "failed traversal extraction leaves no destination directory");
    check(!QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral("escaped.txt"))),
          "traversal entry cannot escape staging directory");
}

void testRejectsShaMismatch() {
    TestDirectory temporary;
    const QString archive = QDir(temporary.path()).filePath(QStringLiteral("bad-sha.akrion"));
    check(writeStoredZip(archive, packagedRunEntries(true)), "bad SHA ZIP fixture can be written");
    const akrion::PackageResult result = akrion::RunPackage::validate(archive);
    check(result.error == akrion::PackageError::Integrity,
          "valid ZIP CRC cannot hide a package SHA-256 mismatch");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    testRoundTrip();
    testRejectsRecordingAndNestedOutput();
    testRejectsPathTraversal();
    testRejectsShaMismatch();
    if (failures == 0) {
        std::cout << "All RunPackage tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
