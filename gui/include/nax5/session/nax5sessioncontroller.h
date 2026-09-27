#pragma once

#include "nax5/connection/nax5connectionmaterial.h"
#include "nax5/nax5clientreport.h"
#include "nax5/nax5reportqueue.h"
#include "nax5/nax5processlog.h"
#include "nax5/nax5networkhint.h"
#include "nax5/nax5pathprobe.h"
#include "nax5/nax5streamhealth.h"
#include "nax5/session/nax5sessionerror.h"
#include "nax5/session/nax5sessionlifecycle.h"
#include "nax5/session/nax5sessionstate.h"

#include <chiaki/session.h>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

class Nax5ApiClient;
class Nax5AuthController;
class QmlBackend;
class StreamSession;
struct Nax5ConnectionParseResult;
struct Nax5SessionParseResult;
class QTimer;
class QThread;

class Nax5SessionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool reserving READ reserving NOTIFY stateChanged)
    Q_PROPERTY(bool reserved READ reserved NOTIFY stateChanged)
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged)
    Q_PROPERTY(bool playEnabled READ playEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool releaseEnabled READ releaseEnabled NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString consoleCode READ consoleCode NOTIFY assignmentChanged)
    Q_PROPERTY(QString consoleRegion READ consoleRegion NOTIFY assignmentChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY assignmentChanged)
    Q_PROPERTY(bool updateRequired READ updateRequired NOTIFY stateChanged)
    Q_PROPERTY(QString updateUrl READ updateUrl NOTIFY stateChanged)

