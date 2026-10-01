#include "nax5/nax5reportqueue.h"
#include "nax5/nax5clientreport.h"
#include "sessionlog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUuid>

namespace {
QRecursiveMutex queue_mutex;
constexpr int chunk_bytes = 256 * 1024;
constexpr int live_chunk_min_bytes = 128 * 1024;
// Stop capturing, never evict unacknowledged data, when disk backlog exceeds this.
constexpr qint64 spool_limit = 512LL * 1024 * 1024;
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString journalPath(const QString &root, const QString &id)
{
    if (QUuid(id).isNull()) return QString();
    return QDir(root).filePath(id + QStringLiteral(".journal"));
}
QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool saveObject(const QString &path, const QJsonObject &object)
{
    if (path.isEmpty()) return false;
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QJsonObject source(const QString &path, qint64 offset)
{
    return {{"path", path}, {"initial_offset", double(offset)}, {"offset", double(offset)}, {"end", -1}};
}
bool enqueue(const QString &root, QJsonObject &journal, QJsonObject manifest, const QByteArray &data)
{
    const auto seq = journal.value("sequence").toInt();
    const QString id = journal.value("report_id").toString();
    const QString part_id = id + QStringLiteral("-%1").arg(seq, 8, 10, QLatin1Char('0'));
    manifest.insert("schema", 2);
    manifest.insert("report_id", id);
    manifest.insert("part_id", part_id);
    manifest.insert("sequence", seq);
    manifest.insert("session_id", journal.value("session_id"));
    manifest.insert("created_utc", now());
    manifest.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    const QByteArray zip = nax5ZipBytes({
        {QStringLiteral("BUILD-INFO"), journal.value("build_info").toString().toUtf8()},
        {QStringLiteral("MANIFEST.json"), QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
        {QStringLiteral("log-part.txt"), data}});
    if (zip.size() > nax5ClientReportMaxArchiveBytes()) return false;
    QJsonObject envelope{{"owner", journal.value("owner")}, {"session_id", journal.value("session_id")},
        {"report_id", id}, {"sequence", seq},
        {"client_version", journal.value("client_version")}, {"client_sha", journal.value("client_sha")},
        {"kind", journal.value("kind")}, {"archive", QString::fromLatin1(zip.toBase64())}};
    if (!saveObject(QDir(root).filePath(part_id + QStringLiteral(".part")), envelope)) return false;
    journal.insert("sequence", seq + 1);
    return true;
}
}

QString nax5ReportQueueRoot()
{
    const QString base = GetLogBaseDir();
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("report-queue-v2"));
}

QString nax5BeginReport(const QString &root, qint64 owner, const QString &session_id,
    const QString &build_info, const QList<QPair<QString, qint64>> &sources)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0 || !QDir().mkpath(root)) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonArray entries;
    for (const auto &item : sources)
        if (!item.first.isEmpty()) entries.append(source(item.first, item.second));
    QJsonObject journal{{"schema", 2}, {"report_id", id}, {"owner", double(owner)},
        {"client_version", nax5ClientVersion()}, {"client_sha", nax5ClientSha()},
        {"session_id", session_id}, {"kind", "automatic"}, {"created_utc", now()},
        {"build_info", build_info}, {"sources", entries}, {"sequence", 0}, {"finished", false}};
    if (!saveObject(journalPath(root, id), journal)) return {};
    return id;
}

bool nax5AddReportSource(const QString &root, const QString &id, const QString &path, qint64 offset)
{
    QMutexLocker lock(&queue_mutex);
    auto journal = readObject(journalPath(root, id));
    if (journal.isEmpty() || path.isEmpty()) return false;
    auto entries = journal.value("sources").toArray();
    for (const auto &item : entries)
        if (item.toObject().value("path").toString() == path) return true;
    entries.append(source(path, offset));
    journal.insert("sources", entries);
    return saveObject(journalPath(root, id), journal);
}

