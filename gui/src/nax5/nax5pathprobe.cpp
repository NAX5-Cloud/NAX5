#include "nax5/nax5pathprobe.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace {

constexpr char kMagic[6] = {'N', 'A', 'X', '5', 'C', '1'};
constexpr int kPacketBytes = 32;
constexpr int kHeaderBytes = 6 + 4 + 8; // magic, sequence, send time
constexpr int kMaxSeconds = 120;

struct SecondStats
{
    int sent = 0;
    int received = 0;
    int lost = 0;
    QVector<double> rtts;
};

double percentile(QVector<double> sorted, double fraction)
{
    if (sorted.isEmpty())
        return -1;
    const int index = qBound(0, int(std::ceil(fraction * sorted.size())) - 1, int(sorted.size()) - 1);
    return sorted.at(index);
}

} // namespace

Nax5PathWindow nax5SummarizePath(const QVector<double> &rtts_ms, int sent, int lost)
{
    Nax5PathWindow out;
    out.sent = sent;
    out.received = int(rtts_ms.size());
    out.lost = lost;
    if (rtts_ms.isEmpty())
        return out;
    QVector<double> sorted = rtts_ms;
    std::sort(sorted.begin(), sorted.end());
    out.rtt_p50_ms = percentile(sorted, 0.5);
    out.rtt_p95_ms = percentile(sorted, 0.95);
    out.rtt_max_ms = sorted.last();
    if (rtts_ms.size() > 1)
    {
        double sum = 0;
        for (int i = 1; i < rtts_ms.size(); ++i)
            sum += std::abs(rtts_ms.at(i) - rtts_ms.at(i - 1));
        out.jitter_ms = sum / (rtts_ms.size() - 1);
    }
    return out;
}

void nax5InsertPathFields(QJsonObject &metric, const QString &prefix, const Nax5PathWindow &path)
{
    metric.insert(prefix + QStringLiteral("_sent"), path.sent);
    metric.insert(prefix + QStringLiteral("_lost"), path.lost);
    if (path.rtt_p50_ms < 0)
        return;
    metric.insert(prefix + QStringLiteral("_rtt_p50_ms"), path.rtt_p50_ms);
    metric.insert(prefix + QStringLiteral("_rtt_p95_ms"), path.rtt_p95_ms);
    metric.insert(prefix + QStringLiteral("_rtt_max_ms"), path.rtt_max_ms);
    if (path.jitter_ms >= 0)
        metric.insert(prefix + QStringLiteral("_jitter_ms"), path.jitter_ms);
}

class Nax5PathProbe::Impl
{
public:
    class Worker final : public QThread
    {
    public:
        Worker(Impl *owner, const QString &host, quint16 port) : owner(owner), host(host), port(port) {}

