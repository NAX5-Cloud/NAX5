#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

struct Nax5BuildInfoSnapshot
{
    QString decoder;
    QString resolution;
    QString bitrate_kbps;
    QString fps;
    bool show_stream_stats = false;
    bool log_verbose = false;
    bool log_sanitize = true;
    bool has_stream_stats = false;
    double avg_packet_loss = 0;
    QString max_packet_loss;
    int dropped_frames = 0;
    int frames_lost = 0;
    qint64 measured_bitrate_kbps = -1;
    qint64 session_duration_sec = 0;
    bool stream_connected = false;
    qint64 ps5_path_rtt_us = -1;
    qint64 ps5_path_mtu_in = -1;
    qint64 ps5_path_mtu_out = -1;
    qint64 gateway_probe_sent = 0;
    qint64 gateway_probe_replies = 0;
    double gateway_rtt_avg_ms = -1;
    quint64 interface_rx_errors_delta = 0;
    quint64 interface_tx_errors_delta = 0;
    quint64 interface_rx_discards_delta = 0;
    quint64 interface_tx_discards_delta = 0;
    qint64 network_route_changes = 0;
};

void nax5ProcessLogStart();
void nax5ProcessLogWrite(const char *category, const QString &message);
QString nax5ProcessLogPath();
// nax5 log of the launcher run before this one (may have ended in a crash).
QString nax5PreviousProcessLogPath();
QString nax5ProcessLogTail(int max_bytes);
QByteArray nax5ReadFileTailBytes(const QString &path, int max_bytes);
QString nax5LatestChiakiSessionLogPath();
QStringList nax5RecentChiakiSessionLogPaths(int max_files);
QString nax5ChiakiSessionLogTail(int max_bytes);
QString nax5SanitizeProcessLogLine(const QString &line);
QString nax5ClientVersion();
QString nax5ClientSha();
QString nax5OsPlatform();
QString nax5OsVersion();
QString nax5OsBuild();
QString nax5LocaleName();
// Display adapters and driver versions for BUILD-INFO; empty off Windows.
QString nax5GpuAdapters();
QString nax5BuildInfoText();
QString nax5BuildInfoText(const Nax5BuildInfoSnapshot &snapshot);
