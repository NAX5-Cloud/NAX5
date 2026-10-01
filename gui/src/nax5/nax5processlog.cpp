#include "nax5/nax5processlog.h"

#include "nax5/nax5networkhint.h"
#include "nax5/nax5runtime.h"
#include "sessionlog.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QOperatingSystemVersion>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <QtGlobal>

#ifndef CHIAKI_VERSION
#define CHIAKI_VERSION "unknown"
#endif
#ifndef NAX5_VERSION
#define NAX5_VERSION "unknown"
#endif
#ifndef NAX5_CLIENT_SHA
#define NAX5_CLIENT_SHA "unknown"
#endif

namespace {

const int kKeepLogFiles = 5;
QMutex g_mutex;
QString g_path;
bool g_started = false;

QString dateStamp()
{
    static const QString format = QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzzzzz");
    return QDateTime::currentDateTime().toString(format);
}

void rotateNax5Logs(const QDir &dir)
{
    static const QRegularExpression name_re(QStringLiteral("^nax5_(.*)\\.log$"));
    const QStringList existing = dir.entryList(QStringList() << QStringLiteral("nax5_*.log"), QDir::Files);
    QVector<QPair<QString, QDateTime>> dated;
    dated.reserve(existing.size());
    for (const QString &filename : existing)
    {
        QDateTime date;
        const QRegularExpressionMatch match = name_re.match(filename);
        if (match.hasMatch())
            date = QDateTime::fromString(match.captured(1), QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzzzzz"));
        dated.append(qMakePair(filename, date));
    }
    std::sort(dated.begin(), dated.end(), [](const QPair<QString, QDateTime> &a, const QPair<QString, QDateTime> &b) {
        return a.second > b.second;
    });
    for (int i = kKeepLogFiles; i < dated.size(); ++i)
    {
        if (!dated[i].second.isValid())
            break;
        QDir(dir).remove(dated[i].first);
    }
}

} // namespace

QByteArray nax5ReadFileTailBytes(const QString &path, int max_bytes)
{
    if (path.isEmpty() || max_bytes <= 0)
        return QByteArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    const qint64 size = file.size();
    if (size > max_bytes)
        file.seek(size - max_bytes);
    QByteArray bytes = file.readAll();
    if (size > max_bytes)
    {
        const int newline = bytes.indexOf('\n');
        if (newline >= 0 && newline + 1 < bytes.size())
            bytes = bytes.mid(newline + 1);
    }
    return bytes;
}

QString nax5SanitizeProcessLogLine(const QString &line)
{
    QString sanitized = line;
    static const QRegularExpression secret_re(
        QStringLiteral("(password|passwd|session[_-]?token|x-session-token|regist[_-]?key|morning|console[_-]?pin|\\bpin\\b)([\"'\\s:=]+)([^\\s,;\"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    sanitized.replace(secret_re, QStringLiteral("\\1\\2<redacted>"));
    sanitized.replace(QRegularExpression(QStringLiteral("X-Session-Token:\\s*\\S+"), QRegularExpression::CaseInsensitiveOption),
                      QStringLiteral("X-Session-Token: <redacted>"));
    static const QRegularExpression headers(QStringLiteral("(Authorization|Cookie|Set-Cookie):[^\\r\\n]*"), QRegularExpression::CaseInsensitiveOption);
    sanitized.replace(headers, QStringLiteral("\\1: <redacted>"));
    static const QRegularExpression identifiers(QStringLiteral("(duid|account[_ -]?id|psn[_ -]?id|rp[_ -]?key|rp[_ -]?registkey|access[_ -]?token|refresh[_ -]?token)([\"'\\s:=]+)([^\\s,;\"']+)"), QRegularExpression::CaseInsensitiveOption);
    sanitized.replace(identifiers, QStringLiteral("\\1\\2<redacted>"));
    static const QRegularExpression addresses(QStringLiteral("\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}\\b"));
    sanitized.replace(addresses, QStringLiteral("<redacted-ipv4>"));
    static const QRegularExpression hex(QStringLiteral("\\b[a-fA-F0-9]{32,}\\b"));
    sanitized.replace(hex, QStringLiteral("<redacted-hex>"));
    return sanitized;
}

QString nax5ClientVersion()
{
    return QStringLiteral(NAX5_VERSION);
}

QString nax5ClientSha()
{
    return QStringLiteral(NAX5_CLIENT_SHA);
}

QString nax5OsPlatform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}

QString nax5OsVersion()
{
    const QOperatingSystemVersion current = QOperatingSystemVersion::current();
#if defined(Q_OS_WIN)
    if (current >= QOperatingSystemVersion::Windows11)
        return QStringLiteral("Windows 11");
    return QStringLiteral("Windows %1").arg(current.majorVersion());
#else
    const QString name = current.name();
    if (!name.isEmpty())
        return name;
    return QStringLiteral("%1.%2").arg(current.majorVersion()).arg(current.minorVersion());
#endif
}

QString nax5OsBuild()
{
    const QOperatingSystemVersion current = QOperatingSystemVersion::current();
    return QStringLiteral("%1.%2.%3")
        .arg(current.majorVersion())
        .arg(current.minorVersion())
        .arg(current.microVersion());
}

QString nax5LocaleName()
{
    return QLocale::system().name();
}

namespace {

QString boolText(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

QString formatPacketLoss(double value)
{
    return QString::number(value, 'f', 6);
}

} // namespace

QString nax5BuildInfoText()
{
    return nax5BuildInfoText(Nax5BuildInfoSnapshot());
}

QString nax5BuildInfoText(const Nax5BuildInfoSnapshot &snapshot)
{
    QString text;
    text += QStringLiteral("client_version=%1\n").arg(nax5ClientVersion());
    text += QStringLiteral("client_sha=%1\n").arg(nax5ClientSha());
    text += QStringLiteral("chiaki_version=%1\n").arg(QStringLiteral(CHIAKI_VERSION));
    text += QStringLiteral("operator_mode=%1\n").arg(boolText(Nax5Runtime::operatorMode()));
    text += QStringLiteral("decoder=%1\n").arg(snapshot.decoder);
    text += QStringLiteral("resolution=%1\n").arg(snapshot.resolution);
    text += QStringLiteral("bitrate=%1\n").arg(snapshot.bitrate_kbps);
    text += QStringLiteral("fps=%1\n").arg(snapshot.fps);
    text += QStringLiteral("show_stream_stats=%1\n").arg(boolText(snapshot.show_stream_stats));
    text += QStringLiteral("log_verbose=%1\n").arg(boolText(snapshot.log_verbose));
    text += QStringLiteral("log_sanitize=%1\n").arg(boolText(snapshot.log_sanitize));
    text += QStringLiteral("os_platform=%1\n").arg(nax5OsPlatform());
    text += QStringLiteral("os_version=%1\n").arg(nax5OsVersion());
    text += QStringLiteral("os_build=%1\n").arg(nax5OsBuild());
    text += QStringLiteral("locale=%1\n").arg(nax5LocaleName());
    text += nax5NetworkHintText(nax5QueryNetworkHint());
    if (snapshot.has_stream_stats)
        text += QStringLiteral("avg_packet_loss=%1\n").arg(formatPacketLoss(snapshot.avg_packet_loss));
    else
        text += QStringLiteral("avg_packet_loss=\n");
    text += QStringLiteral("max_packet_loss=%1\n").arg(snapshot.max_packet_loss);
    text += QStringLiteral("packet_loss_semantics=session_mean_of_1hz_rolling_2s_fractions\n");
    text += QStringLiteral("max_packet_loss_semantics=max_observed_rolling_sample_not_all_packets\n");
    text += QStringLiteral("frames_lost_semantics=cumulative_receiver_frames\nrender_dropped_semantics=session_total_renderer_drops\nmeasured_bitrate_semantics=session_mean_kbps_of_measured_seconds\n");
    text += QStringLiteral("diagnostics_schema=4\nclock_utc=%1\n").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    text += QStringLiteral("local_utc_offset_seconds=%1\n").arg(QDateTime::currentDateTime().offsetFromUtc());
    text += QStringLiteral("dropped_frames=%1\n").arg(snapshot.has_stream_stats ? QString::number(snapshot.dropped_frames) : QString());
    text += QStringLiteral("frames_lost=%1\n").arg(snapshot.has_stream_stats ? QString::number(snapshot.frames_lost) : QString());
    if (snapshot.has_stream_stats && snapshot.measured_bitrate_kbps >= 0)
        text += QStringLiteral("measured_bitrate=%1\n").arg(snapshot.measured_bitrate_kbps);
    else
        text += QStringLiteral("measured_bitrate=\n");
    text += QStringLiteral("session_duration_sec=%1\n")
                .arg(snapshot.session_duration_sec > 0 ? QString::number(snapshot.session_duration_sec) : QString());
    text += QStringLiteral("stream_connected=%1\n").arg(boolText(snapshot.stream_connected));
    text += QStringLiteral("ps5_path_rtt_ms=%1\n")
                .arg(snapshot.ps5_path_rtt_us >= 0 ? QString::number(snapshot.ps5_path_rtt_us / 1000.0, 'f', 3) : QString());
    text += QStringLiteral("ps5_path_mtu_in=%1\n")
                .arg(snapshot.ps5_path_mtu_in >= 0 ? QString::number(snapshot.ps5_path_mtu_in) : QString());
    text += QStringLiteral("ps5_path_mtu_out=%1\n")
                .arg(snapshot.ps5_path_mtu_out >= 0 ? QString::number(snapshot.ps5_path_mtu_out) : QString());
    text += QStringLiteral("ps5_path_semantics=chiaki_senkusha_startup_measurement_to_console\n");
    text += QStringLiteral("gateway_probe_sent=%1\n").arg(snapshot.gateway_probe_sent);
    text += QStringLiteral("gateway_probe_replies=%1\n").arg(snapshot.gateway_probe_replies);
    text += QStringLiteral("gateway_loss_fraction=%1\n")
                .arg(snapshot.gateway_probe_sent > 0
                    ? QString::number(1.0 - double(snapshot.gateway_probe_replies) / double(snapshot.gateway_probe_sent), 'f', 6)
                    : QString());
    text += QStringLiteral("gateway_rtt_avg_ms=%1\n")
                .arg(snapshot.gateway_rtt_avg_ms >= 0 ? QString::number(snapshot.gateway_rtt_avg_ms, 'f', 3) : QString());
    text += QStringLiteral("interface_rx_errors_delta=%1\n").arg(snapshot.interface_rx_errors_delta);
    text += QStringLiteral("interface_tx_errors_delta=%1\n").arg(snapshot.interface_tx_errors_delta);
    text += QStringLiteral("interface_rx_discards_delta=%1\n").arg(snapshot.interface_rx_discards_delta);
    text += QStringLiteral("interface_tx_discards_delta=%1\n").arg(snapshot.interface_tx_discards_delta);
    text += QStringLiteral("network_route_changes=%1\n").arg(snapshot.network_route_changes);
    text += QStringLiteral("gateway_semantics=default_ipv4_gateway_icmp_not_console_path\n");
    return text;
}

void nax5ProcessLogStart()
{
    QMutexLocker lock(&g_mutex);
    if (g_started)
        return;
    const QString dir_str = GetLogBaseDir();
    if (dir_str.isEmpty())
        return;
    QDir dir(dir_str);
    rotateNax5Logs(dir);
    g_path = dir.absoluteFilePath(QStringLiteral("nax5_%1.log").arg(dateStamp()));
    QFile file(g_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        g_path.clear();
        return;
    }
    const QString header = nax5SanitizeProcessLogLine(
        QStringLiteral("[%1] nax5 process log start version=%2 sha=%3 operator=%4\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                 nax5ClientVersion(),
                 nax5ClientSha(),
                 Nax5Runtime::operatorMode() ? QStringLiteral("true") : QStringLiteral("false")));
    file.write(header.toUtf8());
    file.flush();
    g_started = true;
}

void nax5ProcessLogWrite(const char *category, const QString &message)
{
    const QString cat = QString::fromUtf8(category ? category : "");
    if (!cat.startsWith(QLatin1String("nax5")))
        return;
    QMutexLocker lock(&g_mutex);
    if (!g_started || g_path.isEmpty())
        return;
    QFile file(g_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    const QString line = nax5SanitizeProcessLogLine(
        QStringLiteral("[%1] [%2] %3\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate), cat, message));
    file.write(line.toUtf8());
    file.flush();
}

QString nax5ProcessLogPath()
{
    QMutexLocker lock(&g_mutex);
    return g_path;
}

QString nax5PreviousProcessLogPath()
{
    const QString current = QFileInfo(nax5ProcessLogPath()).fileName();
    const QString dir_str = GetLogBaseDir();
    if (dir_str.isEmpty())
        return QString();
    QDir dir(dir_str);
    // Names embed a sortable timestamp, so the name order is the start order.
    const QStringList files = dir.entryList(QStringList() << QStringLiteral("nax5_*.log"), QDir::Files, QDir::Name | QDir::Reversed);
    for (const QString &name : files)
    {
        if (name != current && (current.isEmpty() || name < current))
            return dir.absoluteFilePath(name);
    }
    return QString();
}

QString nax5ProcessLogTail(int max_bytes)
{
    QString path;
    {
        QMutexLocker lock(&g_mutex);
        path = g_path;
    }
    return nax5SanitizeProcessLogLine(QString::fromUtf8(nax5ReadFileTailBytes(path, max_bytes)));
}

QStringList nax5RecentChiakiSessionLogPaths(int max_files)
{
    QStringList paths;
    if (max_files <= 0)
        return paths;
    const QString dir_str = GetLogBaseDir();
    if (dir_str.isEmpty())
        return paths;
    QDir dir(dir_str);
    const QStringList files = dir.entryList(QStringList() << QStringLiteral("chiaki_session_*.log"), QDir::Files, QDir::Time);
    const int limit = qMin(max_files, files.size());
    for (int i = 0; i < limit; ++i)
        paths.append(dir.absoluteFilePath(files.at(i)));
    return paths;
}

QString nax5LatestChiakiSessionLogPath()
{
    const QStringList paths = nax5RecentChiakiSessionLogPaths(1);
    return paths.isEmpty() ? QString() : paths.first();
}

QString nax5ChiakiSessionLogTail(int max_bytes)
{
    return nax5SanitizeProcessLogLine(QString::fromUtf8(nax5ReadFileTailBytes(nax5LatestChiakiSessionLogPath(), max_bytes)));
}
