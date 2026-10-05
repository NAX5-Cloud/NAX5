#include "nax5/session/nax5sessioncontroller.h"

#include "nax5/connection/nax5connectionparser.h"
#include "nax5/connection/nax5transienthost.h"
#include "nax5/nax5apiclient.h"
#include "nax5/nax5apiconfig.h"
#include "nax5/nax5apilane.h"
#include "nax5/nax5authcontroller.h"
#include "nax5/nax5clientreport.h"
#include "nax5/nax5crashhandler.h"
#include "nax5/nax5telemetry.h"
#include "nax5/nax5processlog.h"
#include "nax5/nax5runtime.h"
#include "nax5/session/nax5sessionparser.h"
#include "controllermanager.h"
#include "qmlbackend.h"
#include "qmlmainwindow.h"
#include "settings.h"
#include "streamsession.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSettings>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <QtGlobal>

Q_LOGGING_CATEGORY(nax5SessionLog, "nax5.session")

namespace {
constexpr int kHeartbeatIntervalMs = 20 * 1000;
constexpr int kHeartbeatRetryMs = 5 * 1000;
constexpr int kTelemetryFlushMs = 300;
constexpr int kTelemetryBatchLimit = 25;

QString boolDecoderName(const QString &hw_decoder)
{
    if (hw_decoder.isEmpty() || hw_decoder == QLatin1String("none"))
        return QStringLiteral("software");
    return hw_decoder;
}
}

Nax5SessionController::Nax5SessionController(Nax5AuthController *auth, QmlBackend *backend, QObject *parent)
    : QObject(parent)
    , auth(auth)
    , backend(backend)
    , api(new Nax5ApiClient(this))
    , lease_timer(new QTimer(this))
    , heartbeat_timer(new QTimer(this))
    , reserve_backoff_timer(new QTimer(this))
    , retry_tick_timer(new QTimer(this))
    , telemetry_flush_timer(new QTimer(this))
    , diagnostic_timer(new QTimer(this))
    , report_queue_timer(new QTimer(this))
    , report_worker(new QThread(this))
    , session_state(Nax5GameSessionStateIdle)
    , generation(0)
    , stream_generation(0)
    , reserve_request_id(0)
    , current_request_id(0)
    , cancel_request_id(0)
    , connection_request_id(0)
    , connected_request_id(0)
    , heartbeat_request_id(0)
    , test_result_request_id(0)
    , operator_request_id(0)
    , ignore_cancel_result(false)
    , stream_was_connected(false)
    , stream_first_frame_seen(false)
    , operator_test_active(false)
    , shutdown_started(false)
    , awaiting_abort_current(false)
    , pending_start_stream(false)
    , unauth_logout_pending(false)
    , client_report_request_id(0)
    , stream_max_packet_loss(0)
    , last_avg_packet_loss(0)
    , last_frames_lost(0)
    , last_measured_bitrate_kbps(-1)
    , last_dropped_frames(0)
{
    pending_terminal.mutation = Nax5TerminalMutationNone;
    pending_terminal.generation = 0;
    pending_terminal.request_id = 0;
    pending_terminal.attempts = 0;
    pending_terminal.silent = true;
    lease_timer->setSingleShot(true);
    connect(lease_timer, &QTimer::timeout, this, &Nax5SessionController::syncCurrent);
    heartbeat_timer->setSingleShot(true);
    connect(heartbeat_timer, &QTimer::timeout, this, &Nax5SessionController::sendHeartbeat);
    reserve_backoff_timer->setSingleShot(true);
    connect(reserve_backoff_timer, &QTimer::timeout, this, &Nax5SessionController::stateChanged);
    retry_tick_timer->setInterval(1000);
    connect(retry_tick_timer, &QTimer::timeout, this, [this]() {
        if (!reserve_backoff_timer->isActive())
            retry_tick_timer->stop();
        emit retryChanged();
    });
    telemetry_flush_timer->setSingleShot(true);
    telemetry_flush_timer->setInterval(kTelemetryFlushMs);
    connect(telemetry_flush_timer, &QTimer::timeout, this, &Nax5SessionController::flushTelemetry);
    diagnostic_timer->setInterval(1000);
    connect(diagnostic_timer, &QTimer::timeout, this, &Nax5SessionController::onDiagnosticTick);
    report_queue_timer->setInterval(1000);
    connect(report_queue_timer, &QTimer::timeout, this, &Nax5SessionController::serviceDiagnosticQueue);
    report_queue_timer->start();
    auto *collector = new QObject;
    collector->moveToThread(report_worker);
    connect(report_worker, &QThread::finished, collector, &QObject::deleteLater);
    connect(this, &Nax5SessionController::diagnosticCaptureRequested, collector,
        [this](const QString &root, qint64 owner) {
            emit diagnosticCaptureFinished(nax5CaptureReports(root, owner, 4));
        });
    connect(this, &Nax5SessionController::diagnosticCaptureFinished, this, [this](bool ok) {
        report_capture_busy = false;
        if (!ok) qCWarning(nax5SessionLog) << "diagnostic capture delayed: check disk space or source log";
        flushPendingClientReport();
    });
    report_worker->start(QThread::LowPriority);
    connect(api, &Nax5ApiClient::reserveFinished, this, &Nax5SessionController::onReserveFinished);
    connect(api, &Nax5ApiClient::currentFinished, this, &Nax5SessionController::onCurrentFinished);
    connect(api, &Nax5ApiClient::cancelFinished, this, &Nax5SessionController::onCancelFinished);
    connect(api, &Nax5ApiClient::connectionFinished, this, &Nax5SessionController::onConnectionFinished);
    connect(api, &Nax5ApiClient::connectedFinished, this, &Nax5SessionController::onConnectedFinished);
    connect(api, &Nax5ApiClient::heartbeatFinished, this, &Nax5SessionController::onHeartbeatFinished);
    // Any gamepad button, stick or touchpad event: lets the logs tell an AFK player
    // from a playing one (input_idle_s in stream_sample).
    connect(ControllerManager::GetInstance(), &ControllerManager::ControllerMoved, this, [this]() { last_input_clock.start(); });
    connect(api, &Nax5ApiClient::failFinished, this, &Nax5SessionController::onFailFinished);
    connect(api, &Nax5ApiClient::endFinished, this, &Nax5SessionController::onEndFinished);
    connect(api, &Nax5ApiClient::clientReportFinished, this, &Nax5SessionController::onClientReportFinished);
    connect(api, &Nax5ApiClient::operatorFinished, this, [this](quint64 request_id, const Nax5ConnectionParseResult &result) {
        if (request_id == operator_request_id)
            operator_request_id = 0;
        const bool silent = request_id == test_result_request_id;
        if (silent)
            test_result_request_id = 0;
        if (result.error != Nax5SessionErrorNone)
        {
            if (logoutIfUnauthenticated(result.error))
                return;
            if (!silent || (session_state != Nax5GameSessionStateConnecting && session_state != Nax5GameSessionStateActive))
            {
                setError(result.error);
                setStatusText(errorText(result.error));
            }
            return;
        }
        if (!silent)
        {
            setError(Nax5SessionErrorNone);
            setStatusText(QStringLiteral("Оператор: готово"));
        }
    });
    if (this->auth)
    {
        connect(this->auth, &Nax5AuthController::stateChanged, this, &Nax5SessionController::onAuthStateChanged);
        connect(this->auth, &Nax5AuthController::accountChanged, this, [this]() { emit stateChanged(); });
    }
    if (this->backend)
        connect(this->backend, &QmlBackend::sessionChanged, this, &Nax5SessionController::onChiakiSessionChanged);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Nax5SessionController::prepareShutdown);
    if (this->auth && this->auth->authenticated())
        flushPendingClientReport();
}

Nax5SessionController::~Nax5SessionController()
{
    network_diagnostics.stop();
    path_probe.stop();
    home_probe.stop();
    report_queue_timer->stop();
    report_worker->quit();
    report_worker->wait();
    disconnect(qApp, &QCoreApplication::aboutToQuit, this, &Nax5SessionController::prepareShutdown);
    ignore_cancel_result = true;
    pending_terminal.mutation = Nax5TerminalMutationNone;
    discardMaterial();
}

