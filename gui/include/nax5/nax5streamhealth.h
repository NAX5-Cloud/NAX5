#pragma once

#include "nax5/nax5pathprobe.h"

#include <QJsonObject>
#include <QVector>
#include <QtGlobal>

// Aggregates 1 Hz stream samples into the STREAM_HEALTH telemetry event sent
// every kWindowSeconds during play, so lag is visible on the server live.
// Keys must match ALLOWED_METADATA_KEYS in nax5-backend client_telemetry/schemas.py.

struct Nax5StreamSecond
{
    double packet_loss = 0;     // chiaki rolling fraction (0..1)
    int frames_lost_total = 0;  // cumulative receiver frames lost
    qint64 bitrate_kbps = -1;
    int render_dropped = 0;     // renderer drops in the last second
    double queue_depth = 0;     // render queue depth EMA
};

class Nax5StreamHealth
{
public:
    static constexpr int kWindowSeconds = 20;

    void reset();
    // Returns true when a full window is ready to be taken.
    bool add(const Nax5StreamSecond &second);
    QJsonObject take(const Nax5PathWindow &path);
    int size() const { return int(seconds.size()); }

private:
    QVector<Nax5StreamSecond> seconds;
    int frames_lost_base = -1;
};

// Whole-session aggregates for the end-of-session report summary. The summary
// used to copy the last 1 s sample, so a session with thousands of dropped
// frames could report dropped_frames=0 and the final second's bitrate.
class Nax5SessionTotals
{
public:
    void reset();
    void add(const Nax5StreamSecond &second);
    int seconds() const { return sample_count; }
    double averagePacketLoss() const;   // mean of the ~2 s rolling fractions
    double maxPacketLoss() const { return max_loss; }
    int renderDroppedTotal() const { return dropped_total; }
    qint64 averageBitrateKbps() const;  // mean over seconds with a measurement, -1 if none

private:
    int sample_count = 0;
    double loss_sum = 0;
    double max_loss = 0;
    int dropped_total = 0;
    qint64 bitrate_sum = 0;
    int bitrate_count = 0;
};