bool nax5FinishReport(const QString &root, const QString &id, const QString &kind, const QString &build_info)
{
    QMutexLocker lock(&queue_mutex);
    const auto path = journalPath(root, id);
    auto journal = readObject(path);
    if (journal.isEmpty()) return false;
    // Final source sizes are immutable: later sessions cannot enter this report.
    if (!journal.value("finished").toBool())
    {
        auto entries = journal.value("sources").toArray();
        for (int i = 0; i < entries.size(); ++i)
        {
            auto entry = entries[i].toObject();
            const QFileInfo info(entry.value("path").toString());
            entry.insert("end", double(info.exists() ? info.size() : entry.value("offset").toDouble()));
            if (!info.exists()) entry.insert("missing", true);
            entries[i] = entry;
        }
        journal.insert("sources", entries);
        journal.insert("finished", true);
        journal.insert("kind", kind);
        journal.insert("build_info", build_info);
        journal.insert("finished_utc", now());
    }
    return saveObject(path, journal);
}

void nax5RecoverReports(const QString &root, qint64 owner, const QString &active_id)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0) return;
    for (const auto &name : QDir(root).entryList({QStringLiteral("*.journal")}, QDir::Files))
    {
        const auto journal = readObject(QDir(root).filePath(name));
        if (journal.value("owner").toDouble() != owner || journal.value("finished").toBool()
            || journal.value("report_id").toString() == active_id) continue;
        nax5FinishReport(root, journal.value("report_id").toString(), QStringLiteral("crash"),
            journal.value("build_info").toString() + QStringLiteral("recovered_after_unclean_exit=true\n"));
    }
}

bool nax5CaptureReports(const QString &root, qint64 owner, int max_parts)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0) return false;
    qint64 used = 0;
    for (const auto &info : QDir(root).entryInfoList({QStringLiteral("*.part")}, QDir::Files)) used += info.size();
    for (const auto &name : QDir(root).entryList({QStringLiteral("*.journal")}, QDir::Files, QDir::Time | QDir::Reversed))
    {
        const QString path = QDir(root).filePath(name);
        auto journal = readObject(path);
        if (journal.value("owner").toDouble() != owner) continue;
        auto entries = journal.value("sources").toArray();
        bool caught_up = true;
        for (int i = 0; i < entries.size(); ++i)
        {
            auto entry = entries[i].toObject();
            QFile file(entry.value("path").toString());
            if (!file.open(QIODevice::ReadOnly))
            {
                entry.insert("missing", true);
                entries[i] = entry;
                continue;
            }
            const qint64 offset = qint64(entry.value("offset").toDouble());
            const qint64 limit = journal.value("finished").toBool()
                ? qint64(entry.value("end").toDouble()) : file.size();
            if (file.size() < offset) { entry.insert("truncated", true); entries[i] = entry; continue; }
            if (offset >= limit) continue;
            if (!journal.value("finished").toBool() && limit - offset < live_chunk_min_bytes)
            {
                caught_up = false;
                continue;
            }
            if (max_parts <= 0) return true;
            if (used + 2 * chunk_bytes > spool_limit) return false;
            if (!file.seek(offset)) return false;
            QByteArray raw = file.read(qMin<qint64>(chunk_bytes, limit - offset));
            if (raw.isEmpty()) return false;
            // Keep complete lines so a secret cannot straddle two sanitization calls.
            if (offset + raw.size() < limit || !journal.value("finished").toBool())
            {
                const int newline = raw.lastIndexOf('\n');
                if (newline >= 0) raw.truncate(newline + 1);
                else if (raw.size() < chunk_bytes) { caught_up = false; continue; }
                else return false; // Retain source, never silently truncate a pathological line.
            }
            const QByteArray sanitized = nax5SanitizeProcessLogLine(QString::fromUtf8(raw)).toUtf8();
            QJsonObject manifest{{"source", QFileInfo(file).fileName()}, {"source_offset", double(offset)},
                {"source_bytes", raw.size()}, {"sanitized", true}, {"final", false}};
            if (!enqueue(root, journal, manifest, sanitized)) return false;
            entry.insert("offset", double(offset + raw.size()));
            entries[i] = entry;
            journal.insert("sources", entries);
            // If this write fails the immutable part may be replayed, never lost.
            if (!saveObject(path, journal)) return false;
            used += 2 * chunk_bytes;
            --max_parts;
            if (offset + raw.size() < limit) caught_up = false;
        }
        if (caught_up && journal.value("finished").toBool() && max_parts > 0)
        {
            QJsonArray summary;
            for (auto item : entries)
            {
                auto entry = item.toObject();
                entry.insert("source", QFileInfo(entry.take("path").toString()).fileName());
                summary.append(entry);
            }
            if (!enqueue(root, journal, {{"final", true}, {"sources", summary},
                {"finished_utc", journal.value("finished_utc")}}, {})) return false;
            if (!QFile::remove(path)) return false;
            --max_parts;
        }
    }
    return true;
}