bool Nax5SessionController::playEnabled() const
{
    if (shutdown_started) return false;
    if (updateRequired()) return false;
    if (!auth || !nax5PlayEligibilityOk(auth->authenticated(), auth->emailVerified(), auth->accessStatus()))
        return false;
    if (!nax5SessionCanStartPlay(session_state))
        return false;
    if (nax5SessionTerminalBlocksPlay(pending_terminal.mutation))
        return false;
    if (reserve_backoff_timer->isActive())
        return false;
    if (streamSessionAlive())
        return false;
    return true;
}

bool Nax5SessionController::streamSessionAlive() const
{
    return backend && backend->qmlSession();
}

QString Nax5SessionController::liveToken() const
{
    if (!auth)
        return QString();
    return auth->sessionToken();
}

quint64 Nax5SessionController::bumpGeneration()
{
    stream_was_connected = nax5StreamConnectedOnNewGeneration();
    stream_first_frame_seen = nax5StreamFirstFrameSeenOnNewGeneration();
    reserve_request_id = 0;
    current_request_id = 0;
    connection_request_id = 0;
    connected_request_id = 0;
    return ++generation;
}

void Nax5SessionController::setState(Nax5GameSessionState next)
{
    if (session_state == next)
        return;
    session_state = next;
    emit stateChanged();
}

void Nax5SessionController::setStatusText(const QString &text)
{
    if (status_text == text)
        return;
    status_text = text;
    emit statusTextChanged();
}

void Nax5SessionController::setError(Nax5SessionError error)
{
    current_error = error;
    if (error != Nax5SessionErrorClientUpdateRequired)
        update_url.clear();
    const QString message = errorText(error);
    if (error_message == message)
        return;
    error_message = message;
    emit errorMessageChanged();
}

QString Nax5SessionController::errorText(Nax5SessionError error) const
{
    return nax5SessionErrorMessage(error, Nax5Runtime::operatorMode());
}

bool Nax5SessionController::isOperatorTest() const
{
    return operator_test_active || material.session_id == QLatin1String("operator-test");
}

void Nax5SessionController::clearAssignment()
{
    stream_was_connected = nax5StreamConnectedOnNewGeneration();
    stream_first_frame_seen = nax5StreamFirstFrameSeenOnNewGeneration();
    session_id.clear();
    console_code.clear();
    console_region.clear();
    lease_timer->stop();
    heartbeat_timer->stop();
    heartbeat_request_id = 0;
    emit assignmentChanged();
}

void Nax5SessionController::discardMaterial()
{
    material.clear();
}

void Nax5SessionController::applyAssignment(const Nax5SessionParseResult &result)
{
    session_id = result.session.id;
    console_code = result.console.code;
    console_region = result.console.region;
    beginDiagnosticReport();
    emit assignmentChanged();
    scheduleLeaseSync(result.session.lease_expires_at);
}

void Nax5SessionController::resetLocal()
{
    bumpGeneration();
    reserve_request_id = 0;
    current_request_id = 0;
    cancel_request_id = 0;
    connection_request_id = 0;
    connected_request_id = 0;
    heartbeat_request_id = 0;
    test_result_request_id = 0;
    operator_request_id = 0;
    pending_terminal.mutation = Nax5TerminalMutationNone;
    pending_terminal.request_id = 0;
    pending_terminal.attempts = 0;
    idempotency_key.clear();
    operator_console_code.clear();
    setRemainingSeconds(-1);
    setTopUpSuggested(false);
    ignore_cancel_result = false;
    stream_was_connected = false;
    stream_first_frame_seen = false;
    operator_test_active = false;
    awaiting_abort_current = false;
    pending_start_stream = false;
    unauth_logout_pending = false;
    heartbeat_timer->stop();
    reserve_backoff_timer->stop();
    retry_tick_timer->stop();
    emit retryChanged();
    discardMaterial();
    clearAssignment();
    setError(Nax5SessionErrorNone);
    setStatusText(QString());
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionReset));
}

void Nax5SessionController::scheduleLeaseSync(const QString &lease_expires_at)
{
    lease_timer->stop();
    if (lease_expires_at.isEmpty())
        return;
    const QDateTime expires = QDateTime::fromString(lease_expires_at, Qt::ISODate);
    if (!expires.isValid())
        return;
    const int msec = static_cast<int>(QDateTime::currentDateTimeUtc().msecsTo(expires.toUTC()));
    if (msec <= 0)
        QTimer::singleShot(0, this, &Nax5SessionController::syncCurrent);
    else
        lease_timer->start(msec + 250);
}

void Nax5SessionController::scheduleHeartbeat(int delay_ms)
{
    heartbeat_timer->stop();
    if (session_state != Nax5GameSessionStateActive || session_id.isEmpty() || !auth || liveToken().isEmpty())
        return;
    heartbeat_timer->start(delay_ms);
}

void Nax5SessionController::sendHeartbeat()
{
    if (heartbeat_request_id != 0 || session_state != Nax5GameSessionStateActive || !auth || session_id.isEmpty())
        return;
    const QString token = liveToken();
    if (token.isEmpty())
        return;
    heartbeat_request_id = api->heartbeatSession(token, session_id);
}

void Nax5SessionController::syncCurrent()
{
    if (!auth || !auth->authenticated())
        return;
    if (session_state != Nax5GameSessionStateIdle && session_state != Nax5GameSessionStateError)
        return;
    current_request_id = api->fetchCurrentSession(auth->sessionToken());
}

void Nax5SessionController::onAuthStateChanged()
{
    emit stateChanged();
    if (auth && auth->authenticated())
    {
        shutdown_started = false;
        report_retry_at = 0;
        report_retry_count = 0;
        if (diagnostic_owner != auth->userId())
        {
            diagnostic_report_id.clear();
            diagnostic_session_id.clear();
            diagnostic_finalized = false;
            diagnostic_process_offset = QFileInfo(nax5ProcessLogPath()).size();
        }
        nax5RecoverReports(nax5ReportQueueRoot(), auth->userId(), diagnostic_report_id);
        queueCrashDumps();
        flushPendingClientReport();
        // An /end/ that never reached the server must land before /current/,
        // or the client would reconnect to a session the player already left.
        if (!resumePersistedTerminal())
            syncCurrent();
        return;
    }
    diagnostic_timer->stop();
    resetLocal();
}

void Nax5SessionController::emitTelemetry(const QString &event_type, const QJsonObject &metadata)
{
    if (!auth || liveToken().isEmpty())
        return;
    nax5QueueClientEvent(event_type, session_id, metadata);
    if (nax5QueuedClientEventCount() >= kTelemetryBatchLimit)
        flushTelemetry();
    else
        telemetry_flush_timer->start();
}

void Nax5SessionController::flushTelemetry()
{
    telemetry_flush_timer->stop();
    const QString token = liveToken();
    if (token.isEmpty())
        return;
    nax5FlushClientEvents(token, api);
}

void Nax5SessionController::sampleStreamStats()
{
    if (!backend) return;
    StreamSession *session = backend->qmlSession();
    if (!session) session = qobject_cast<StreamSession *>(sender());
    if (!session) return;
    last_avg_packet_loss = session->GetAveragePacketLoss();
    if (last_avg_packet_loss > stream_max_packet_loss)
        stream_max_packet_loss = last_avg_packet_loss;
    last_frames_lost = session->GetFramesLost();
    last_measured_bitrate_kbps = qRound(session->GetMeasuredBitrate() * 1000.0);
    if (backend->qmlWindow())
        last_dropped_frames = qMax(0, backend->qmlWindow()->droppedFrames());
    if (diagnostic_clock.isValid())
    {
        diagnostic_duration_ms = diagnostic_clock.elapsed();
        const qint64 gap = diagnostic_duration_ms - diagnostic_last_sample_ms;
        diagnostic_last_sample_ms = diagnostic_duration_ms;
        QJsonObject metric{{"schema", 3}, {"event", "stream_sample"},
            {"session_id", diagnostic_session_id},
            {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"elapsed_ms", double(diagnostic_duration_ms)}, {"sample_gap_ms", double(gap)},
            {"packet_loss_rolling_fraction", last_avg_packet_loss},
            {"frames_lost_total", last_frames_lost}, {"render_dropped_recent_1s", last_dropped_frames},
            {"measured_bitrate_kbps", double(last_measured_bitrate_kbps)}};
        // Seconds since the last gamepad input in this stream (counted from the first frame).
        metric.insert("input_idle_s", last_input_clock.isValid() ? double(last_input_clock.elapsed() / 1000) : -1.0);
        metric.insert("ps5_path_rtt_ms", session->GetInitialRttUs() / 1000.0);
        metric.insert("ps5_path_mtu_in", double(session->GetMtuIn()));
        metric.insert("ps5_path_mtu_out", double(session->GetMtuOut()));
        if (backend->qmlWindow())
        {
            metric.insert("render_queue_depth_ema", backend->qmlWindow()->queueDepthAverage());
            metric.insert("pending_frame_age_ms", 1000.0 * backend->qmlWindow()->pendingFrameAge());
            metric.insert("renderer_backend_enum", backend->qmlWindow()->runtimeRendererBackend());
        }
        if (path_probe.running())
            nax5InsertPathFields(metric, QStringLiteral("vps"), path_probe.window(1));
        if (home_probe.running())
            nax5InsertPathFields(metric, QStringLiteral("home"), home_probe.window(1));
        nax5ProcessLogWrite("nax5.metrics", QString::fromUtf8(QJsonDocument(metric).toJson(QJsonDocument::Compact)));
    }
}

