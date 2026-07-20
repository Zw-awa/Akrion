#pragma once

#include <QString>
#include <QStringList>

namespace akrion {

enum class PackageError {
    None,
    InvalidArgument,
    DestinationExists,
    Io,
    InvalidRun,
    InvalidArchive,
    Integrity,
    Unsupported,
};

struct PackageResult {
    PackageError error = PackageError::None;
    QString message;
    QString outputPath;
    QStringList files;
    quint64 totalBytes = 0;

    [[nodiscard]] bool ok() const noexcept { return error == PackageError::None; }
    explicit operator bool() const noexcept { return ok(); }
};

class RunPackage final {
public:
    static constexpr auto packageSchema = "akrion.package/1";
    static constexpr auto packageManifestName = "package-manifest.json";

    // The current self-contained backend writes standard ZIP archives using
    // method 0 (stored). The public API is independent from that backend so it
    // can be replaced by miniz without changing CLI or core callers.
    [[nodiscard]] static PackageResult pack(const QString& runDirectory,
                                            const QString& archivePath);
    [[nodiscard]] static PackageResult validate(const QString& archivePath);
    [[nodiscard]] static PackageResult unpack(const QString& archivePath,
                                              const QString& destinationDirectory);
};

} // namespace akrion