    protected:
        void run() override
        {
            QHostAddress address(host);
            if (address.isNull())
            {
                const QHostInfo info = QHostInfo::fromName(host);
                for (const QHostAddress &candidate : info.addresses())
                    if (candidate.protocol() == QAbstractSocket::IPv4Protocol) { address = candidate; break; }
            }
            if (address.isNull() || isInterruptionRequested())
                return;
            QUdpSocket socket;
            if (!socket.bind(QHostAddress::AnyIPv4, 0))
                return;

            QElapsedTimer clock;
            clock.start();
            QHash<quint32, qint64> pending; // sequence -> send time (ns)
            quint32 sequence = 0;
            qint64 next_send = 0;
            qint64 next_second = 1000000000LL;
            SecondStats current;
            QByteArray packet(kPacketBytes, '\0');
            memcpy(packet.data(), kMagic, sizeof(kMagic));

            while (!isInterruptionRequested())
            {
                qint64 now = clock.nsecsElapsed();
                if (now >= next_send)
                {
                    ++sequence;
                    qToLittleEndian<quint32>(sequence, packet.data() + 6);
                    qToLittleEndian<qint64>(now, packet.data() + 10);
                    if (socket.writeDatagram(packet, address, port) == kPacketBytes)
                    {
                        pending.insert(sequence, now);
                        current.sent++;
                    }
                    next_send += qint64(kIntervalMs) * 1000000;
                    if (next_send < now) // fell behind (suspend, stall): do not burst
                        next_send = now + qint64(kIntervalMs) * 1000000;
                }

                const qint64 wait_ns = std::min(next_send, next_second) - clock.nsecsElapsed();
                if (wait_ns > 0)
                    socket.waitForReadyRead(int(std::max<qint64>(1, wait_ns / 1000000)));
                while (socket.hasPendingDatagrams())
                {
                    QByteArray reply(int(socket.pendingDatagramSize()), '\0');
                    QHostAddress from;
                    const qint64 size = socket.readDatagram(reply.data(), reply.size(), &from);
                    if (size < kHeaderBytes || from.toIPv4Address() != address.toIPv4Address()
                        || memcmp(reply.constData(), kMagic, sizeof(kMagic)) != 0)
                        continue;
                    const quint32 seq = qFromLittleEndian<quint32>(reply.constData() + 6);
                    const auto it = pending.find(seq);
                    if (it == pending.end())
                        continue;
                    current.received++;
                    current.rtts.append((clock.nsecsElapsed() - it.value()) / 1e6);
                    pending.erase(it);
                }

                now = clock.nsecsElapsed();
                if (now >= next_second)
                {
                    const qint64 deadline = now - qint64(kReplyTimeoutMs) * 1000000;
                    for (auto it = pending.begin(); it != pending.end();)
                    {
                        if (it.value() <= deadline) { current.lost++; it = pending.erase(it); }
                        else ++it;
                    }
                    owner->push(current);
                    current = SecondStats();
                    next_second += 1000000000LL;
                    if (next_second <= now)
                        next_second = now + 1000000000LL;
                }
            }
        }

    private:
        Impl *owner;
        QString host;
        quint16 port;
    };

    void push(const SecondStats &stats)
    {
        QMutexLocker lock(&mutex);
        seconds.push_back(stats);
        while (seconds.size() > kMaxSeconds)
            seconds.pop_front();
    }

    mutable QMutex mutex;
    std::deque<SecondStats> seconds;
    std::unique_ptr<Worker> worker;
};

Nax5PathProbe::Nax5PathProbe() : impl(new Impl) {}
Nax5PathProbe::~Nax5PathProbe() { stop(); }

void Nax5PathProbe::start(const QString &host, quint16 port)
{
    stop();
    if (host.isEmpty() || port == 0)
        return;
    {
        QMutexLocker lock(&impl->mutex);
        impl->seconds.clear();
    }
    impl->worker.reset(new Impl::Worker(impl.get(), host, port));
    impl->worker->start(QThread::LowPriority);
}

void Nax5PathProbe::stop()
{
    if (!impl->worker)
        return;
    impl->worker->requestInterruption();
    // The send/receive loop notices interruption within ~100 ms. Only a slow
    // DNS lookup can take longer; after it the worker returns without touching
    // Impl, so it is safe to let it finish on its own instead of blocking the UI.
    if (!impl->worker->wait(1500))
    {
        QThread *detached = impl->worker.release();
        QObject::connect(detached, &QThread::finished, detached, &QObject::deleteLater);
        if (detached->isFinished())
            detached->deleteLater();
        return;
    }
    impl->worker.reset();
}

bool Nax5PathProbe::running() const
{
    return impl->worker && impl->worker->isRunning();
}

Nax5PathWindow Nax5PathProbe::window(int seconds) const
{
    QVector<double> rtts;
    int sent = 0, lost = 0, used = 0;
    {
        QMutexLocker lock(&impl->mutex);
        const int count = qMin<int>(qMax(0, seconds), int(impl->seconds.size()));
        for (auto it = impl->seconds.end() - count; it != impl->seconds.end(); ++it)
        {
            sent += it->sent;
            lost += it->lost;
            rtts += it->rtts;
        }
        used = count;
    }
    Nax5PathWindow out = nax5SummarizePath(rtts, sent, lost);
    out.seconds = used;
    return out;
}