void Nax5SessionController::onDiagnosticTick()
{
    sampleStreamStats();
    if (!diagnostic_clock.isValid())
        return;
    Nax5StreamSecond second;
    second.packet_loss = last_avg_packet_loss;
    second.frames_lost_total = last_frames_lost;
    second.bitrate_kbps = last_measured_bitrate_kbps;
    second.render_dropped = last_dropped_frames;
    if (backend && backend->qmlWindow())
        second.queue_depth = backend->qmlWindow()->queueDepthAverage();
    session_totals.add(second);
    if (stream_health.add(second) && !isOperatorTest())
        emitTelemetry(QStringLiteral("STREAM_HEALTH"), stream_health.take(path_probe.window(Nax5StreamHealth::kWindowSeconds)));
    if (!stream_stalled && session_state == Nax5GameSessionStateActive && last_frame_clock.isValid()
        && nax5StreamStalled(stream_first_frame_seen, last_frame_clock.elapsed()))
        stopStalledStream();
}

void Nax5SessionController::stopStalledStream()
{
    // Normal stop, as if the player pressed Disconnect: onStreamQuit posts /end/
    // and uploads the report, and the console is freed for the next player.
    qCWarning(nax5SessionLog) << "no frame decoded for" << last_frame_clock.elapsed() << "ms, stopping stream";
    stream_stalled = true;
    if (streamSessionAlive())
        backend->stopSession(false);
}

Nax5BuildInfoSnapshot Nax5SessionController::buildInfoSnapshot()
{
    Nax5BuildInfoSnapshot snapshot;
    Settings *settings = backend ? backend->chiakiSettings() : nullptr;
    if (settings)
    {
        snapshot.decoder = last_decoder.isEmpty() ? boolDecoderName(settings->GetHardwareDecoder()) : last_decoder;
        const ChiakiConnectVideoProfile profile = settings->GetVideoProfileRemotePS5();
        snapshot.resolution = last_resolution.isEmpty()
            ? QStringLiteral("%1x%2").arg(profile.width).arg(profile.height)
            : last_resolution;
        snapshot.bitrate_kbps = last_bitrate_kbps.isEmpty() && profile.bitrate
            ? QString::number(profile.bitrate)
            : last_bitrate_kbps;
        snapshot.fps = last_fps.isEmpty() && profile.max_fps
            ? QString::number(profile.max_fps)
            : last_fps;
        snapshot.show_stream_stats = settings->GetShowStreamStats();
        snapshot.log_verbose = settings->GetLogVerbose();
        snapshot.log_sanitize = !Nax5Runtime::operatorMode() || settings->GetLogSanitize();
    }
    else
    {
        snapshot.decoder = last_decoder;
        snapshot.resolution = last_resolution;
        snapshot.bitrate_kbps = last_bitrate_kbps;
        snapshot.fps = last_fps;
    }

    const bool live = streamSessionAlive();
    snapshot.stream_connected = diagnostic_stream_connected;
    if (live || stream_connected_at.isValid() || last_dropped_frames > 0 || last_frames_lost > 0)
    {
        snapshot.has_stream_stats = true;
        // Whole-session figures once 1 Hz samples exist; the last sample only before that.
        const bool totals = session_totals.seconds() > 0;
        snapshot.avg_packet_loss = totals ? session_totals.averagePacketLoss() : last_avg_packet_loss;
        const double max_loss = totals ? qMax(session_totals.maxPacketLoss(), stream_max_packet_loss) : stream_max_packet_loss;
        if (max_loss > 0)
            snapshot.max_packet_loss = QString::number(max_loss, 'f', 6);
        snapshot.dropped_frames = totals ? session_totals.renderDroppedTotal() : last_dropped_frames;
        snapshot.frames_lost = last_frames_lost;
        snapshot.measured_bitrate_kbps = totals ? session_totals.averageBitrateKbps() : last_measured_bitrate_kbps;
    }
    if (stream_connected_at.isValid())
        snapshot.session_duration_sec = diagnostic_duration_ms / 1000;
    if (live)
        snapshot.stream_connected = backend->qmlSession()->GetConnected() || diagnostic_stream_connected;
    StreamSession *stream = backend ? backend->qmlSession() : nullptr;
    if (!stream && diagnostic_stream)
        stream = diagnostic_stream.data();
    if (stream)
    {
        snapshot.ps5_path_rtt_us = static_cast<qint64>(stream->GetInitialRttUs());
        snapshot.ps5_path_mtu_in = stream->GetMtuIn();
        snapshot.ps5_path_mtu_out = stream->GetMtuOut();
    }
    const Nax5NetworkDiagnosticsSummary network = network_diagnostics.summary();
    snapshot.gateway_probe_sent = network.probes_sent;
    snapshot.gateway_probe_replies = network.probe_replies;
    snapshot.gateway_rtt_avg_ms = network.probe_replies > 0
        ? network.rtt_sum_ms / double(network.probe_replies) : -1;
    snapshot.interface_rx_errors_delta = network.rx_errors_delta;
    snapshot.interface_tx_errors_delta = network.tx_errors_delta;
    snapshot.interface_rx_discards_delta = network.rx_discards_delta;
    snapshot.interface_tx_discards_delta = network.tx_discards_delta;
    snapshot.network_route_changes = network.route_changes;
    return snapshot;
}

void Nax5SessionController::play()
{
    if (!playEnabled())
        return;
    if (api->hasLane(Nax5ApiLaneReserve))
        return;
    if (nax5SessionTerminalBlocksPlay(pending_terminal.mutation))
        return;
    if (streamSessionAlive())
        return;

    setTopUpSuggested(false);
    setRemainingSeconds(-1);
    if (!diagnostic_report_id.isEmpty() && !diagnostic_finalized)
        submitClientReport(Nax5ClientReportKindQuit);
    if (auth && auth->userId() > 0)
    {
        nax5RecoverReports(nax5ReportQueueRoot(), auth->userId());
        queueCrashDumps();
    }
    diagnostic_report_id.clear();
    diagnostic_session_id.clear();
    diagnostic_finalized = false;
    diagnostic_stream_connected = false;
    diagnostic_owner = auth ? auth->userId() : 0;
    diagnostic_process_offset = QFileInfo(nax5ProcessLogPath()).size();
    diagnostic_timer->stop();
    network_diagnostics.stop();
    path_probe.stop();
    home_probe.stop();
    diagnostic_clock.invalidate();
    diagnostic_duration_ms = 0;
    diagnostic_last_sample_ms = 0;
    last_decoder.clear();
    last_resolution.clear();
    last_bitrate_kbps.clear();
    last_fps.clear();
    bumpGeneration();
    idempotency_key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ignore_cancel_result = false;
    stream_connected_at = QDateTime();
    stream_max_packet_loss = 0;
    session_totals.reset();
    last_avg_packet_loss = 0;
    last_frames_lost = 0;
    last_measured_bitrate_kbps = -1;
    last_dropped_frames = 0;
    setError(Nax5SessionErrorNone);
    setStatusText(QStringLiteral("Ищем свободную консоль..."));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionPlayClicked));
    qCInfo(nax5SessionLog) << "play start";
    emitTelemetry(QStringLiteral("PLAY_REQUESTED"));
    reserve_request_id = api->reserveSession(liveToken(), idempotency_key);
}

