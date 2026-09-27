#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QList>
#include <QPair>

// One account-scoped durable journal per play. No credentials are persisted.
// Acknowledgement removes only the exact immutable part that was uploaded.
struct Nax5QueuedReportPart
{
    QString path;
    QString session_id;
    QString kind;
    QByteArray archive;
    QString client_version;
    QString client_sha;
};

QString nax5ReportQueueRoot();
QString nax5BeginReport(const QString &root, qint64 owner, const QString &session_id,
    const QString &build_info, const QList<QPair<QString, qint64>> &sources);
bool nax5AddReportSource(const QString &root, const QString &id, const QString &path, qint64 offset = 0);
bool nax5FinishReport(const QString &root, const QString &id, const QString &kind, const QString &build_info);
// Bounded work: at most max_parts * 256 KiB of source data per call.
bool nax5CaptureReports(const QString &root, qint64 owner, int max_parts = 4);
void nax5RecoverReports(const QString &root, qint64 owner, const QString &active_id = QString());
// Turns crash files from nax5InstallCrashHandler (and WER LocalDumps) into
// uploadable "crash" parts, then deletes them. Returns the number queued.
int nax5QueueCrashDumps(const QString &root, qint64 owner, const QString &dump_dir, const QString &build_info);
Nax5QueuedReportPart nax5NextReportPart(const QString &root, qint64 owner);
bool nax5AcknowledgeReportPart(const Nax5QueuedReportPart &part);
bool nax5ReportQueueHasWork(const QString &root, qint64 owner);