Nax5QueuedReportPart nax5NextReportPart(const QString &root, qint64 owner)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0) return {};
    for (const auto &name : QDir(root).entryList({QStringLiteral("*.part")}, QDir::Files, QDir::Time | QDir::Reversed))
    {
        const QString path = QDir(root).filePath(name);
        const auto object = readObject(path);
        if (object.value("owner").toDouble() != owner) continue;
        const QString journal_path = journalPath(root, object.value("report_id").toString());
        if (journal_path.isEmpty()) continue;
        if (QFile::exists(journal_path))
        {
            const auto journal = readObject(journal_path);
            // Do not upload a part until its source cursor is durably committed.
            if (journal.isEmpty() || object.value("sequence").toInt() >= journal.value("sequence").toInt()) continue;
        }
        const auto archive = QByteArray::fromBase64(object.value("archive").toString().toLatin1());
        if (!archive.startsWith("PK\x03\x04") || archive.size() > nax5ClientReportMaxArchiveBytes()) continue;
        return {path, object.value("session_id").toString(), object.value("kind").toString(), archive,
            object.value("client_version").toString(), object.value("client_sha").toString()};
    }
    return {};
}

int nax5QueueCrashDumps(const QString &root, qint64 owner, const QString &dump_dir, const QString &build_info)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0 || dump_dir.isEmpty() || !QDir().mkpath(root)) return 0;
    QDir dir(dump_dir);
    if (!dir.exists()) return 0;

    // Attribute each crash to the latest play journal that started before it.
    QList<QPair<QDateTime, QString>> plays;
    for (const auto &name : QDir(root).entryList({QStringLiteral("*.journal")}, QDir::Files))
    {
        const auto journal = readObject(QDir(root).filePath(name));
        if (journal.value("owner").toDouble() != owner) continue;
        plays.append({QDateTime::fromString(journal.value("created_utc").toString(), Qt::ISODateWithMs),
            journal.value("session_id").toString()});
    }

    int queued = 0;
    const auto dumps = dir.entryInfoList({QStringLiteral("*.dmp"), QStringLiteral("*.txt")}, QDir::Files, QDir::Time | QDir::Reversed);
    QStringList bases;
    for (const auto &info : dumps)
        if (!bases.contains(info.completeBaseName())) bases.append(info.completeBaseName());
    // WER LocalDumps names its file "<exe>.<pid>.dmp"; drop it when the in-app
    // handler already captured the same process ("NAX5-<utc>-<pid>").
    QSet<QString> handled_pids;
    for (const auto &base : bases)
        if (base.startsWith(QStringLiteral("NAX5-"))) handled_pids.insert(base.section(QLatin1Char('-'), -1));
    for (const auto &base : bases)
    {
        if (!base.startsWith(QStringLiteral("NAX5-")) && handled_pids.contains(base.section(QLatin1Char('.'), -1)))
        {
            QFile::remove(dir.filePath(base + QStringLiteral(".dmp")));
            continue;
        }
        const QString dump_path = dir.filePath(base + QStringLiteral(".dmp"));
        const QString summary_path = dir.filePath(base + QStringLiteral(".txt"));
        const QFileInfo marker(QFile::exists(dump_path) ? dump_path : summary_path);
        // Skip files a crashing process may still be writing.
        if (marker.lastModified().secsTo(QDateTime::currentDateTime()) < 5) continue;

        QByteArray dump, summary;
        if (QFile file(dump_path); file.open(QIODevice::ReadOnly)) dump = file.readAll();
        if (QFile file(summary_path); file.open(QIODevice::ReadOnly)) summary = file.read(64 * 1024);

        QString session_id;
        QDateTime best;
        const QDateTime crashed_at = marker.lastModified().toUTC();
        for (const auto &play : plays)
            if (play.first.isValid() && play.first <= crashed_at && (!best.isValid() || play.first > best))
            {
                best = play.first;
                session_id = play.second;
            }

        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject manifest{{"schema", 2}, {"report_id", id}, {"part_id", id + QStringLiteral("-00000000")},
            {"sequence", 0}, {"session_id", session_id}, {"created_utc", now()}, {"final", true},
            {"crash_dump", base}, {"dump_bytes", double(dump.size())},
            {"crash_utc", crashed_at.toString(Qt::ISODate)}};
        QList<QPair<QString, QByteArray>> files{
            {QStringLiteral("BUILD-INFO"), build_info.toUtf8()},
            {QStringLiteral("crash-summary.txt"), summary}};
        QByteArray zip;
        if (!dump.isEmpty())
        {
            manifest.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(dump, QCryptographicHash::Sha256).toHex()));
            auto with_dump = files;
            with_dump.prepend({QStringLiteral("MANIFEST.json"), QJsonDocument(manifest).toJson(QJsonDocument::Compact)});
            with_dump.append({base + QStringLiteral(".dmp"), dump});
            zip = nax5ZipBytes(with_dump);
        }
        if (zip.isEmpty() || zip.size() > nax5ClientReportMaxArchiveBytes())
        {
            // Never drop the crash: the summary alone still names the faulting module.
            manifest.insert("dump_omitted", dump.isEmpty() ? QStringLiteral("missing") : QStringLiteral("too_large"));
            files.prepend({QStringLiteral("MANIFEST.json"), QJsonDocument(manifest).toJson(QJsonDocument::Compact)});
            zip = nax5ZipBytes(files);
        }
        if (zip.size() > nax5ClientReportMaxArchiveBytes()) continue;

        const QJsonObject envelope{{"owner", double(owner)}, {"session_id", session_id}, {"report_id", id},
            {"sequence", 0}, {"client_version", nax5ClientVersion()}, {"client_sha", nax5ClientSha()},
            {"kind", "crash"}, {"archive", QString::fromLatin1(zip.toBase64())}};
        if (!saveObject(QDir(root).filePath(id + QStringLiteral("-00000000.part")), envelope)) break;
        QFile::remove(dump_path);
        QFile::remove(summary_path);
        ++queued;
    }
    return queued;
}

bool nax5AcknowledgeReportPart(const Nax5QueuedReportPart &part)
{
    QMutexLocker lock(&queue_mutex);
    return !part.path.isEmpty() && QFile::remove(part.path);
}

bool nax5ReportQueueHasWork(const QString &root, qint64 owner)
{
    QMutexLocker lock(&queue_mutex);
    if (root.isEmpty() || owner <= 0) return false;
    for (const auto &name : QDir(root).entryList({QStringLiteral("*.journal"), QStringLiteral("*.part")}, QDir::Files))
        if (readObject(QDir(root).filePath(name)).value("owner").toDouble() == owner) return true;
    return false;
}