void Nax5SessionController::release()
{
    if (!releaseEnabled())
        return;
    if (session_state == Nax5GameSessionStateReserving)
    {
        ignore_cancel_result = false;
        abortReserveAndSyncCurrent();
        return;
    }
    if (session_id.isEmpty() || !auth)
        return;
    const Nax5TerminalMutation mutation = nax5ReleaseMutation(session_state, stream_was_connected);
    bumpGeneration();
    reserve_request_id = 0;
    current_request_id = 0;
    connection_request_id = 0;
    setError(Nax5SessionErrorNone);
    setStatusText(QStringLiteral("Освобождаем консоль..."));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionReleaseClicked));
    ignore_cancel_result = false;
    heartbeat_timer->stop();
    heartbeat_request_id = 0;
    dispatchTerminal(mutation, false);
    if (streamSessionAlive())
        backend->stopSession(false);
}

void Nax5SessionController::abortReserveAndSyncCurrent()
{
    api->abortLane(Nax5ApiLaneReserve);
    reserve_request_id = 0;
    awaiting_abort_current = true;
    setError(Nax5SessionErrorNone);
    setStatusText(QStringLiteral("Освобождаем консоль..."));
    if (session_state == Nax5GameSessionStateReserving)
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionReleaseClicked));
    if (auth && auth->authenticated() && !liveToken().isEmpty())
        current_request_id = api->fetchCurrentSession(liveToken());
}

void Nax5SessionController::waitForShutdownIo()
{
    QElapsedTimer elapsed;
    elapsed.start();
    const int hops = awaiting_abort_current || pending_terminal.mutation != Nax5TerminalMutationNone ? 2 : 1;
    const int budget = nax5ShutdownGraceMs() * hops;
    while (elapsed.elapsed() < budget)
    {
        const bool waiting = current_request_id != 0
            || awaiting_abort_current
            || pending_terminal.mutation != Nax5TerminalMutationNone;
        if (!waiting)
            break;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(api, &Nax5ApiClient::currentFinished, &loop, &QEventLoop::quit);
        QObject::connect(api, &Nax5ApiClient::endFinished, &loop, &QEventLoop::quit);
        QObject::connect(api, &Nax5ApiClient::failFinished, &loop, &QEventLoop::quit);
        QObject::connect(api, &Nax5ApiClient::cancelFinished, &loop, &QEventLoop::quit);
        const int remaining = budget - static_cast<int>(elapsed.elapsed());
        timer.start(remaining > 0 ? remaining : 1);
        loop.exec();
    }
}

void Nax5SessionController::waitForShutdownReport()
{
    QElapsedTimer elapsed;
    elapsed.start();
    const int budget = nax5ShutdownReportGraceMs();
    while (elapsed.elapsed() < budget)
    {
        if (!report_capture_busy && client_report_request_id == 0
            && (!auth || !nax5ReportQueueHasWork(nax5ReportQueueRoot(), auth->userId()))) break;
        serviceDiagnosticQueue();
        if (!report_capture_busy && client_report_request_id == 0 && !api->hasLane(Nax5ApiLaneReport))
            break;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(api, &Nax5ApiClient::clientReportFinished, &loop, &QEventLoop::quit);
        QObject::connect(this, &Nax5SessionController::diagnosticCaptureFinished, &loop, &QEventLoop::quit);
        const int remaining = budget - static_cast<int>(elapsed.elapsed());
        timer.start(remaining > 0 ? remaining : 1);
        loop.exec();
    }
    // Every upload already has a durable copy. Never rebuild from newer logs here.
}

void Nax5SessionController::dispatchTerminal(Nax5TerminalMutation mutation, bool silent)
{
    if (mutation == Nax5TerminalMutationNone)
        return;
    pending_terminal.mutation = mutation;
    pending_terminal.token = liveToken();
    pending_terminal.session_id = session_id;
    pending_terminal.generation = generation;
    pending_terminal.attempts = 0;
    pending_terminal.silent = silent;
    pending_terminal.resumed = false;
    pending_terminal.request_id = 0;
    if (pending_terminal.token.isEmpty() || pending_terminal.session_id.isEmpty())
        return;
    if (auth && auth->userId() > 0)
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
            Nax5Runtime::settingsOrganizationName(), Nax5Runtime::settingsApplicationName());
        nax5SaveTerminal(settings, {auth->userId(), pending_terminal.session_id, mutation, QString()});
    }
    sendPendingTerminal();
}

bool Nax5SessionController::resumePersistedTerminal()
{
    if (!auth || auth->userId() <= 0 || pending_terminal.mutation != Nax5TerminalMutationNone)
        return false;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        Nax5Runtime::settingsOrganizationName(), Nax5Runtime::settingsApplicationName());
    const Nax5PersistedTerminal saved = nax5LoadTerminal(settings, auth->userId());
    if (saved.mutation == Nax5TerminalMutationNone || saved.session_id == session_id)
        return false;
    qCInfo(nax5SessionLog) << "resending terminal left from previous run" << static_cast<int>(saved.mutation);
    pending_terminal = {};
    pending_terminal.mutation = saved.mutation;
    pending_terminal.token = liveToken();
    pending_terminal.session_id = saved.session_id;
    pending_terminal.generation = generation;
    pending_terminal.silent = true;
    pending_terminal.resumed = true;
    if (pending_terminal.token.isEmpty())
    {
        pending_terminal = {};
        return false;
    }
    sendPendingTerminal();
    return true;
}

void Nax5SessionController::sendPendingTerminal()
{
    if (pending_terminal.mutation == Nax5TerminalMutationNone)
        return;
    if (pending_terminal.token.isEmpty() || pending_terminal.session_id.isEmpty())
        return;
    pending_terminal.attempts++;
    switch (pending_terminal.mutation) {
    case Nax5TerminalMutationEnd:
        pending_terminal.request_id = api->endSession(pending_terminal.token, pending_terminal.session_id);
        cancel_request_id = pending_terminal.request_id;
        break;
    case Nax5TerminalMutationFail:
        pending_terminal.request_id = api->failSession(pending_terminal.token, pending_terminal.session_id);
        cancel_request_id = pending_terminal.request_id;
        break;
    case Nax5TerminalMutationCancel:
        pending_terminal.request_id = api->cancelSession(pending_terminal.token, pending_terminal.session_id);
        cancel_request_id = pending_terminal.request_id;
        break;
    case Nax5TerminalMutationNone:
        break;
    }
}

bool Nax5SessionController::retryTerminalIfNeeded(const Nax5SessionParseResult &result)
{
    if (shutdown_started)
        return false;
    if (pending_terminal.mutation == Nax5TerminalMutationNone)
        return false;
    if (pending_terminal.generation != generation)
        return false;
    if (!nax5TerminalShouldRetry(result.error))
        return false;
    if (pending_terminal.attempts >= nax5TerminalRetryLimit())
        return false;
    qCWarning(nax5SessionLog) << "terminal retry" << static_cast<int>(result.error) << "attempt" << pending_terminal.attempts;
    QTimer::singleShot(nax5TerminalRetryDelayMs(pending_terminal.attempts), this, &Nax5SessionController::sendPendingTerminal);
    return true;
}

