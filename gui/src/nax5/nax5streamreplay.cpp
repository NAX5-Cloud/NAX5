#include "nax5/nax5streamreplay.h"

#include "streamsession.h"

#include <chiaki/audio.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/session.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

namespace {
struct Media
{
    QList<QByteArray> video;
    QList<QByteArray> audio;
    ChiakiCodec codec = CHIAKI_CODEC_H264;
    int width = 1280, height = 720, fps = 60;
};

struct Counters
{
    std::atomic<qint64> video_sent{0}, video_dropped{0}, video_rejected{0}, video_corrupted{0};
    std::atomic<qint64> decoded_frames{0}, audio_sent{0}, mic_frames{0};
};

Counters counters;
std::atomic<bool> stop_requested{false};
QThread *feeder = nullptr;
QString load_error;

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : fallback;
}

QList<QByteArray> readFramed(const QString &path)
{
    // [u32 little-endian size][payload] repeated; written by make-replay-media.py.
    QList<QByteArray> out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QByteArray all = file.readAll();
    qsizetype pos = 0;
    while (pos + 4 <= all.size())
    {
        const auto *p = reinterpret_cast<const uchar *>(all.constData() + pos);
        const quint32 size = p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24);
        pos += 4;
        if (size == 0 || pos + qsizetype(size) > all.size()) break;
        out.append(all.mid(pos, size));
        pos += size;
    }
    return out;
}

Media loadMedia()
{
    Media media;
    const QDir dir(qEnvironmentVariable("NAX5_REPLAY_DIR"));
    QFile meta_file(dir.filePath(QStringLiteral("meta.json")));
    if (!meta_file.open(QIODevice::ReadOnly))
    {
        load_error = QStringLiteral("meta.json missing");
        return media;
    }
    const auto meta = QJsonDocument::fromJson(meta_file.readAll()).object();
    const QString codec = meta.value("codec").toString();
    media.codec = codec == "h265_hdr" ? CHIAKI_CODEC_H265_HDR : codec == "h265" ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
    media.width = meta.value("width").toInt(1280);
    media.height = meta.value("height").toInt(720);
    media.fps = meta.value("fps").toInt(60);
    media.video = readFramed(dir.filePath(QStringLiteral("video.au")));
    media.audio = readFramed(dir.filePath(QStringLiteral("audio.opus")));
    if (media.video.isEmpty()) load_error = QStringLiteral("video.au empty");
    return media;
}

Media &media()
{
    static Media loaded = loadMedia();
    return loaded;
}

