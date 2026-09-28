#include "nax5/session/nax5sessionlifecycle.h"

#include <QDateTime>
#include <QSettings>
#include <QString>
#include <QStringList>

Nax5TerminalMutation nax5ShutdownMutation(Nax5GameSessionState state, bool stream_was_connected)
{
    if (stream_was_connected || state == Nax5GameSessionStateActive || state == Nax5GameSessionStateEnding)
        return Nax5TerminalMutationEnd;
    if (state == Nax5GameSessionStateConnecting)
        return Nax5TerminalMutationFail;
    if (state == Nax5GameSessionStateReserving
        || state == Nax5GameSessionStateFetchingConnection
        || state == Nax5GameSessionStateCancelling)
        return Nax5TerminalMutationCancel;
    return Nax5TerminalMutationNone;
}

Nax5TerminalMutation nax5StreamQuitMutation(bool stream_was_connected, bool operator_test)
{
    if (operator_test)
        return Nax5TerminalMutationNone;
    if (stream_was_connected)
        return Nax5TerminalMutationEnd;
    return Nax5TerminalMutationFail;
}

Nax5TerminalMutation nax5ReleaseMutation(Nax5GameSessionState state, bool stream_was_connected)
{
    if (state == Nax5GameSessionStateActive || stream_was_connected)
        return Nax5TerminalMutationEnd;
    if (state == Nax5GameSessionStateConnecting)
        return Nax5TerminalMutationFail;
    if (state == Nax5GameSessionStateReserving
        || state == Nax5GameSessionStateFetchingConnection
        || state == Nax5GameSessionStateCancelling)
        return Nax5TerminalMutationCancel;
    return Nax5TerminalMutationNone;
}

bool nax5SessionTerminalBlocksPlay(Nax5TerminalMutation mutation)
{
    return mutation == Nax5TerminalMutationEnd || mutation == Nax5TerminalMutationFail;
}

bool nax5SessionNeedsQuitRelease(Nax5GameSessionState state)
{
    return state == Nax5GameSessionStateReserving
        || state == Nax5GameSessionStateFetchingConnection
        || state == Nax5GameSessionStateConnecting
        || state == Nax5GameSessionStateActive
        || state == Nax5GameSessionStateEnding
        || state == Nax5GameSessionStateCancelling;
}

bool nax5ShutdownNeedsCurrentSync(Nax5GameSessionState state)
{
    return state == Nax5GameSessionStateReserving;
}

bool nax5ShouldSendTerminalBeforeUnauthLogout(bool has_session_id, bool has_token, Nax5TerminalMutation mutation)
{
    return has_session_id && has_token && mutation != Nax5TerminalMutationNone;
}

bool nax5PlayEligibilityOk(bool authenticated, bool email_verified, const QString &access_status)
{
    return authenticated && email_verified && access_status == QLatin1String("ACTIVE");
}

bool nax5AcceptAsync(quint64 live_generation, quint64 event_generation, quint64 live_request_id, quint64 event_request_id)
{
    return event_request_id != 0
        && live_request_id == event_request_id
        && live_generation == event_generation;
}

bool nax5AcceptSessionIdentity(const QString &live_session_id, const QString &event_session_id)
{
    if (event_session_id.isEmpty())
        return false;
    return live_session_id == event_session_id;
}

int nax5TerminalRetryLimit()
{
    return 4;
}

bool nax5TerminalShouldRetry(Nax5SessionError error)
{
    return error == Nax5SessionErrorNetworkError || error == Nax5SessionErrorServerError;
}

int nax5StreamRetryDelayMs(qint64 elapsed_since_first_attempt_ms)
{
    // The console usually frees its previous session within a few seconds:
    // retry twice a second at first, then once a second.
    return elapsed_since_first_attempt_ms < 3000 ? 500 : 1000;
}

int nax5TerminalRetryDelayMs(int attempt)
{
    return 300 * qBound(1, attempt, 4);
}

int nax5StreamStallTimeoutMs()
{
    // A live PS5 stream decodes ~60 frames/s even on a static screen.
    return 60 * 1000;
}

bool nax5StreamStalled(bool first_frame_seen, qint64 ms_since_last_frame)
{
    return first_frame_seen && ms_since_last_frame > nax5StreamStallTimeoutMs();
}

bool nax5HeartbeatSessionClosed(Nax5SessionError error)
{
    return error == Nax5SessionErrorNotFound;
}

int nax5ShutdownGraceMs()
{
    return 400;
}

int nax5ShutdownReportGraceMs()
{
    return 10000;
}

bool nax5MaySleepConsole(bool operator_mode)
{
    return operator_mode;
}

bool nax5MayResumeAfterOsSleep(bool operator_mode)
{
    return operator_mode;
}