void Nax5SessionController::handleTerminalFinished(quint64 request_id, const Nax5SessionParseResult &result, Nax5TerminalMutation mutation)
{
    if (!nax5AcceptAsync(pending_terminal.generation, generation, pending_terminal.request_id, request_id)
        && request_id != cancel_request_id)
        return;
    if (pending_terminal.mutation != mutation && pending_terminal.mutation != Nax5TerminalMutationNone)
        return;
    if (retryTerminalIfNeeded(result))
        return;
    if (!nax5TerminalShouldRetry(result.error))
    {
        // The session is over on the server: show the play time it left.
        setRemainingSeconds(-1);
        if (auth)
            auth->refreshAccount();
        // The server gave a final answer (done, already ended, not found): nothing to resend.
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
            Nax5Runtime::settingsOrganizationName(), Nax5Runtime::settingsApplicationName());
        nax5ClearTerminal(settings, pending_terminal.session_id);
    }
    if (pending_terminal.resumed)
    {
        qCInfo(nax5SessionLog) << "resent terminal from previous run" << static_cast<int>(mutation)
                               << "result" << static_cast<int>(result.error);
        pending_terminal = {};
        syncCurrent();
        return;
    }
    pending_terminal.request_id = 0;
    const bool silent = ignore_cancel_result || pending_terminal.silent || shutdown_started;
    pending_terminal.mutation = Nax5TerminalMutationNone;
    pending_terminal.attempts = 0;
    if (unauth_logout_pending)
    {
        finishUnauthLogout();
        return;
    }
    if (logoutIfUnauthenticated(result.error))
        return;
    if (silent)
        ignore_cancel_result = false;
    if (mutation == Nax5TerminalMutationFail || mutation == Nax5TerminalMutationEnd)
    {
        stream_was_connected = nax5StreamConnectedOnNewGeneration();
        stream_first_frame_seen = nax5StreamFirstFrameSeenOnNewGeneration();
    }
    emit stateChanged();
    if (!shutdown_started && silent)
        submitClientReport(mutation == Nax5TerminalMutationFail ? Nax5ClientReportKindError : Nax5ClientReportKindQuit);
    if (silent)
        return;
    onCancelFinished(request_id, result);
}

bool Nax5SessionController::logoutIfUnauthenticated(Nax5SessionError error)
{
    if (error != Nax5SessionErrorUnauthenticated)
        return false;
    if (unauth_logout_pending)
    {
        finishUnauthLogout();
        return true;
    }
    const Nax5TerminalMutation mutation = nax5ReleaseMutation(session_state, stream_was_connected);
    if (nax5ShouldSendTerminalBeforeUnauthLogout(!session_id.isEmpty(), !liveToken().isEmpty(), mutation))
    {
        qCInfo(nax5SessionLog) << "401 terminal before logout" << static_cast<int>(mutation);
        unauth_logout_pending = true;
        dispatchTerminal(mutation, true);
        if (streamSessionAlive())
            backend->stopSession(false);
        return true;
    }
    finishUnauthLogout();
    return true;
}

void Nax5SessionController::finishUnauthLogout()
{
    unauth_logout_pending = false;
    submitClientReport(Nax5ClientReportKindLogout);
    if (auth)
        auth->logout();
}

void Nax5SessionController::prepareShutdown()
{
    if (shutdown_started)
        return;
    shutdown_started = true;
    ignore_cancel_result = true;
    flushTelemetry();
    if (nax5ShutdownNeedsCurrentSync(session_state))
    {
        abortReserveAndSyncCurrent();
        waitForShutdownIo();
        submitClientReport(Nax5ClientReportKindQuit);
        waitForShutdownReport();
        bumpGeneration();
        return;
    }
    const Nax5TerminalMutation mutation = nax5ShutdownMutation(session_state, stream_was_connected);
    if (mutation != Nax5TerminalMutationNone && !session_id.isEmpty() && !liveToken().isEmpty())
        dispatchTerminal(mutation, true);
    if (streamSessionAlive())
    {
        pending_start_stream = false;
        setState(Nax5GameSessionStateEnding);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(backend, &QmlBackend::sessionChanged, &loop, [&loop](StreamSession *session) {
            if (!session) loop.quit();
        });
        backend->stopSession(false);
        if (streamSessionAlive()) { timeout.start(2000); loop.exec(); }
        if (diagnostic_stream)
            QCoreApplication::sendPostedEvents(diagnostic_stream.data(), QEvent::DeferredDelete);
    }
    waitForShutdownIo();
    submitClientReport(Nax5ClientReportKindQuit);
    waitForShutdownReport();
    bumpGeneration();
    reserve_request_id = 0;
    current_request_id = 0;
    connection_request_id = 0;
    connected_request_id = 0;
}

void Nax5SessionController::releaseAndLogout()
{
    prepareShutdown();
    resetLocal();
    if (auth)
        auth->logout();
}

void Nax5SessionController::saveReport()
{
    sampleStreamStats();
    const QString zip_path = nax5WriteClientReportZip(buildInfoSnapshot());
    if (!zip_path.isEmpty())
        setStatusText(QStringLiteral("Отчёт сохранён на рабочий стол"));
    if (auth && auth->authenticated() && !liveToken().isEmpty())
        submitClientReport(Nax5ClientReportKindManual);
}

void Nax5SessionController::flushPendingClientReport()
{
    if (!auth || !auth->authenticated() || liveToken().isEmpty()
        || client_report_request_id != 0 || api->hasLane(Nax5ApiLaneReport)
        || QDateTime::currentMSecsSinceEpoch() < report_retry_at)
        return;
    // Sending full logs must not compete with reservation, handshake, gameplay,
    // or an unacknowledged /end/ or /fail/ (concurrent uploads deadlocked the backend).
    if ((!diagnostic_report_id.isEmpty() && !diagnostic_finalized) || streamSessionAlive()
        || pending_terminal.mutation != Nax5TerminalMutationNone) return;
    uploading_part = nax5NextReportPart(nax5ReportQueueRoot(), auth->userId());
    if (uploading_part.path.isEmpty()) return;
    client_report_request_id = api->postClientReportArchive(liveToken(),
        nax5ClientReportKindFromName(uploading_part.kind), uploading_part.session_id, uploading_part.archive,
        uploading_part.client_version, uploading_part.client_sha);
}

void Nax5SessionController::onClientReportFinished(quint64 request_id, int http_status)
{
    if (request_id == 0 || request_id != client_report_request_id) return;
    client_report_request_id = 0;
    const bool ok = http_status == 200 || http_status == 201 || http_status == 204;
    if (ok)
    {
        if (!nax5AcknowledgeReportPart(uploading_part))
            qCWarning(nax5SessionLog) << "report acknowledgement could not be persisted";
        report_retry_count = 0;
        report_retry_at = 0;
    }
    else
    {
        report_retry_count = qMin(report_retry_count + 1, 6);
        const int delay = (http_status == 400 || http_status == 401 || http_status == 403 || http_status == 413)
            ? 300000 : qMin(300000, 5000 * (1 << report_retry_count));
        report_retry_at = QDateTime::currentMSecsSinceEpoch() + delay;
        qCWarning(nax5SessionLog) << "report part retained for retry" << http_status << "delay_ms" << delay;
    }
    uploading_part = {};
    if (ok) QTimer::singleShot(0, this, &Nax5SessionController::serviceDiagnosticQueue);
}

void Nax5SessionController::beginDiagnosticReport()
{
    if (!diagnostic_report_id.isEmpty() || !auth || auth->userId() <= 0) return;
    diagnostic_owner = auth->userId();
    diagnostic_session_id = session_id;
    diagnostic_report_id = nax5BeginReport(nax5ReportQueueRoot(), diagnostic_owner,
        diagnostic_session_id, nax5BuildInfoText(buildInfoSnapshot()),
        {{nax5ProcessLogPath(), diagnostic_process_offset}});
    if (diagnostic_report_id.isEmpty())
        qCWarning(nax5SessionLog) << "cannot create durable diagnostic journal";
}

void Nax5SessionController::queueCrashDumps()
{
    if (!auth || auth->userId() <= 0) return;
    // Before the summaries are packed into reports and removed.
    applyCrashDecoderFallback();
    const int queued = nax5QueueCrashDumps(nax5ReportQueueRoot(), auth->userId(), nax5CrashDumpDir(),
        nax5BuildInfoText(buildInfoSnapshot()));
    if (queued > 0)
        qCWarning(nax5SessionLog) << "crash dumps queued for upload" << queued;
}

void Nax5SessionController::serviceDiagnosticQueue()
{
    if (!auth || !auth->authenticated()) return;
    if (!report_capture_busy)
    {
        report_capture_busy = true;
        emit diagnosticCaptureRequested(nax5ReportQueueRoot(), auth->userId());
    }
    if (!report_capture_busy) flushPendingClientReport();
}