void writeResult(const QString &outcome)
{
    const QString path = qEnvironmentVariable("NAX5_REPLAY_RESULT");
    if (path.isEmpty()) return;
    QJsonObject result{
        {"outcome", outcome},
        {"load_error", load_error},
        {"video_sent", double(counters.video_sent)},
        {"video_dropped", double(counters.video_dropped)},
        {"video_rejected_by_decoder", double(counters.video_rejected)},
        {"video_corrupted", double(counters.video_corrupted)},
        {"decoded_frames", double(counters.decoded_frames)},
        {"audio_sent", double(counters.audio_sent)},
        {"mic_frames", double(counters.mic_frames)},
        {"finished_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(result).toJson());
}
}

// Friend of StreamSession: calls the same entry points the chiaki session
// thread would call when packets arrive from the console.
class Nax5StreamReplay
{
public:
    static void run(StreamSession *session)
    {
        Media &m = media();
        if (m.video.isEmpty())
        {
            writeResult(QStringLiteral("no_media"));
            QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
            return;
        }
        const int seconds = envInt("NAX5_REPLAY_SECONDS", 20);
        const int loss_pct = envInt("NAX5_REPLAY_LOSS_PCT", 0);
        const int corrupt_pct = envInt("NAX5_REPLAY_CORRUPT_PCT", 0);
        const int burst_every_s = envInt("NAX5_REPLAY_BURST_EVERY_S", 0);
        const int stall_ms = envInt("NAX5_REPLAY_STALL_MS", 0);
        // Opens audio output and the microphone but never plays a frame, so the
        // echo canceller gets no reference signal.
        const bool audio_header_only = envInt("NAX5_REPLAY_AUDIO_HEADER_ONLY", 0) != 0;
        auto *rng = QRandomGenerator::global();

        ChiakiEvent connected = {};
        connected.type = CHIAKI_EVENT_CONNECTED;
        session->Event(&connected);

        ChiakiAudioSink sink;
        chiaki_opus_decoder_get_sink(&session->opus_decoder, &sink);
        if (!m.audio.isEmpty())
        {
            // PS5 audio: Opus, 2 channels, 48 kHz, 480-sample (10 ms) frames.
            ChiakiAudioHeader header;
            chiaki_audio_header_set(&header, 2, 16, 48000, 480);
            sink.header_cb(&header, sink.user);
        }

        ChiakiFfmpegDecoder *decoder = session->GetFfmpegDecoder();
        const qint64 frame_ns = 1000000000LL / qMax(1, m.fps);
        const qint64 audio_ns = 10000000LL;
        QElapsedTimer clock;
        clock.start();
        qint64 next_video = 0, next_audio = 0, next_burst = burst_every_s * 1000000000LL;
        bool stalled = false;
        int video_index = 0, audio_index = 0, pending_lost = 0;

        while (!stop_requested && clock.nsecsElapsed() < seconds * 1000000000LL)
        {
            const qint64 now = clock.nsecsElapsed();
            if (stall_ms > 0 && !stalled && now > seconds * 500000000LL)
            {
                // One mid-session freeze, as when the path drops for a moment.
                stalled = true;
                QThread::msleep(stall_ms);
                continue;
            }
            int frames_due = now >= next_video ? 1 : 0;
            if (burst_every_s > 0 && now >= next_burst)
            {
                // Late packets arriving at once after a jitter spike.
                frames_due = 8;
                next_burst += burst_every_s * 1000000000LL;
            }
            for (int i = 0; i < frames_due && decoder; ++i)
            {
                QByteArray au = m.video[video_index];
                video_index = (video_index + 1) % m.video.size();
                next_video += frame_ns;
                // Keep the first IDR intact so the decoder can start.
                if (counters.video_sent > 0 && loss_pct > 0 && int(rng->bounded(100)) < loss_pct)
                {
                    ++pending_lost;
                    ++counters.video_dropped;
                    continue;
                }
                if (counters.video_sent > 0 && corrupt_pct > 0 && int(rng->bounded(100)) < corrupt_pct && au.size() > 64)
                {
                    for (int k = 0; k < 16; ++k)
                        au[32 + int(rng->bounded(au.size() - 32))] = char(rng->bounded(256));
                    ++counters.video_corrupted;
                }
                const bool ok = chiaki_ffmpeg_decoder_video_sample_cb(reinterpret_cast<uint8_t *>(au.data()),
                    size_t(au.size()), pending_lost, false, decoder);
                if (!ok) ++counters.video_rejected;
                pending_lost = 0;
                ++counters.video_sent;
            }
            while (!m.audio.isEmpty() && !audio_header_only && now >= next_audio)
            {
                QByteArray packet = m.audio[audio_index];
                audio_index = (audio_index + 1) % m.audio.size();
                sink.frame_cb(reinterpret_cast<uint8_t *>(packet.data()), size_t(packet.size()), sink.user);
                ++counters.audio_sent;
                next_audio += audio_ns;
            }
            const qint64 wait_ns = qMin(next_video, m.audio.isEmpty() ? next_video : next_audio) - clock.nsecsElapsed();
            if (wait_ns > 1000000) QThread::usleep(quint64(wait_ns / 1000));
        }

        writeResult(stop_requested ? QStringLiteral("stopped") : QStringLiteral("completed"));
        ChiakiEvent quit = {};
        quit.type = CHIAKI_EVENT_QUIT;
        quit.quit.reason = CHIAKI_QUIT_REASON_STOPPED;
        session->Event(&quit);
        QMetaObject::invokeMethod(qApp, [] { QTimer::singleShot(1500, qApp, &QCoreApplication::quit); }, Qt::QueuedConnection);
    }
};

bool nax5StreamReplayActive()
{
    return qEnvironmentVariableIsSet("NAX5_REPLAY_DIR");
}

void nax5StreamReplayApply(StreamSessionConnectInfo &info)
{
    const Media &m = media();
    info.target = CHIAKI_TARGET_PS5_1;
    info.video_profile.codec = m.codec;
    info.video_profile.width = m.width;
    info.video_profile.height = m.height;
    info.video_profile.max_fps = m.fps;
    const QString decoder = qEnvironmentVariable("NAX5_REPLAY_DECODER");
    if (!decoder.isEmpty()) info.hw_decoder = decoder == "none" ? QString() : decoder;
    info.start_mic_unmuted = envInt("NAX5_REPLAY_MIC", 0) != 0;
    info.speech_processing_enabled = envInt("NAX5_REPLAY_ECHO", 0) != 0;
    if (qEnvironmentVariableIsSet("NAX5_REPLAY_AUDIO_OUT")) info.audio_out_device = qEnvironmentVariable("NAX5_REPLAY_AUDIO_OUT");
    if (qEnvironmentVariableIsSet("NAX5_REPLAY_AUDIO_IN")) info.audio_in_device = qEnvironmentVariable("NAX5_REPLAY_AUDIO_IN");
}

void nax5StreamReplayStart(StreamSession *session)
{
    stop_requested = false;
    QObject::connect(session, &StreamSession::FfmpegFrameAvailable, session,
        [] { ++counters.decoded_frames; }, Qt::DirectConnection);
    feeder = QThread::create([session] { Nax5StreamReplay::run(session); });
    feeder->start();
}

void nax5StreamReplayStop(StreamSession *)
{
    stop_requested = true;
    if (feeder && feeder->currentThread() != feeder)
        feeder->wait(5000);
}

void nax5StreamReplayMicFrame()
{
    ++counters.mic_frames;
}
