#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <memory>

// Client -> NAX5 VPS UDP probe during a stream (build 12 lag diagnostics).
// Sends a 32-byte datagram every 100 ms to the VPS echo service and records
// per-second loss and RTT. The VPS is ~5 ms from the PS5's home line, so loss
// here that the home agent does not see points at the player's ISP path.
// Build 14 runs a second instance against the home agent next to the PS5
// (router port forward kHomePort), measuring the player <-> home path itself.

struct Nax5PathWindow
{
    int seconds = 0;
    int sent = 0;
    int received = 0;
    int lost = 0;
    double rtt_p50_ms = -1;
    double rtt_p95_ms = -1;
    double rtt_max_ms = -1;
    double jitter_ms = -1; // mean absolute difference of consecutive RTTs
};

// Pure helper, unit-tested: summarize RTT samples (ms, in arrival order).
Nax5PathWindow nax5SummarizePath(const QVector<double> &rtts_ms, int sent, int lost);

// Adds <prefix>_sent/_lost and, when replies arrived, <prefix>_rtt_p50_ms/_rtt_p95_ms/
// _rtt_max_ms/_jitter_ms to a stream_sample line (see nax5-backend docs/LOGS.md).
void nax5InsertPathFields(QJsonObject &metric, const QString &prefix, const Nax5PathWindow &path);

class Nax5PathProbe
{
public:
    static constexpr quint16 kDefaultPort = 40998;
    static constexpr quint16 kHomePort = 40999;
    static constexpr int kIntervalMs = 100;
    static constexpr int kReplyTimeoutMs = 1000;

    Nax5PathProbe();
    ~Nax5PathProbe();
    Nax5PathProbe(const Nax5PathProbe &) = delete;
    Nax5PathProbe &operator=(const Nax5PathProbe &) = delete;

    // host may be a name; resolution happens on the probe thread.
    void start(const QString &host, quint16 port);
    void stop();
    bool running() const;
    // Aggregate of the last `seconds` completed seconds (max 120).
    Nax5PathWindow window(int seconds) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