void Nax5SessionController::submitClientReport(Nax5ClientReportKind kind)
{
    flushTelemetry();
    if (kind == Nax5ClientReportKindManual && streamSessionAlive())
    {
        serviceDiagnosticQueue(); // Manual export must not stop the active capture.
        return;
    }
    // The transport may still be shutting down after the terminal API reply.
    if (streamSessionAlive()) return;
    if (diagnostic_finalized) { serviceDiagnosticQueue(); return; }
    beginDiagnosticReport();
    if (diagnostic_report_id.isEmpty()) return;
    sampleStreamStats();
    diagnostic_timer->stop();
    diagnostic_clock.invalidate();
    diagnostic_finalized = nax5FinishReport(nax5ReportQueueRoot(), diagnostic_report_id,
        nax5ClientReportKindName(kind), nax5BuildInfoText(buildInfoSnapshot()));
    if (!diagnostic_finalized)
        qCWarning(nax5SessionLog) << "cannot finalize diagnostic journal; original logs retained";
    serviceDiagnosticQueue();
}

void Nax5SessionController::fetchConnection()
{
    if (!auth || session_id.isEmpty())
        return;
    setStatusText(QStringLiteral("Подключаемся..."));
    connection_request_id = api->fetchConnection(auth->sessionToken(), session_id);
}

void Nax5SessionController::startStream()
{
    if (!backend)
        return;
    if (session_state != Nax5GameSessionStateFetchingConnection && session_state != Nax5GameSessionStateConnecting)
    {
        pending_start_stream = false;
        return;
    }
    if (!nax5SessionCanCreateStream(streamSessionAlive()))
    {
        pending_start_stream = true;
        return;
    }
    pending_start_stream = false;
    stream_generation = generation;
    stream_stalled = false;
    last_frame_clock.invalidate();
    StreamSessionConnectInfo info;
    if (!nax5FillStreamSessionConnectInfo(backend->chiakiSettings(), material, &info))
    {
        discardMaterial();
        setError(Nax5SessionErrorInvalidConnectionMaterial);
        setStatusText(errorText(Nax5SessionErrorInvalidConnectionMaterial));
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionConnectionFailed));
        if (isOperatorTest())
            reportOperatorTestResult(false);
        else
            reportFail();
        return;
    }
    if (nax5ProductShouldWakeupBeforeCreateSession())
    {
        const Nax5MaterialWakeup wakeup = nax5MaterialWakeupCall(material.host, material.regist_key, true);
        if (wakeup.ready)
        {
            const bool sent = backend->sendMaterialWakeup(wakeup.host, wakeup.regist_key, wakeup.ps5);
            qCInfo(nax5SessionLog) << (sent ? "wakeup sent" : "wakeup failed")
                                   << nax5WakeupHostKindName(wakeup.host);
        }
    }
    // Product diagnostics are always sanitized, independently of operator preferences.
    if (!Nax5Runtime::operatorMode()) info.log_sanitize = true;
    beginDiagnosticReport();
    if (!nax5AddReportSource(nax5ReportQueueRoot(), diagnostic_report_id, info.log_file))
        qCWarning(nax5SessionLog) << "cannot attach stream log to diagnostic journal";
    backend->createSession(info);
    last_decoder = boolDecoderName(info.hw_decoder);
    last_resolution = QStringLiteral("%1x%2").arg(info.video_profile.width).arg(info.video_profile.height);
    last_bitrate_kbps = info.video_profile.bitrate ? QString::number(info.video_profile.bitrate) : QString();
    last_fps = info.video_profile.max_fps ? QString::number(info.video_profile.max_fps) : QString();
}

void Nax5SessionController::onReserveFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    if (!nax5AcceptAsync(generation, generation, reserve_request_id, request_id))
        return;
    reserve_request_id = 0;

    if (logoutIfUnauthenticated(result.error))
        return;

    if (result.error == Nax5SessionErrorNone && result.has_session)
    {
        applyAssignment(result);
        idempotency_key.clear();
        setError(Nax5SessionErrorNone);
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionReserveSucceeded));
        emitTelemetry(QStringLiteral("RESERVE_SUCCEEDED"));
        fetchConnection();
        return;
    }
    if (result.error == Nax5SessionErrorActiveSessionExists && result.has_session)
    {
        applyAssignment(result);
        idempotency_key.clear();
        setError(Nax5SessionErrorNone);
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionSyncedOccupied));
        fetchConnection();
        return;
    }

    idempotency_key.clear();
    clearAssignment();
    update_url = result.update_url;
    setError(result.error);
    if (result.error == Nax5SessionErrorInsufficientBalance)
    {
        setTopUpSuggested(true);
        if (auth)
            auth->refreshAccount();
    }
    if (result.error == Nax5SessionErrorNoCapacity || result.error == Nax5SessionErrorConsoleOffline
        || result.error == Nax5SessionErrorUserNotEligible
        || result.error == Nax5SessionErrorInsufficientBalance || result.error == Nax5SessionErrorSessionCooldown
        || result.error == Nax5SessionErrorUnauthenticated)
    {
        setStatusText(errorText(result.error));
        setState(nax5SessionReduce(session_state, (result.error == Nax5SessionErrorNoCapacity || result.error == Nax5SessionErrorConsoleOffline) ? Nax5GameSessionActionReserveNoCapacity : Nax5GameSessionActionReserveDenied));
        if (result.error == Nax5SessionErrorNoCapacity || result.error == Nax5SessionErrorConsoleOffline)
            startReservePause(result.retry_after_seconds);
        // No play time and the cooldown are expected answers, not failures worth a diagnostic report.
        if (result.error != Nax5SessionErrorUnauthenticated && result.error != Nax5SessionErrorInsufficientBalance
            && result.error != Nax5SessionErrorSessionCooldown && result.error != Nax5SessionErrorConsoleOffline)
            submitClientReport(Nax5ClientReportKindReserveFail);
        return;
    }
    setStatusText(errorText(result.error));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionReserveFailed));
    if (result.error == Nax5SessionErrorRateLimited)
    {
        // The server's pause after "no console" is still running: wait it out, nothing to report.
        startReservePause(result.retry_after_seconds);
        return;
    }
    submitClientReport(Nax5ClientReportKindReserveFail);
}

void Nax5SessionController::onCurrentFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    if (!nax5AcceptAsync(generation, generation, current_request_id, request_id))
        return;
    current_request_id = 0;
    if (!auth || !auth->authenticated())
        return;
    if (logoutIfUnauthenticated(result.error))
        return;
    if (result.error != Nax5SessionErrorNone)
    {
        if (awaiting_abort_current)
        {
            awaiting_abort_current = false;
            setError(result.error);
            setStatusText(errorText(result.error));
            setState(Nax5GameSessionStateError);
        }
        return;
    }
    if (!result.has_session)
    {
        if (operator_test_active || session_state == Nax5GameSessionStateConnecting || session_state == Nax5GameSessionStateActive)
            return;
        if (awaiting_abort_current)
        {
            awaiting_abort_current = false;
            if (nax5SessionShouldResetLocalAfterAbortCurrent(false))
                resetLocal();
            return;
        }
        discardMaterial();
        clearAssignment();
        if (nax5SessionHasAssignment(session_state) || session_state == Nax5GameSessionStateCancelling)
        {
            setStatusText(QString());
            setError(Nax5SessionErrorNone);
            setState(nax5SessionReduce(session_state, Nax5GameSessionActionSyncedEmpty));
        }
        return;
    }
    if (operator_test_active || session_state == Nax5GameSessionStateConnecting || session_state == Nax5GameSessionStateActive)
        return;
    applyAssignment(result);
    setError(Nax5SessionErrorNone);
    if (awaiting_abort_current)
    {
        awaiting_abort_current = false;
        if (session_state != Nax5GameSessionStateCancelling)
            setState(nax5SessionReduce(session_state, Nax5GameSessionActionReleaseClicked));
        dispatchTerminal(Nax5TerminalMutationCancel, false);
        return;
    }
    const Nax5GameSessionState previous = session_state;
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionSyncedOccupied));
    if (nax5SessionShouldFetchOnSyncedOccupied(previous))
        fetchConnection();
}