public:
    explicit Nax5SessionController(Nax5AuthController *auth, QmlBackend *backend, QObject *parent = nullptr);
    ~Nax5SessionController() override;

    int state() const { return static_cast<int>(session_state); }
    bool reserving() const { return session_state == Nax5GameSessionStateReserving; }
    bool reserved() const { return nax5SessionHasAssignment(session_state); }
    bool cancelling() const
    {
        return session_state == Nax5GameSessionStateCancelling || session_state == Nax5GameSessionStateEnding;
    }
    bool playEnabled() const;
    bool releaseEnabled() const { return nax5SessionCanRelease(session_state); }
    QString statusText() const { return status_text; }
    QString errorMessage() const { return error_message; }
    QString consoleCode() const { return console_code; }
    QString consoleRegion() const { return console_region; }
    QString sessionId() const { return session_id; }
    bool updateRequired() const { return current_error == Nax5SessionErrorClientUpdateRequired; }
    QString updateUrl() const { return update_url; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void release();
    Q_INVOKABLE void releaseAndLogout();
    Q_INVOKABLE void saveReport();
    Q_INVOKABLE void operatorActivate(const QString &console_code);
    Q_INVOKABLE void operatorTest(const QString &console_code);
    void provisionFromFields(const QString &console_code, int target, const QByteArray &regist_key, const QByteArray &morning, const QString &console_pin, const QString &nickname);
    void prepareShutdown();

signals:
    void stateChanged();
    void statusTextChanged();
    void errorMessageChanged();
    void assignmentChanged();
    void diagnosticCaptureRequested(const QString &root, qint64 owner);
    void diagnosticCaptureFinished(bool ok);

private:
    struct PendingTerminal
    {
        Nax5TerminalMutation mutation;
        QString token;
        QString session_id;
        quint64 generation;
        quint64 request_id;
        int attempts;
        bool silent;
        bool resumed = false;  // resent from the write-ahead record at login
    };

    void setState(Nax5GameSessionState next);
    void setStatusText(const QString &text);
    void setError(Nax5SessionError error);
    void clearAssignment();
    void discardMaterial();
    void applyAssignment(const Nax5SessionParseResult &result);
    void resetLocal();
    QString errorText(Nax5SessionError error) const;
    bool isOperatorTest() const;
    void reportOperatorTestResult(bool passed);
    void syncCurrent();
    void scheduleLeaseSync(const QString &lease_expires_at);
    void scheduleHeartbeat(int delay_ms);
    void sendHeartbeat();
    void onAuthStateChanged();
    void onReserveFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onCurrentFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onCancelFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onConnectionFinished(quint64 request_id, const Nax5ConnectionParseResult &result);
    void onConnectedFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onHeartbeatFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onFailFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void onEndFinished(quint64 request_id, const Nax5SessionParseResult &result);
    void fetchConnection();
    void startStream();
    void onChiakiSessionChanged(StreamSession *session);
    void onStreamTransportConnected();
    void onStreamFirstFrame();
    void onStreamQuit(ChiakiQuitReason reason, const QString &reason_str);
    void reportFail();
    void reportEnd();
    void dispatchTerminal(Nax5TerminalMutation mutation, bool silent);
    void sendPendingTerminal();
    void handleTerminalFinished(quint64 request_id, const Nax5SessionParseResult &result, Nax5TerminalMutation mutation);
    bool retryTerminalIfNeeded(const Nax5SessionParseResult &result);
    bool resumePersistedTerminal();
    bool logoutIfUnauthenticated(Nax5SessionError error);
    void finishUnauthLogout();
    void abortReserveAndSyncCurrent();
    void waitForShutdownIo();
    void waitForShutdownReport();
    void submitClientReport(Nax5ClientReportKind kind);
    void flushPendingClientReport();
    void onClientReportFinished(quint64 request_id, int http_status);
    void emitTelemetry(const QString &event_type, const QJsonObject &metadata = QJsonObject());
    void flushTelemetry();
    void sampleStreamStats();
    void onDiagnosticTick();
    void beginDiagnosticReport();
    void queueCrashDumps();
    void serviceDiagnosticQueue();
    Nax5BuildInfoSnapshot buildInfoSnapshot();
    bool streamSessionAlive() const;
    quint64 bumpGeneration();
    QString liveToken() const;

    QPointer<Nax5AuthController> auth;
    QPointer<QmlBackend> backend;
    QPointer<StreamSession> diagnostic_stream;
    Nax5NetworkDiagnostics network_diagnostics;
    Nax5PathProbe path_probe;
    Nax5PathProbe home_probe;
    Nax5StreamHealth stream_health;
    Nax5SessionTotals session_totals;
    Nax5ApiClient *api;
    QTimer *lease_timer;
    QTimer *heartbeat_timer;
    QTimer *reserve_backoff_timer;
    QTimer *telemetry_flush_timer;
    QTimer *diagnostic_timer;
    QTimer *report_queue_timer;
    QThread *report_worker;
    bool report_capture_busy = false;
    QString diagnostic_report_id;
    QString diagnostic_session_id;
    qint64 diagnostic_owner = 0;
    qint64 diagnostic_process_offset = 0;
    qint64 report_retry_at = 0;
    int report_retry_count = 0;
    bool diagnostic_finalized = false;
    bool diagnostic_stream_connected = false;
    Nax5QueuedReportPart uploading_part;
    QElapsedTimer diagnostic_clock;
    qint64 diagnostic_duration_ms = 0;
    qint64 diagnostic_last_sample_ms = 0;
    Nax5GameSessionState session_state;
    QString status_text;
    QString error_message;
    Nax5SessionError current_error = Nax5SessionErrorNone;
    QString update_url;
    QString console_code;
    QString console_region;
    QString session_id;
    QString operator_console_code;
    QString idempotency_key;
    Nax5ConnectionMaterial material;
    PendingTerminal pending_terminal;
    quint64 generation;
    quint64 stream_generation;
    quint64 reserve_request_id;
    quint64 current_request_id;
    quint64 cancel_request_id;
    quint64 connection_request_id;
    quint64 connected_request_id;
    quint64 heartbeat_request_id;
    quint64 test_result_request_id;
    quint64 operator_request_id;
    bool ignore_cancel_result;
    bool stream_was_connected;
    bool stream_first_frame_seen;
    bool operator_test_active;
    bool shutdown_started;
    bool awaiting_abort_current;
    bool pending_start_stream;
    bool unauth_logout_pending;
    quint64 client_report_request_id;
    QString last_decoder;
    QString last_resolution;
    QString last_bitrate_kbps;
    QString last_fps;
    QDateTime stream_connected_at;
    double stream_max_packet_loss;
    double last_avg_packet_loss;
    int last_frames_lost;
    qint64 last_measured_bitrate_kbps;
    int last_dropped_frames;
};
