#pragma once

#include "nax5/session/nax5sessionerror.h"
#include "nax5/session/nax5sessionstate.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

enum Nax5TerminalMutation
{
    Nax5TerminalMutationNone = 0,
    Nax5TerminalMutationCancel,
    Nax5TerminalMutationFail,
    Nax5TerminalMutationEnd
};

Nax5TerminalMutation nax5ShutdownMutation(Nax5GameSessionState state, bool stream_was_connected);
Nax5TerminalMutation nax5StreamQuitMutation(bool stream_was_connected, bool operator_test);
Nax5TerminalMutation nax5ReleaseMutation(Nax5GameSessionState state, bool stream_was_connected);
bool nax5SessionTerminalBlocksPlay(Nax5TerminalMutation mutation);
bool nax5SessionNeedsQuitRelease(Nax5GameSessionState state);
bool nax5ShutdownNeedsCurrentSync(Nax5GameSessionState state);
bool nax5ShouldSendTerminalBeforeUnauthLogout(bool has_session_id, bool has_token, Nax5TerminalMutation mutation);
bool nax5PlayEligibilityOk(bool authenticated, bool email_verified, const QString &access_status);
bool nax5AcceptAsync(quint64 live_generation, quint64 event_generation, quint64 live_request_id, quint64 event_request_id);
bool nax5AcceptSessionIdentity(const QString &live_session_id, const QString &event_session_id);
int nax5TerminalRetryLimit();
// Network failures and 5xx (e.g. a backend deadlock) leave the session occupied; retry them.
bool nax5TerminalShouldRetry(Nax5SessionError error);
int nax5TerminalRetryDelayMs(int attempt);
// Pause before chiaki re-requests a Remote Play session that failed to start
// (e.g. the console is still releasing the previous one: "Remote is already in use").
int nax5StreamRetryDelayMs(qint64 elapsed_since_first_attempt_ms);
// A console that enters rest mode or drops off the network can stop streaming
// without a disconnect; chiaki then keeps the session open forever. The product
// ends a started stream once no frame has been decoded for this long.
int nax5StreamStallTimeoutMs();
bool nax5StreamStalled(bool first_frame_seen, qint64 ms_since_last_frame);
// Heartbeat answer meaning the backend already ended the session (admin, expiry).
bool nax5HeartbeatSessionClosed(Nax5SessionError error);
int nax5ShutdownGraceMs();
int nax5ShutdownReportGraceMs();
bool nax5MaySleepConsole(bool operator_mode);
bool nax5MayResumeAfterOsSleep(bool operator_mode);
bool nax5ShowsChiakiQuitDialog(bool operator_mode);
bool nax5OperatorHostConnectAllowed(bool operator_mode);
bool nax5StreamConnectedOnNewGeneration();
bool nax5StreamFirstFrameSeenOnNewGeneration();
bool nax5ShouldRetryMarkConnected(Nax5GameSessionState state, bool shutdown_started, Nax5SessionError error);
bool nax5ProductShouldWakeupBeforeCreateSession();
int nax5ProductWakeupSendsPerStartStream();
bool nax5OperatorConnectShouldWakeup(bool discovered, bool standby);
QString nax5WakeupHostKindName(const QString &host);

struct Nax5MaterialWakeup
{
    QString host;
    QByteArray regist_key;
    bool ps5;
    bool ready;

    Nax5MaterialWakeup()
        : ps5(false)
        , ready(false)
    {
    }
};

Nax5MaterialWakeup nax5MaterialWakeupCall(const QString &host, const QByteArray &regist_key, bool ps5);

// Write-ahead record of a terminal mutation (/end/, /fail/, /cancel/) that the
// server has not acknowledged yet. Saved before sending and cleared on a final
// answer, so a network drop, closed window or crash does not leave the session
// open on the server; the next login resends it before syncing the current one.
struct Nax5PersistedTerminal
{
    qint64 owner = 0;
    QString session_id;
    Nax5TerminalMutation mutation = Nax5TerminalMutationNone;
    QString saved_utc;
};

class QSettings;
void nax5SaveTerminal(QSettings &settings, const Nax5PersistedTerminal &terminal);
// Empty result if nothing is stored, it belongs to another account or is older than a day.
Nax5PersistedTerminal nax5LoadTerminal(QSettings &settings, qint64 owner);
// Clears only the record for session_id, so a newer session's record survives.
void nax5ClearTerminal(QSettings &settings, const QString &session_id);

// Play time for people: whole minutes, never rounded up ("1 ч 12 мин").
QString nax5FormatPlayTime(qint64 seconds);
// 300 or 60 when the time left has just dropped to five minutes or one minute; 0 otherwise.
int nax5LowTimeThreshold(qint64 previous_remaining, qint64 remaining);
QString nax5LowTimeNotice(int threshold);
// How long the notice stays over the stream.
int nax5LowTimeNoticeMs(int threshold);
// One line about the single console for the play panel; empty when the server says nothing.
QString nax5ConsoleStatusText(const QString &state, qint64 free_in_seconds, bool just_freed);
// What to tell the player when the server ended the session; `reason` comes from the closing answer.
QString nax5SessionClosedText(const QString &reason);
bool nax5SessionClosedForBalance(const QString &reason);

// A crash inside the GPU vendor's Vulkan driver while the Vulkan video decoder was in use: the fix that
// works in the field is the Direct3D decoder. Returns the decoder to switch to, or an empty string.
QString nax5DecoderAfterCrash(const QString &current_decoder, const QString &fault_module);
// How long «Играть» stays blocked after the server refused; the server's own pause when it tells one.
int nax5ReserveRetryPauseMs(qint64 retry_after_seconds);