void Nax5SessionController::onCancelFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    if (ignore_cancel_result || shutdown_started)
        return;
    if (!nax5AcceptAsync(generation, generation, cancel_request_id, request_id)
        && request_id != pending_terminal.request_id)
        return;
    if (retryTerminalIfNeeded(result))
        return;
    cancel_request_id = 0;
    pending_terminal.mutation = Nax5TerminalMutationNone;
    discardMaterial();
    if (logoutIfUnauthenticated(result.error))
        return;
    if (result.error == Nax5SessionErrorNone || result.error == Nax5SessionErrorNotFound)
    {
        clearAssignment();
        setError(Nax5SessionErrorNone);
        setStatusText(QString());
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionCancelSucceeded));
        if (!shutdown_started)
            submitClientReport(Nax5ClientReportKindQuit);
        return;
    }
    setError(result.error);
    setStatusText(errorText(result.error));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionCancelFailed));
}

void Nax5SessionController::onConnectionFinished(quint64 request_id, const Nax5ConnectionParseResult &result)
{
    if (!nax5AcceptAsync(generation, generation, connection_request_id, request_id))
        return;
    connection_request_id = 0;
    if (logoutIfUnauthenticated(result.error))
        return;
    if (session_state != Nax5GameSessionStateFetchingConnection && session_state != Nax5GameSessionStateConnecting)
    {
        if (result.has_material)
        {
            Nax5ConnectionMaterial stale = result.material;
            stale.clear();
        }
        return;
    }
    if (result.error != Nax5SessionErrorNone || !result.has_material)
    {
        discardMaterial();
        setError(result.error == Nax5SessionErrorNone ? Nax5SessionErrorInvalidConnectionMaterial : result.error);
        setStatusText(errorText(result.error == Nax5SessionErrorNone ? Nax5SessionErrorInvalidConnectionMaterial : result.error));
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionConnectionFailed));
        if (isOperatorTest() && result.has_material)
            reportOperatorTestResult(false);
        else if (!isOperatorTest())
            reportFail();
        operator_test_active = false;
        return;
    }
    const bool operator_test = result.material.session_id == QLatin1String("operator-test");
    if (!operator_test && !nax5AcceptSessionIdentity(session_id, result.material.session_id) && !result.material.session_id.isEmpty())
    {
        discardMaterial();
        setError(Nax5SessionErrorInvalidConnectionMaterial);
        setStatusText(errorText(Nax5SessionErrorInvalidConnectionMaterial));
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionConnectionFailed));
        reportFail();
        return;
    }
    operator_test_active = operator_test;
    if (operator_test)
        session_id.clear();
    material = result.material;
    setError(Nax5SessionErrorNone);
    setStatusText(QStringLiteral("Подключаемся..."));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionConnectionReceived));
    emitTelemetry(QStringLiteral("CONNECTION_MATERIAL_RECEIVED"));
    startStream();
}

void Nax5SessionController::onChiakiSessionChanged(StreamSession *session)
{
    if (!session)
    {
        emit stateChanged();
        if (pending_start_stream)
            startStream();
        return;
    }
    diagnostic_stream = session;
    connect(session, &StreamSession::ConnectedChanged, this, &Nax5SessionController::onStreamTransportConnected, Qt::UniqueConnection);
    connect(session, &StreamSession::FfmpegFrameAvailable, this, &Nax5SessionController::onStreamFirstFrame,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(session, &StreamSession::SessionQuit, this, &Nax5SessionController::onStreamQuit, Qt::UniqueConnection);
    if (session->GetConnected())
        onStreamTransportConnected();
}

void Nax5SessionController::onStreamTransportConnected()
{
    if (stream_generation != generation)
        return;
    if (!backend || !backend->qmlSession() || !backend->qmlSession()->GetConnected())
        return;
    if (session_state != Nax5GameSessionStateConnecting)
        return;
    setStatusText(QStringLiteral("Запуск PS5..."));
}

void Nax5SessionController::onStreamFirstFrame()
{
    if (stream_generation != generation)
        return;
    // Emitted for every decoded frame: feeds the stalled-stream check in onDiagnosticTick().
    last_frame_clock.start();
    if (stream_first_frame_seen)
        return;
    if (!backend || !backend->qmlSession())
        return;
    if (session_state != Nax5GameSessionStateConnecting)
        return;
    stream_first_frame_seen = true;
    stream_was_connected = true;
    diagnostic_stream_connected = true;
    stream_connected_at = QDateTime::currentDateTime();
    diagnostic_clock.start();
    last_input_clock.start();
    network_diagnostics.start(diagnostic_session_id);
    stream_health.reset();
    if (!isOperatorTest())
    {
        path_probe.start(Nax5ApiConfig::baseUrl().host(), Nax5PathProbe::kDefaultPort);
        // Same probe towards the home agent next to the PS5, through the router's port forward.
        home_probe.start(material.host, Nax5PathProbe::kHomePort);
    }
    diagnostic_timer->start();
    sampleStreamStats();
    qCInfo(nax5SessionLog) << "first decoded frame, posting connected";
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionStreamConnected));
    setStatusText(QStringLiteral("Игра"));
    if (isOperatorTest())
    {
        reportOperatorTestResult(true);
        return;
    }
    emitTelemetry(QStringLiteral("STREAM_CONNECTED"));
    if (auth && !session_id.isEmpty())
        connected_request_id = api->markConnected(auth->sessionToken(), session_id);
    scheduleHeartbeat(kHeartbeatIntervalMs);
}

void Nax5SessionController::onConnectedFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    if (!nax5AcceptAsync(generation, generation, connected_request_id, request_id))
        return;
    connected_request_id = 0;
    if (logoutIfUnauthenticated(result.error))
        return;
    if (nax5ShouldRetryMarkConnected(session_state, shutdown_started, result.error) && auth && !session_id.isEmpty())
        connected_request_id = api->markConnected(auth->sessionToken(), session_id);
}

void Nax5SessionController::onHeartbeatFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    if (!nax5AcceptAsync(generation, generation, heartbeat_request_id, request_id))
        return;
    heartbeat_request_id = 0;
    if (logoutIfUnauthenticated(result.error))
        return;
    if (session_state != Nax5GameSessionStateActive)
        return;
    if (nax5HeartbeatSessionClosed(result.error))
    {
        endSessionClosedByBackend(result.reason);
        return;
    }
    if (result.error == Nax5SessionErrorNone)
        setRemainingSeconds(result.remaining_seconds);
    sampleStreamStats();
    scheduleHeartbeat(result.error == Nax5SessionErrorNone ? kHeartbeatIntervalMs : kHeartbeatRetryMs);
}

int Nax5SessionController::retrySeconds() const
{
    return reserve_backoff_timer->isActive() ? (reserve_backoff_timer->remainingTime() + 999) / 1000 : 0;
}

void Nax5SessionController::startReservePause(qint64 retry_after_seconds)
{
    reserve_backoff_timer->start(nax5ReserveRetryPauseMs(retry_after_seconds));
    retry_tick_timer->start();
    emit retryChanged();
    emit stateChanged();
}

void Nax5SessionController::applyCrashDecoderFallback()
{
#ifdef Q_OS_WIN
    if (!backend || !backend->chiakiSettings())
        return;
    const QDir dumps(nax5CrashDumpDir());
    for (const QString &name : dumps.entryList({QStringLiteral("NAX5-*.txt")}, QDir::Files))
    {
        QFile file(dumps.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QString module;
        for (const QByteArray &line : file.readAll().split('\n'))
            if (line.startsWith("fault_module="))
                module = QString::fromUtf8(line.mid(13)).trimmed();
        const QString current = backend->chiakiSettings()->GetHardwareDecoder();
        const QString next = nax5DecoderAfterCrash(current, module);
        if (next.isEmpty())
            continue;
        backend->chiakiSettings()->SetHardwareDecoder(next);
        qCWarning(nax5SessionLog) << "decoder switched after a crash in the video driver" << current << "->" << next;
        setStatusText(QStringLiteral("После сбоя видеодрайвера декодер переключён на Direct3D. Нажмите «Играть»."));
        return;
    }
#endif
}

QString Nax5SessionController::remainingText() const
{
    return remaining_seconds < 0 ? QString() : nax5FormatPlayTime(remaining_seconds);
}

void Nax5SessionController::setRemainingSeconds(qint64 seconds)
{
    const int threshold = nax5LowTimeThreshold(remaining_seconds, seconds);
    if (remaining_seconds != seconds)
    {
        remaining_seconds = seconds;
        emit playTimeChanged();
    }
    if (threshold != 0)
    {
        // Always emitted, even with the same text: the view shows it again.
        time_notice = nax5LowTimeNotice(threshold);
        emit timeNoticeChanged();
    }
}

void Nax5SessionController::setTopUpSuggested(bool suggested)
{
    if (top_up_suggested == suggested)
        return;
    top_up_suggested = suggested;
    emit playTimeChanged();
}

void Nax5SessionController::endSessionClosedByBackend(const QString &reason)
{
    // The backend already ended this session (admin, expiry) and may hand the
    // console to someone else: stop streaming instead of heartbeating a 404 forever.
    qCWarning(nax5SessionLog) << "heartbeat: session closed by backend, stopping stream";
    // New generation: onStreamQuit still uploads the quit report but posts no /end/.
    bumpGeneration();
    heartbeat_timer->stop();
    heartbeat_request_id = 0;
    discardMaterial();
    clearAssignment();
    setError(Nax5SessionErrorNone);
    setStatusText(nax5SessionClosedText(reason));
    setRemainingSeconds(-1);
    setTopUpSuggested(nax5SessionClosedForBalance(reason));
    if (auth)
        auth->refreshAccount();
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionStreamEnded));
    if (streamSessionAlive())
        backend->stopSession(false);
}