bool nax5ShowsChiakiQuitDialog(bool operator_mode)
{
    return operator_mode;
}

bool nax5OperatorHostConnectAllowed(bool operator_mode)
{
    return operator_mode;
}

bool nax5StreamConnectedOnNewGeneration()
{
    return false;
}

bool nax5StreamFirstFrameSeenOnNewGeneration()
{
    return false;
}

bool nax5ShouldRetryMarkConnected(Nax5GameSessionState state, bool shutdown_started, Nax5SessionError error)
{
    if (shutdown_started || error != Nax5SessionErrorNetworkError)
        return false;
    return state == Nax5GameSessionStateActive || state == Nax5GameSessionStateConnecting;
}

bool nax5ProductShouldWakeupBeforeCreateSession()
{
    return true;
}

int nax5ProductWakeupSendsPerStartStream()
{
    return 1;
}

bool nax5OperatorConnectShouldWakeup(bool discovered, bool standby)
{
    return discovered && standby;
}

QString nax5WakeupHostKindName(const QString &host)
{
    const QString trimmed = host.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("unknown");
    if (trimmed.contains(QLatin1Char(':')))
    {
        const QString lower = trimmed.toLower();
        if (lower == QLatin1String("::1")
            || lower.startsWith(QLatin1String("fe80:"))
            || lower.startsWith(QLatin1String("fc"))
            || lower.startsWith(QLatin1String("fd")))
            return QStringLiteral("lan");
        return QStringLiteral("wan");
    }
    const QStringList parts = trimmed.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return QStringLiteral("wan");
    int octet[4];
    for (int i = 0; i < 4; ++i)
    {
        bool ok = false;
        octet[i] = parts.at(i).toInt(&ok);
        if (!ok || octet[i] < 0 || octet[i] > 255)
            return QStringLiteral("wan");
    }
    if (octet[0] == 10
        || octet[0] == 127
        || (octet[0] == 192 && octet[1] == 168)
        || (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31)
        || (octet[0] == 169 && octet[1] == 254))
        return QStringLiteral("lan");
    return QStringLiteral("wan");
}

Nax5MaterialWakeup nax5MaterialWakeupCall(const QString &host, const QByteArray &regist_key, bool ps5)
{
    Nax5MaterialWakeup call;
    call.host = host.trimmed();
    call.regist_key = regist_key;
    call.ps5 = ps5;
    call.ready = !call.host.isEmpty() && !call.regist_key.isEmpty();
    return call;
}

namespace {
const char *kTerminalGroup = "nax5/pending_terminal";
}

void nax5SaveTerminal(QSettings &settings, const Nax5PersistedTerminal &terminal)
{
    if (terminal.owner <= 0 || terminal.session_id.isEmpty() || terminal.mutation == Nax5TerminalMutationNone)
        return;
    settings.beginGroup(QString::fromLatin1(kTerminalGroup));
    settings.setValue(QStringLiteral("owner"), terminal.owner);
    settings.setValue(QStringLiteral("session_id"), terminal.session_id);
    settings.setValue(QStringLiteral("mutation"), static_cast<int>(terminal.mutation));
    settings.setValue(QStringLiteral("saved_utc"), terminal.saved_utc.isEmpty()
        ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : terminal.saved_utc);
    settings.endGroup();
    settings.sync();
}

Nax5PersistedTerminal nax5LoadTerminal(QSettings &settings, qint64 owner)
{
    Nax5PersistedTerminal out;
    settings.beginGroup(QString::fromLatin1(kTerminalGroup));
    out.owner = settings.value(QStringLiteral("owner")).toLongLong();
    out.session_id = settings.value(QStringLiteral("session_id")).toString();
    const int mutation = settings.value(QStringLiteral("mutation")).toInt();
    out.saved_utc = settings.value(QStringLiteral("saved_utc")).toString();
    settings.endGroup();
    const QDateTime saved = QDateTime::fromString(out.saved_utc, Qt::ISODate);
    const bool known = mutation == Nax5TerminalMutationCancel || mutation == Nax5TerminalMutationFail
        || mutation == Nax5TerminalMutationEnd;
    if (owner <= 0 || out.owner != owner || out.session_id.isEmpty() || !known || !saved.isValid()
        || saved.secsTo(QDateTime::currentDateTimeUtc()) > 24 * 3600)
        return {};
    out.mutation = static_cast<Nax5TerminalMutation>(mutation);
    return out;
}

void nax5ClearTerminal(QSettings &settings, const QString &session_id)
{
    settings.beginGroup(QString::fromLatin1(kTerminalGroup));
    if (settings.value(QStringLiteral("session_id")).toString() == session_id)
        settings.remove(QString());
    settings.endGroup();
    settings.sync();
}
