#ifndef REVSTUDIO_BACKENDLOCATOR_H
#define REVSTUDIO_BACKENDLOCATOR_H

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

namespace revstudio {

/** What `rb --server-info` reports. */
struct BackendInfo
{
    int         protocol = 0;
    QString     version;
    QStringList features;
};

/**
 * Finds the `rb` executable. Search order (GUI_Implementation_Note.md, 10.3):
 *   1. the explicit path (--rb): if given, it is the ONLY candidate, so a wrong path is an error, not a silent fallback
 *   2. the path saved in the settings
 *   3. the REVBAYES_EXECUTABLE environment variable
 *   4. next to the GUI executable ("rb" / "rb.exe")
 *   5. ../bin relative to the GUI executable
 *   6. PATH
 */
class BackendLocator
{
public:
    struct Candidate
    {
        QString path;
        QString origin;      // "--rb", "settings", "REVBAYES_EXECUTABLE", "next to the GUI", "../bin", "PATH"
    };

    BackendLocator(const QString& explicitPath,
                   const QString& settingsPath,
                   const QString& applicationDir,
                   const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment());

    /** All candidates in search order, whether or not they exist. */
    QList<Candidate> candidates() const;

    /** The first candidate that exists and is executable. */
    std::optional<Candidate> locate() const;

    /** File name of the backend on this platform: "rb" or "rb.exe". */
    static QString executableName();

    /** Runs `<path> --server-info` (blocking, with a timeout). Returns nullopt and fills *error on any problem. */
    static std::optional<BackendInfo> probe(const QString& path, QString* error = nullptr, int timeoutMs = 10000);

private:
    QString             explicitPath_;
    QString             settingsPath_;
    QString             applicationDir_;
    QProcessEnvironment environment_;
};

} // namespace revstudio

#endif