static Nax5SessionError errorFromQuitReason(ChiakiQuitReason reason)
{
    switch (reason) {
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
    case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED:
    case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
        return Nax5SessionErrorHostUnreachable;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN:
    case CHIAKI_QUIT_REASON_CTRL_UNKNOWN:
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN:
        return Nax5SessionErrorConnectionTimeout;
    default:
        return Nax5SessionErrorInvalidConnectionMaterial;
    }
}

void Nax5SessionController::onStreamQuit(ChiakiQuitReason reason, const QString &reason_str)
{
    if (sender() && diagnostic_stream && sender() != diagnostic_stream.data()) return;
    network_diagnostics.stop();
    path_probe.stop();
    home_probe.stop();
    sampleStreamStats();
    if (diagnostic_clock.isValid()) diagnostic_duration_ms = diagnostic_clock.elapsed();
    diagnostic_clock.invalidate();
    diagnostic_timer->stop();
    // Capture independently of /end/ acknowledgement and generation changes.
    // Delay one event-loop turn so upstream's deleteLater can flush its final log lines.
    const QString report_id = diagnostic_report_id;
    const QPointer<StreamSession> finished_stream = diagnostic_stream;
    QTimer::singleShot(0, this, [this, reason, report_id, finished_stream]() {
        if (diagnostic_report_id != report_id) return;
        if (finished_stream && !streamSessionAlive())
            QCoreApplication::sendPostedEvents(finished_stream.data(), QEvent::DeferredDelete);
        submitClientReport(chiaki_quit_reason_is_error(reason) ? Nax5ClientReportKindError : Nax5ClientReportKindQuit);
    });
    if (stream_generation != generation)
        return;
    if (unauth_logout_pending)
        return;
    pending_start_stream = false;
    heartbeat_timer->stop();
    heartbeat_request_id = 0;
    sampleStreamStats();
    const bool connected_this_play = stream_was_connected;
    const bool handshake_timeout = errorFromQuitReason(reason) == Nax5SessionErrorConnectionTimeout;
    qCInfo(nax5SessionLog) << "stream quit"
                           << "reason" << static_cast<int>(reason)
                           << (handshake_timeout ? "timeout" : "other")
                           << (connected_this_play ? "connected" : "never-connected")
                           << reason_str;
    discardMaterial();
    if (session_state == Nax5GameSessionStateIdle
        || session_state == Nax5GameSessionStateError
        || session_state == Nax5GameSessionStateCancelling
        || session_state == Nax5GameSessionStateEnding)
        return;
    const Nax5TerminalMutation mutation = nax5StreamQuitMutation(connected_this_play, isOperatorTest());
    if (connected_this_play)
    {
        if (mutation == Nax5TerminalMutationEnd)
            reportEnd();
        setState(nax5SessionReduce(session_state, Nax5GameSessionActionStreamEnded));
        // The console ended the stream itself: somebody chose rest mode or power off on it.
        const bool console_shut_down = reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN;
        setStatusText(stream_stalled ? QStringLiteral("Консоль перестала передавать изображение")
            : console_shut_down ? QStringLiteral("Консоль выключили или перевели в режим покоя. Пожалуйста, не выключайте её: после игры просто закройте лаунчер.")
            : QString());
        operator_test_active = false;
        clearAssignment();
        return;
    }
    const Nax5SessionError error = errorFromQuitReason(reason);
    setError(error);
    setStatusText(errorText(error));
    setState(nax5SessionReduce(session_state, Nax5GameSessionActionStreamFailed));
    if (isOperatorTest())
        reportOperatorTestResult(false);
    else
        reportFail();
    operator_test_active = false;
    clearAssignment();
}

void Nax5SessionController::reportFail()
{
    ignore_cancel_result = true;
    dispatchTerminal(Nax5TerminalMutationFail, true);
}

void Nax5SessionController::reportEnd()
{
    ignore_cancel_result = true;
    dispatchTerminal(Nax5TerminalMutationEnd, true);
}

void Nax5SessionController::onFailFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    handleTerminalFinished(request_id, result, Nax5TerminalMutationFail);
}

void Nax5SessionController::onEndFinished(quint64 request_id, const Nax5SessionParseResult &result)
{
    handleTerminalFinished(request_id, result, Nax5TerminalMutationEnd);
}

void Nax5SessionController::reportOperatorTestResult(bool passed)
{
    if (!auth || !Nax5Runtime::operatorMode() || operator_console_code.isEmpty())
        return;
    if (api->hasLane(Nax5ApiLaneOperator) && test_result_request_id != 0)
        return;
    QJsonObject body;
    body.insert(QStringLiteral("passed"), passed);
    test_result_request_id = api->operatorTestResult(
        auth->sessionToken(),
        operator_console_code,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void Nax5SessionController::provisionFromFields(const QString &console_code, int target, const QByteArray &regist_key, const QByteArray &morning, const QString &console_pin, const QString &nickname)
{
    if (!auth || !Nax5Runtime::operatorMode() || console_code.trimmed().isEmpty())
        return;
    if (api->hasLane(Nax5ApiLaneOperator))
        return;
    Nax5Runtime::setLastOperatorConsoleCode(console_code);
    setError(Nax5SessionErrorNone);
    QJsonObject body;
    body.insert(QStringLiteral("version"), Nax5ConnectionContractVersion);
    body.insert(QStringLiteral("target"), target);
    body.insert(QStringLiteral("nickname"), nickname);
    body.insert(QStringLiteral("registKey"), QString::fromLatin1(regist_key.toBase64()));
    body.insert(QStringLiteral("morning"), QString::fromLatin1(morning.toBase64()));
    body.insert(QStringLiteral("consolePin"), console_pin);
    operator_request_id = api->operatorProvision(auth->sessionToken(), console_code, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void Nax5SessionController::operatorActivate(const QString &console_code)
{
    if (!auth || !Nax5Runtime::operatorMode() || console_code.trimmed().isEmpty())
        return;
    if (api->hasLane(Nax5ApiLaneOperator))
        return;
    Nax5Runtime::setLastOperatorConsoleCode(console_code);
    setError(Nax5SessionErrorNone);
    operator_request_id = api->operatorActivate(auth->sessionToken(), console_code);
}

void Nax5SessionController::operatorTest(const QString &console_code)
{
    if (!auth || !Nax5Runtime::operatorMode() || !nax5SessionCanStartPlay(session_state) || console_code.trimmed().isEmpty())
        return;
    if (api->hasLane(Nax5ApiLaneConnection))
        return;
    bumpGeneration();
    operator_console_code = console_code.trimmed();
    operator_test_active = true;
    stream_was_connected = false;
    stream_first_frame_seen = false;
    session_id.clear();
    Nax5Runtime::setLastOperatorConsoleCode(operator_console_code);
    setError(Nax5SessionErrorNone);
    setStatusText(QStringLiteral("Проверяем подключение..."));
    setState(Nax5GameSessionStateFetchingConnection);
    connection_request_id = api->operatorTestConnection(auth->sessionToken(), operator_console_code);
}
