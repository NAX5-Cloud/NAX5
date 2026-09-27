#include "nax5/nax5streamhealth.h"

#include <algorithm>

namespace {
QString number(double value, int decimals = 1)
{
    return QString::number(value, 'f', decimals);
}
}

void Nax5StreamHealth::reset()
{
    seconds.clear();
    frames_lost_base = -1;
}

bool Nax5StreamHealth::add(const Nax5StreamSecond &second)
{
    if (frames_lost_base < 0)
        frames_lost_base = second.frames_lost_total;
    seconds.append(second);
    return seconds.size() >= kWindowSeconds;
}

QJsonObject Nax5StreamHealth::take(const Nax5PathWindow &path)
{
    QJsonObject out;
    if (seconds.isEmpty())
        return out;
    double loss_max = 0;
    int dropped_max = 0;
    double queue_max = 0;
    QVector<qint64> bitrates;
    for (const Nax5StreamSecond &s : seconds)
    {
        loss_max = std::max(loss_max, s.packet_loss);
        dropped_max = std::max(dropped_max, s.render_dropped);
        queue_max = std::max(queue_max, s.queue_depth);
        if (s.bitrate_kbps >= 0)
            bitrates.append(s.bitrate_kbps);
    }
    const int frames_lost_end = seconds.last().frames_lost_total;
    out.insert(QStringLiteral("window_s"), QString::number(seconds.size()));
    out.insert(QStringLiteral("loss_max_pct"), number(loss_max * 100.0, 2));
    // A counter reset (new stream) must not produce a negative delta.
    out.insert(QStringLiteral("frames_lost"), QString::number(std::max(0, frames_lost_end - frames_lost_base)));
    if (!bitrates.isEmpty())
    {
        std::sort(bitrates.begin(), bitrates.end());
        out.insert(QStringLiteral("bitrate_min_kbps"), QString::number(bitrates.first()));
        out.insert(QStringLiteral("bitrate_p50_kbps"), QString::number(bitrates.at(bitrates.size() / 2)));
    }
    out.insert(QStringLiteral("render_dropped_max"), QString::number(dropped_max));
    out.insert(QStringLiteral("queue_max"), number(queue_max));
    if (path.sent > 0)
    {
        out.insert(QStringLiteral("vps_sent"), QString::number(path.sent));
        out.insert(QStringLiteral("vps_lost"), QString::number(path.lost));
        if (path.rtt_p50_ms >= 0)
        {
            out.insert(QStringLiteral("vps_rtt_p50_ms"), number(path.rtt_p50_ms));
            out.insert(QStringLiteral("vps_rtt_p95_ms"), number(path.rtt_p95_ms));
            out.insert(QStringLiteral("vps_rtt_max_ms"), number(path.rtt_max_ms));
        }
        if (path.jitter_ms >= 0)
            out.insert(QStringLiteral("vps_jitter_ms"), number(path.jitter_ms));
    }
    frames_lost_base = frames_lost_end;
    seconds.clear();
    return out;
}

void Nax5SessionTotals::reset()
{
    *this = Nax5SessionTotals();
}

void Nax5SessionTotals::add(const Nax5StreamSecond &second)
{
    ++sample_count;
    loss_sum += second.packet_loss;
    max_loss = qMax(max_loss, second.packet_loss);
    dropped_total += qMax(0, second.render_dropped);
    if (second.bitrate_kbps > 0)
    {
        bitrate_sum += second.bitrate_kbps;
        ++bitrate_count;
    }
}

double Nax5SessionTotals::averagePacketLoss() const
{
    return sample_count > 0 ? loss_sum / sample_count : 0;
}

qint64 Nax5SessionTotals::averageBitrateKbps() const
{
    return bitrate_count > 0 ? bitrate_sum / bitrate_count : -1;
}
