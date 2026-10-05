#include "nax5/nax5apiconfig.h"
#include "nax5/nax5apilane.h"
#include "nax5/nax5operatorhost.h"
#include "nax5/session/nax5sessionerror.h"
#include "nax5/session/nax5sessionlifecycle.h"
#include "nax5/session/nax5sessionparser.h"
#include "nax5/session/nax5sessionstate.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

static int g_failed = 0;

static void expect(bool condition, const char *name)
{
    if (condition)
        std::printf("ok %s\n", name);
    else
    {
        std::fprintf(stderr, "FAIL %s\n", name);
        g_failed++;
    }
}

static const char *kReserved =
    "{\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"RESERVED\","
    "\"reservedAt\":\"2026-09-09T19:00:00Z\",\"leaseExpiresAt\":\"2026-09-09T19:02:00Z\"},"
    "\"console\":{\"code\":\"PS5-439\",\"region\":\"Moscow\"}}";

static void test_reserve_success()
{
    const Nax5SessionParseResult result = nax5ParseReserveResponse(200, kReserved);
    expect(result.error == Nax5SessionErrorNone, "reserve success");
    expect(result.has_session, "reserve has session");
    expect(result.has_console, "reserve has console");
    expect(result.session.id == QStringLiteral("11111111-1111-4111-8111-111111111111"), "reserve id");
    expect(result.session.status == QStringLiteral("RESERVED"), "reserve status");
    expect(result.console.code == QStringLiteral("PS5-439"), "reserve console");
    expect(result.console.region == QStringLiteral("Moscow"), "reserve region");
}

static void test_no_capacity()
{
    const char *body = "{\"code\":\"NO_CAPACITY\",\"message\":\"No console is currently available.\"}";
    const Nax5SessionParseResult result = nax5ParseReserveResponse(409, body);
    expect(result.error == Nax5SessionErrorNoCapacity, "no capacity");
    expect(!result.has_session, "no capacity has no session");
}

static void test_eligibility_denied()
{
    const char *body = "{\"code\":\"USER_NOT_ELIGIBLE\",\"message\":\"denied\"}";
    expect(nax5ParseReserveResponse(403, body).error == Nax5SessionErrorUserNotEligible, "eligibility");
}

static void test_play_time_answers()
{
    const Nax5SessionParseResult denied = nax5ParseReserveResponse(403,
        "{\"code\":\"INSUFFICIENT_BALANCE\",\"message\":\"x\",\"balanceSeconds\":30}");
    expect(denied.error == Nax5SessionErrorInsufficientBalance && denied.balance_seconds == 30, "no play time is its own error");
    expect(nax5ParseReserveResponse(429, "{\"code\":\"RATE_LIMITED\",\"reason\":\"SESSION_COOLDOWN\"}").error
               == Nax5SessionErrorSessionCooldown, "cooldown after the shared limit");
    expect(nax5ParseReserveResponse(429, "{\"code\":\"RATE_LIMITED\"}").error == Nax5SessionErrorRateLimited, "plain 429 unchanged");

    const char *beat =
        "{\"session\":{\"id\":\"s\",\"status\":\"ACTIVE\",\"reservedAt\":\"2026-10-05T10:00:00Z\"},"
        "\"console\":{\"code\":\"PS5\",\"region\":\"MSK\"},\"remainingSeconds\":299}";
    expect(nax5ParseCancelResponse(200, beat).remaining_seconds == 299, "heartbeat carries the time left");
    const char *old_beat =
        "{\"session\":{\"id\":\"s\",\"status\":\"ACTIVE\",\"reservedAt\":\"2026-10-05T10:00:00Z\"},"
        "\"console\":{\"code\":\"PS5\",\"region\":\"MSK\"},\"remainingSeconds\":null}";
    expect(nax5ParseCancelResponse(200, old_beat).remaining_seconds == -1, "no time left reported while nothing is charged");

    const Nax5SessionParseResult closed = nax5ParseCancelResponse(404,
        "{\"code\":\"SESSION_NOT_FOUND\",\"message\":\"x\",\"reason\":\"BALANCE_EXHAUSTED\"}");
    expect(closed.error == Nax5SessionErrorNotFound && nax5SessionClosedForBalance(closed.reason), "closing answer names the reason");
    expect(!nax5SessionClosedForBalance(QStringLiteral("SESSION_LIMIT_REACHED")), "shared limit is not a balance stop");
    expect(nax5SessionClosedText(QString()) == QStringLiteral("Сессия завершена сервером"), "unknown reason keeps the old text");

    expect(nax5FormatPlayTime(4320) == QStringLiteral("1 ч 12 мин"), "format hours and minutes");
    expect(nax5FormatPlayTime(3600) == QStringLiteral("1 ч"), "format whole hours");
    expect(nax5FormatPlayTime(599) == QStringLiteral("9 мин"), "format never rounds up");
    expect(nax5LowTimeThreshold(320, 300) == 300, "five minute warning");
    expect(nax5LowTimeThreshold(300, 280) == 0, "five minute warning only once");
    expect(nax5LowTimeThreshold(80, 60) == 60, "one minute warning");
    expect(nax5LowTimeThreshold(-1, 200) == 300, "a session that starts short warns at once");
    expect(nax5LowTimeThreshold(-1, 4000) == 0, "no warning with plenty of time");
    expect(nax5LowTimeThreshold(100, -1) == 0, "no warning when time is not reported");
    expect(nax5ConsoleStatusText(QStringLiteral("busy"), 900, false) == QStringLiteral("Консоль занята. Освободится через 15 мин"), "busy console says when it gets free");
    expect(nax5ConsoleStatusText(QStringLiteral("busy"), 10800, false) == QStringLiteral("Консоль занята. Освободится через 3 ч"), "long wait is said as it is");
    expect(nax5ConsoleStatusText(QStringLiteral("busy"), -1, false) == QStringLiteral("Консоль занята другим игроком"), "busy without an estimate");
    expect(nax5ConsoleStatusText(QStringLiteral("free"), -1, true).contains(QStringLiteral("освободилась")), "freed console is announced");
    expect(nax5ConsoleStatusText(QString(), -1, false).isEmpty(), "old backend: no console line");
}

static void test_build16_decisions()
{
    const QString intel = QStringLiteral("C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\iigd_dch.inf_amd64_1bb5\\igvk64.dll");
    expect(nax5DecoderAfterCrash(QStringLiteral("auto"), intel) == QStringLiteral("d3d11va"), "Intel Vulkan driver crash switches auto to d3d11va");
    expect(nax5DecoderAfterCrash(QStringLiteral("vulkan"), QStringLiteral("C:/x/amdvlk64.dll")) == QStringLiteral("d3d11va"), "AMD Vulkan driver crash switches vulkan");
    expect(nax5DecoderAfterCrash(QStringLiteral("auto"), QStringLiteral("C:\\Windows\\SYSTEM32\\ntdll.dll")).isEmpty(), "a crash elsewhere leaves the decoder alone");
    expect(nax5DecoderAfterCrash(QStringLiteral("d3d11va"), intel).isEmpty(), "already on d3d11va: nothing to do");
    expect(nax5DecoderAfterCrash(QStringLiteral("none"), intel).isEmpty(), "software decoding chosen by the player is kept");
    expect(nax5DecoderAfterCrash(QStringLiteral("auto"), QString()).isEmpty(), "no module, no switch");

    expect(nax5ReserveRetryPauseMs(-1) == 60000, "default pause matches the server's minute");
    expect(nax5ReserveRetryPauseMs(20) == 20000, "the server's own pause wins");
    expect(nax5ReserveRetryPauseMs(100000) == 600000, "an absurd pause is capped");
    const Nax5SessionParseResult busy = nax5ParseReserveResponse(409, "{\"code\":\"NO_CAPACITY\",\"retryAfterSeconds\":45}");
    expect(busy.error == Nax5SessionErrorNoCapacity && busy.retry_after_seconds == 45, "no capacity carries the pause");
    expect(nax5ParseReserveResponse(429, "{\"code\":\"RATE_LIMITED\",\"retryAfterSeconds\":12}").retry_after_seconds == 12, "429 carries the pause");
    expect(nax5ParseReserveResponse(409, "{\"code\":\"NO_CAPACITY\"}").retry_after_seconds == -1, "older server: no pause sent");
    const Nax5SessionParseResult off = nax5ParseReserveResponse(409, "{\"code\":\"CONSOLE_OFFLINE\",\"retryAfterSeconds\":60}");
    expect(off.error == Nax5SessionErrorConsoleOffline && off.retry_after_seconds == 60, "a switched-off console is its own answer");
}

static void test_unauthenticated()
{
    expect(nax5ParseReserveResponse(401, "{\"code\":\"UNAUTHENTICATED\"}").error == Nax5SessionErrorUnauthenticated, "401");
}

static void test_required_update()
{
    const Nax5SessionParseResult result = nax5ParseReserveResponse(426,
        "{\"code\":\"CLIENT_UPDATE_REQUIRED\",\"minimumVersion\":\"alpha-0.5-build-9\","
        "\"updateUrl\":\"https://cloudgta6.com/account/\"}");
    expect(result.error == Nax5SessionErrorClientUpdateRequired, "required update error");
    expect(result.minimum_version == QStringLiteral("alpha-0.5-build-9"), "required update minimum version");
    expect(result.update_url == QStringLiteral("https://cloudgta6.com/account/"), "required update url");
}

static void test_conflict_and_rate_limit_and_server()
{
    expect(nax5ParseReserveResponse(409, "{\"code\":\"ACTIVE_SESSION_EXISTS\",\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"RESERVED\",\"reservedAt\":\"2026-09-09T19:00:00Z\",\"leaseExpiresAt\":\"2026-09-09T19:02:00Z\"},\"console\":{\"code\":\"PS5-439\",\"region\":\"Moscow\"}}").error == Nax5SessionErrorActiveSessionExists, "active exists");
    expect(nax5ParseReserveResponse(429, "{\"status\":429}").error == Nax5SessionErrorRateLimited, "429");
    expect(nax5ParseReserveResponse(500, "internal").error == Nax5SessionErrorServerError, "500");
}

static void test_malformed_and_timeout()
{
    expect(nax5ParseReserveResponse(200, "not-json").error == Nax5SessionErrorInvalidResponse, "malformed");
    expect(nax5ParseReserveResponse(200, "{\"session\":{}}").error == Nax5SessionErrorInvalidResponse, "incomplete");
    expect(nax5MapSessionHttpError(0, true, false) == Nax5SessionErrorNetworkError, "timeout");
    expect(nax5MapSessionHttpError(0, false, true) == Nax5SessionErrorNetworkError, "offline");
}

static void test_current_none_and_present()
{
    const Nax5SessionParseResult empty = nax5ParseCurrentResponse(200, "{\"session\":null}");
    expect(empty.error == Nax5SessionErrorNone, "current none error");
    expect(!empty.has_session, "current none");
    const Nax5SessionParseResult present = nax5ParseCurrentResponse(200, kReserved);
    expect(present.error == Nax5SessionErrorNone, "current present");
    expect(present.console.code == QStringLiteral("PS5-439"), "current console");
}

static void test_cancel_success_and_failure()
{
    const char *cancelled =
        "{\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"CANCELLED\","
        "\"reservedAt\":\"2026-09-09T19:00:00Z\",\"leaseExpiresAt\":null},"
        "\"console\":{\"code\":\"PS5-439\",\"region\":\"Moscow\"}}";
    const Nax5SessionParseResult ok = nax5ParseCancelResponse(200, cancelled);
    expect(ok.error == Nax5SessionErrorNone, "cancel ok");
    expect(ok.session.status == QStringLiteral("CANCELLED"), "cancel status");
    expect(nax5ParseCancelResponse(403, "{\"code\":\"SESSION_NOT_OWNED\"}").error == Nax5SessionErrorForbidden, "cancel forbidden");
}

static void test_state_transitions_and_double_click()
{
    expect(nax5SessionCanStartPlay(Nax5GameSessionStateIdle), "idle can play");
    expect(nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionPlayClicked) == Nax5GameSessionStateReserving, "play");
    expect(!nax5SessionCanStartPlay(Nax5GameSessionStateReserving), "busy cannot play");
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionPlayClicked) == Nax5GameSessionStateReserving, "double click ignored");
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionReserveSucceeded) == Nax5GameSessionStateFetchingConnection, "fetching");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionConnectionReceived) == Nax5GameSessionStateConnecting, "connecting");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionStreamConnected) == Nax5GameSessionStateActive, "active");
    expect(nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionStreamConnected) == Nax5GameSessionStateIdle, "stale connected after cancel");
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionReserveNoCapacity) == Nax5GameSessionStateIdle, "no capacity idle");
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionReserveFailed) == Nax5GameSessionStateError, "error");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionStreamFailed) == Nax5GameSessionStateError, "failed stream retryable");
    expect(nax5SessionCanStartPlay(Nax5GameSessionStateError), "retry from error");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateCancelling, "release");
    expect(nax5SessionReduce(Nax5GameSessionStateCancelling, Nax5GameSessionActionCancelSucceeded) == Nax5GameSessionStateIdle, "cancelled idle");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionSyncedEmpty) == Nax5GameSessionStateIdle, "stale idle");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionConnectionReceived) == Nax5GameSessionStateConnecting, "connection retry stays connecting");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateEnding, "connecting release ends");
    expect(nax5SessionReduce(Nax5GameSessionStateActive, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateEnding, "active release ends");
    expect(nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionConnectionReceived) == Nax5GameSessionStateIdle, "stale connection after logout");
    expect(nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionSyncedOccupied) == Nax5GameSessionStateFetchingConnection, "leftover occupying");
    expect(nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateIdle), "idle leftover fetches");
    expect(nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateError), "error leftover fetches");
    expect(!nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateConnecting), "live leftover does not refetch");
    expect(!nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateActive), "active leftover does not refetch");
    expect(!nax5SessionCanStartPlay(Nax5GameSessionStateFetchingConnection), "leftover occupying play busy");
    expect(nax5SessionCanRelease(Nax5GameSessionStateFetchingConnection), "leftover occupying can release");
    expect(nax5SessionReduce(Nax5GameSessionStateEnding, Nax5GameSessionActionCancelFailed) == Nax5GameSessionStateError, "end fail after local stop");
    expect(nax5SessionReduce(Nax5GameSessionStateEnding, Nax5GameSessionActionCancelFailed) != Nax5GameSessionStateActive, "end fail is not active");
    expect(nax5SessionCanStartPlay(Nax5GameSessionStateError), "play after failed end");
    expect(nax5SessionReduce(Nax5GameSessionStateCancelling, Nax5GameSessionActionCancelFailed) == Nax5GameSessionStateFetchingConnection, "cancel fail keeps assignment");
}

static void test_user_facing_errors_and_no_leak()
{
    const QString no_capacity = nax5SessionErrorMessage(Nax5SessionErrorNoCapacity);
    expect(no_capacity.contains(QStringLiteral("занята")), "capacity message");
    expect(!no_capacity.contains(QStringLiteral("HTTP")), "no http");
    expect(!no_capacity.contains(QStringLiteral("409")), "no status code");
    expect(!nax5SessionPayloadLooksLeaky(kReserved), "clean payload");
    expect(nax5SessionPayloadLooksLeaky("{\"public_host\":\"1.2.3.4\",\"regist_key\":\"aa\"}"), "leaky payload");
    expect(nax5SessionErrorMessage(Nax5SessionErrorInvalidConnectionMaterial) == QStringLiteral("Не удалось подключиться к консоли."), "product generic");
    expect(nax5SessionErrorMessage(Nax5SessionErrorHostNotConfigured) == QStringLiteral("Не удалось подключиться к консоли."), "product host generic");
    expect(nax5SessionErrorMessage(Nax5SessionErrorHostNotConfigured, true) == QStringLiteral("Test failed: console host not configured"), "operator host");
    expect(nax5SessionErrorMessage(Nax5SessionErrorHostUnreachable, true) == QStringLiteral("Test failed: PS5 unreachable"), "operator unreachable");
    expect(nax5SessionErrorMessage(Nax5SessionErrorInvalidConnectionMaterial, true).contains(QStringLiteral("authentication")), "operator auth");
    expect(nax5SessionErrorMessage(Nax5SessionErrorConnectionTimeout, true) == QStringLiteral("Test failed: timeout"), "operator timeout");
    expect(!nax5SessionErrorMessage(Nax5SessionErrorInvalidConnectionMaterial, true).contains(QStringLiteral("regist")), "no regist");
    expect(!nax5SessionErrorMessage(Nax5SessionErrorNone, true).contains(QStringLiteral("Test failed")), "cleared error");
}

static void test_user_agent_version()
{
    expect(Nax5ApiConfig::userAgent().startsWith(QStringLiteral("NAX5/0.4")), "user agent 0.4");
}

static void test_request_lanes_do_not_abort_unrelated()
{
    expect(nax5ApiLaneAllowsConcurrent(Nax5ApiLaneTerminal), "terminal concurrent");
    expect(!nax5ApiLaneAllowsConcurrent(Nax5ApiLaneReserve), "reserve exclusive");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneConnection, Nax5ApiLaneTerminal), "end does not abort connection");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneTerminal, Nax5ApiLaneTerminal), "connected does not abort end");
    expect(nax5ApiShouldAbortExisting(Nax5ApiLaneReserve, Nax5ApiLaneReserve), "second reserve replaces first");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneQuery, Nax5ApiLaneReserve), "query does not abort reserve");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneOperator, Nax5ApiLaneTerminal), "operator does not abort terminal");
    expect(nax5ApiLaneAllowsConcurrent(Nax5ApiLaneReport), "report concurrent");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneTerminal, Nax5ApiLaneReport), "report does not abort terminal");
    expect(!nax5ApiShouldAbortExisting(Nax5ApiLaneReport, Nax5ApiLaneReserve), "report does not abort reserve");
}

static void test_shutdown_and_stale_lifecycle()
{
    expect(nax5ShutdownMutation(Nax5GameSessionStateFetchingConnection, false) == Nax5TerminalMutationCancel, "reserved cancel");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, false) == Nax5TerminalMutationFail, "connecting fail");
    expect(nax5ShutdownMutation(Nax5GameSessionStateActive, false) == Nax5TerminalMutationEnd, "active end");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, true) == Nax5TerminalMutationEnd, "connected stream end");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, true) != Nax5TerminalMutationCancel, "local connected close is not cancel");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, true) != Nax5TerminalMutationFail, "end while connected http pending");
    expect(nax5StreamQuitMutation(true, false) == Nax5TerminalMutationEnd, "quit after connected");
    expect(nax5StreamQuitMutation(false, false) == Nax5TerminalMutationFail, "quit before connected");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, false) == Nax5TerminalMutationFail, "shutdown connecting without local connection");
    expect(nax5ShutdownMutation(Nax5GameSessionStateConnecting, false) != Nax5TerminalMutationEnd, "never-connected connecting is not ended");
    expect(nax5SessionNeedsQuitRelease(Nax5GameSessionStateReserving), "quit during reserving");
    expect(nax5SessionNeedsQuitRelease(Nax5GameSessionStateFetchingConnection), "quit during fetch");
    expect(nax5SessionNeedsQuitRelease(Nax5GameSessionStateCancelling), "quit during cancel");
    expect(!nax5SessionNeedsQuitRelease(Nax5GameSessionStateIdle), "idle host close without nax5 reserve");
    expect(!nax5SessionNeedsQuitRelease(Nax5GameSessionStateError), "error host close without occupying");
    expect(nax5ShutdownNeedsCurrentSync(Nax5GameSessionStateReserving), "reserving shutdown syncs current");
    expect(!nax5ShutdownNeedsCurrentSync(Nax5GameSessionStateFetchingConnection), "assigned fetch uses cancel not current");
    expect(!nax5ShutdownNeedsCurrentSync(Nax5GameSessionStateIdle), "idle shutdown no current");
    expect(nax5ShouldSendTerminalBeforeUnauthLogout(true, true, Nax5TerminalMutationEnd), "401 with session sends end");
    expect(nax5ShouldSendTerminalBeforeUnauthLogout(true, true, Nax5TerminalMutationFail), "401 connecting sends fail");
    expect(!nax5ShouldSendTerminalBeforeUnauthLogout(true, false, Nax5TerminalMutationEnd), "401 without token skips terminal");
    expect(!nax5ShouldSendTerminalBeforeUnauthLogout(false, true, Nax5TerminalMutationFail), "401 without session id skips terminal");
    expect(nax5PlayEligibilityOk(true, true, QStringLiteral("ACTIVE")), "active verified can play");
    expect(!nax5PlayEligibilityOk(true, false, QStringLiteral("ACTIVE")), "unverified cannot play");
    expect(!nax5PlayEligibilityOk(true, true, QStringLiteral("INVITED")), "invited cannot play");
    expect(!nax5PlayEligibilityOk(true, true, QStringLiteral("WAITLISTED")), "waitlisted cannot play");
    expect(!nax5PlayEligibilityOk(false, true, QStringLiteral("ACTIVE")), "anonymous cannot play");
    expect(nax5ReleaseMutation(Nax5GameSessionStateConnecting, false) == Nax5TerminalMutationFail, "handshake release fails");
    expect(nax5ReleaseMutation(Nax5GameSessionStateConnecting, false) != Nax5TerminalMutationEnd, "handshake release is not end");
    expect(nax5ReleaseMutation(Nax5GameSessionStateActive, true) == Nax5TerminalMutationEnd, "active release ends");
    expect(nax5ReleaseMutation(Nax5GameSessionStateFetchingConnection, false) == Nax5TerminalMutationCancel, "reserved release cancels");
    expect(nax5ReleaseMutation(Nax5GameSessionStateReserving, false) == Nax5TerminalMutationCancel, "reserving abort cancels leftover");
    expect(nax5SessionTerminalBlocksPlay(Nax5TerminalMutationEnd), "end in-flight blocks play");
    expect(nax5SessionTerminalBlocksPlay(Nax5TerminalMutationFail), "fail in-flight blocks play");
    expect(!nax5SessionTerminalBlocksPlay(Nax5TerminalMutationNone), "no terminal allows play");
    expect(!nax5SessionTerminalBlocksPlay(Nax5TerminalMutationCancel), "cancel does not block idle play");
    expect(!nax5SessionCanCreateStream(true), "old stream blocks create");
    expect(nax5SessionCanCreateStream(false), "create when stream gone");
    expect(nax5SessionShouldResetLocalAfterAbortCurrent(false), "abort reset when current empty");
    expect(!nax5SessionShouldResetLocalAfterAbortCurrent(true), "abort keeps occupying current");
    expect(nax5StreamQuitMutation(true, true) == Nax5TerminalMutationNone, "operator test no product end");
    expect(nax5AcceptAsync(2, 2, 9, 9), "same generation request");
    expect(!nax5AcceptAsync(3, 2, 9, 9), "stale generation ignored");
    expect(!nax5AcceptAsync(2, 2, 10, 9), "stale request ignored");
    expect(!nax5AcceptAsync(2, 2, 0, 9), "cleared request ignored");
    expect(nax5AcceptSessionIdentity(QStringLiteral("session-a"), QStringLiteral("session-a")), "same session");
    expect(!nax5AcceptSessionIdentity(QStringLiteral("session-b"), QStringLiteral("session-a")), "session b ignores a");
    expect(nax5TerminalRetryLimit() >= 2 && nax5TerminalRetryLimit() <= 6, "bounded retries");
    expect(nax5TerminalShouldRetry(Nax5SessionErrorServerError), "terminal retries 5xx");
    expect(nax5TerminalShouldRetry(Nax5SessionErrorNetworkError), "terminal retries network errors");
    expect(!nax5TerminalShouldRetry(Nax5SessionErrorNotFound), "terminal does not retry 404");
    expect(!nax5TerminalShouldRetry(Nax5SessionErrorUnauthenticated), "terminal does not retry 401");
    // Upstream passed SESSION_RETRY_SECONDS / 3 (6) to a millisecond timer: 150 requests in 2 s.
    expect(nax5StreamRetryDelayMs(0) >= 250, "stream retry waits at least 250 ms");
    expect(nax5StreamRetryDelayMs(10000) >= 500 && nax5StreamRetryDelayMs(10000) <= 2000, "later stream retries about once a second");
    expect(20000 / nax5StreamRetryDelayMs(0) <= 40, "at most ~40 session requests in the 20 s retry window");
    expect(nax5TerminalRetryDelayMs(1) > 0 && nax5TerminalRetryDelayMs(9) <= 1200, "bounded retry delay");
    expect(nax5ShutdownGraceMs() > 0 && nax5ShutdownGraceMs() <= 1000, "short shutdown window");
    expect(nax5ShutdownReportGraceMs() >= 5000 && nax5ShutdownReportGraceMs() <= 15000, "report upload shutdown window");
}

static void test_logout_and_connected_end_races()
{
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionReset) == Nax5GameSessionStateIdle, "logout during reserve");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionReset) == Nax5GameSessionStateIdle, "logout during connection");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateEnding, "connected then end");
    expect(nax5SessionReduce(Nax5GameSessionStateEnding, Nax5GameSessionActionCancelSucceeded) == Nax5GameSessionStateIdle, "end ack idle");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateCancelling, "cancel during connection");
    expect(!nax5SessionCanStartPlay(Nax5GameSessionStateReserving), "double play ignored");
    expect(!nax5SessionCanRelease(Nax5GameSessionStateCancelling), "double cancel ignored");
    expect(!nax5SessionCanRelease(Nax5GameSessionStateEnding), "end already pending");
}

static void test_leftover_current_and_terminal_races()
{
    const Nax5GameSessionState leftover = nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionSyncedOccupied);
    expect(leftover == Nax5GameSessionStateFetchingConnection, "idle leftover to fetching");
    expect(nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateIdle), "idle leftover must fetch");
    expect(!nax5SessionCanStartPlay(leftover), "leftover without fetch would gray play");
    expect(nax5SessionCanRelease(leftover), "leftover can release");
    expect(nax5SessionReduce(Nax5GameSessionStateError, Nax5GameSessionActionSyncedOccupied) == Nax5GameSessionStateFetchingConnection, "error leftover to fetching");
    expect(nax5SessionShouldFetchOnSyncedOccupied(Nax5GameSessionStateError), "error leftover must fetch");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionSyncedOccupied) == Nax5GameSessionStateConnecting, "current during stream stays");
    expect(nax5SessionReduce(Nax5GameSessionStateActive, Nax5GameSessionActionSyncedOccupied) == Nax5GameSessionStateActive, "current during active stays");
    expect(nax5SessionShouldResetLocalAfterAbortCurrent(false), "abort reset only after empty current");
    expect(!nax5SessionShouldResetLocalAfterAbortCurrent(true), "abort does not reset occupying current");
    expect(nax5SessionReduce(Nax5GameSessionStateReserving, Nax5GameSessionActionReleaseClicked) == Nax5GameSessionStateCancelling, "abort reserving waits on current");
    expect(!nax5SessionCanStartPlay(Nax5GameSessionStateCancelling), "abort reserving play busy");
    expect(nax5SessionTerminalBlocksPlay(Nax5TerminalMutationEnd), "disconnect end blocks play bump");
    expect(nax5SessionTerminalBlocksPlay(Nax5TerminalMutationFail), "disconnect fail blocks play bump");
    expect(!nax5SessionCanCreateStream(true), "old stream blocks createSession");
    expect(nax5SessionCanCreateStream(false), "createSession after stream gone");
    expect(nax5ReleaseMutation(Nax5GameSessionStateConnecting, false) == Nax5TerminalMutationFail, "handshake release fail");
    expect(nax5ReleaseMutation(Nax5GameSessionStateActive, false) == Nax5TerminalMutationEnd, "active release end");
    expect(nax5SessionReduce(Nax5GameSessionStateEnding, Nax5GameSessionActionCancelFailed) == Nax5GameSessionStateError, "local stop cancel fail idle-error");
    expect(nax5SessionCanStartPlay(nax5SessionReduce(Nax5GameSessionStateEnding, Nax5GameSessionActionCancelFailed)), "play after local stop fail");
    expect(nax5ParseReserveResponse(401, "{\"code\":\"UNAUTHENTICATED\"}").error == Nax5SessionErrorUnauthenticated, "session 401");
    expect(nax5ParseCurrentResponse(401, "{\"code\":\"UNAUTHENTICATED\"}").error == Nax5SessionErrorUnauthenticated, "current 401");
    expect(nax5ParseCancelResponse(401, "{\"code\":\"UNAUTHENTICATED\"}").error == Nax5SessionErrorUnauthenticated, "cancel 401");
}

static void test_operator_multi_host_selection()
{
    Nax5SyntheticRegisteredHost hosts[2];
    hosts[0].nickname = QStringLiteral("PS5-ONE");
    hosts[0].regist_key = QByteArray("key-one");
    hosts[1].nickname = QStringLiteral("PS5-TWO");
    hosts[1].regist_key = QByteArray("key-two");
    expect(nax5PickSyntheticRegisteredHost(hosts, 2, 1)->nickname == QStringLiteral("PS5-TWO"), "selected second host");
    expect(nax5PickSyntheticRegisteredHost(hosts, 2, 1)->regist_key != hosts[0].regist_key, "not index 0 fallback");
    expect(nax5PickSyntheticRegisteredHost(hosts, 2, 0)->nickname == QStringLiteral("PS5-ONE"), "index 0 only if selected");
    expect(nax5PickSyntheticRegisteredHost(hosts, 2, -1) == nullptr, "reject missing selection");
    expect(nax5PickSyntheticRegisteredHost(hosts, 2, 2) == nullptr, "reject oob");
    expect(nax5ValidateProvisionSelection(-1, false, false, QStringLiteral("PS5-439")) == Nax5OperatorHostNoSelection, "no host");
    expect(nax5ValidateProvisionSelection(1, true, false, QStringLiteral("PS5-439")) == Nax5OperatorHostNotRegistered, "unregistered");
    expect(nax5ValidateProvisionSelection(1, true, true, QString()) == Nax5OperatorHostMissingConsoleCode, "missing code");
    expect(nax5ValidateProvisionSelection(1, true, true, QStringLiteral("PS5-439")) == Nax5OperatorHostOk, "explicit host and code");
}

static void test_product_operator_parity()
{
    expect(!nax5MaySleepConsole(false), "product never GoToBed");
    expect(nax5MaySleepConsole(true), "operator GoToBed like chiaki-ng");
    expect(!nax5MayResumeAfterOsSleep(false), "product no resume_session after OS sleep");
    expect(nax5MayResumeAfterOsSleep(true), "operator resume after OS sleep");
    expect(!nax5ShowsChiakiQuitDialog(false), "product hides chiaki quit string");
    expect(nax5ShowsChiakiQuitDialog(true), "operator keeps chiaki quit string");
    expect(!nax5OperatorHostConnectAllowed(false), "product Play is Nax5Session.play");
    expect(nax5OperatorHostConnectAllowed(true), "operator host auto-connect remains");
}

static void test_connected_flag_resets_on_new_play()
{
    bool stream_was_connected = true;
    bool stream_first_frame_seen = true;
    expect(nax5StreamQuitMutation(stream_was_connected, false) == Nax5TerminalMutationEnd, "connected play ends");
    stream_was_connected = nax5StreamConnectedOnNewGeneration();
    stream_first_frame_seen = nax5StreamFirstFrameSeenOnNewGeneration();
    expect(!stream_was_connected, "new play generation starts never-connected");
    expect(!stream_first_frame_seen, "new play generation must accept the next first frame");
    expect(nax5StreamQuitMutation(stream_was_connected, false) == Nax5TerminalMutationFail, "handshake timeout after prior success is fail");
    expect(nax5StreamQuitMutation(stream_was_connected, false) != Nax5TerminalMutationEnd, "handshake timeout is not end");
    quint64 connected_request_id = 5;
    connected_request_id = 0;
    expect(!nax5AcceptAsync(2, 2, connected_request_id, 5), "new play drops stale connected reply");
    expect(nax5ShouldRetryMarkConnected(Nax5GameSessionStateActive, false, Nax5SessionErrorNetworkError), "retry connected while active");
    expect(nax5ShouldRetryMarkConnected(Nax5GameSessionStateConnecting, false, Nax5SessionErrorNetworkError), "retry connected while still connecting");
    expect(!nax5ShouldRetryMarkConnected(Nax5GameSessionStateActive, true, Nax5SessionErrorNetworkError), "no connected retry during shutdown");
    expect(!nax5ShouldRetryMarkConnected(Nax5GameSessionStateActive, false, Nax5SessionErrorNotFound), "expired session is not retried");
}

static void test_stream_stall_and_backend_close()
{
    expect(!nax5StreamStalled(false, 10 * 60 * 1000), "no stall before the first frame");
    expect(!nax5StreamStalled(true, 1000), "live stream is not stalled");
    expect(!nax5StreamStalled(true, nax5StreamStallTimeoutMs()), "stall needs more than the timeout");
    expect(nax5StreamStalled(true, nax5StreamStallTimeoutMs() + 1), "silent console is stalled");
    expect(nax5HeartbeatSessionClosed(Nax5SessionErrorNotFound), "404 heartbeat means closed by backend");
    expect(!nax5HeartbeatSessionClosed(Nax5SessionErrorNetworkError), "network error keeps retrying");
    expect(!nax5HeartbeatSessionClosed(Nax5SessionErrorServerError), "5xx keeps retrying");
}

static void test_product_wakeup_from_material_once()
{
    expect(nax5ProductShouldWakeupBeforeCreateSession(), "product wakes before createSession");
    expect(nax5ProductWakeupSendsPerStartStream() == 1, "one wakeup per startStream");
    const Nax5MaterialWakeup call = nax5MaterialWakeupCall(QStringLiteral("203.0.113.10"), QByteArray("aabbccdd"), true);
    expect(call.ready, "wakeup ready");
    expect(call.host == QStringLiteral("203.0.113.10"), "wakeup host from material");
    expect(call.regist_key == QByteArray("aabbccdd"), "wakeup regist from material");
    expect(call.ps5, "wakeup ps5");
    expect(!nax5MaterialWakeupCall(QString(), QByteArray("aabbccdd"), true).ready, "empty host skips wakeup");
    expect(nax5OperatorConnectShouldWakeup(true, true), "operator standby discovered wakes");
    expect(!nax5OperatorConnectShouldWakeup(true, false), "operator ready host no wakeup");
    expect(!nax5OperatorConnectShouldWakeup(false, true), "operator undiscovered no wakeup");
    expect(nax5WakeupHostKindName(QStringLiteral("203.0.113.10")) == QStringLiteral("wan"), "public host wan");
    expect(nax5WakeupHostKindName(QStringLiteral("192.168.0.12")) == QStringLiteral("lan"), "rfc1918 lan");
    expect(nax5WakeupHostKindName(QStringLiteral("10.8.0.4")) == QStringLiteral("lan"), "10/8 lan");
}

static void test_shutdown_lifetime_qpointer()
{
    QObject *owner = new QObject();
    QObject *dependency = new QObject(owner);
    QObject *controller = new QObject(dependency);
    QPointer<QObject> dependency_ptr = dependency;
    QPointer<QObject> controller_ptr = controller;
    delete owner;
    expect(controller_ptr.isNull(), "controller cannot outlive parent dependency");
    expect(dependency_ptr.isNull(), "dependency destroyed with owner");
}

static void test_persisted_terminal_roundtrip()
{
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("t.ini")), QSettings::IniFormat);
    expect(nax5LoadTerminal(settings, 7).mutation == Nax5TerminalMutationNone, "nothing stored at first");

    nax5SaveTerminal(settings, {7, QStringLiteral("session-1"), Nax5TerminalMutationEnd, QString()});
    const auto loaded = nax5LoadTerminal(settings, 7);
    expect(loaded.mutation == Nax5TerminalMutationEnd && loaded.session_id == QStringLiteral("session-1"),
        "unacknowledged /end/ survives a restart");
    expect(nax5LoadTerminal(settings, 8).mutation == Nax5TerminalMutationNone, "another account never resends it");

    nax5SaveTerminal(settings, {7, QStringLiteral("session-2"), Nax5TerminalMutationFail, QString()});
    nax5ClearTerminal(settings, QStringLiteral("session-1"));
    expect(nax5LoadTerminal(settings, 7).session_id == QStringLiteral("session-2"),
        "late answer for an older session keeps the newer record");
    nax5ClearTerminal(settings, QStringLiteral("session-2"));
    expect(nax5LoadTerminal(settings, 7).mutation == Nax5TerminalMutationNone, "final answer clears the record");

    nax5SaveTerminal(settings, {7, QStringLiteral("session-3"), Nax5TerminalMutationEnd,
        QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODate)});
    expect(nax5LoadTerminal(settings, 7).mutation == Nax5TerminalMutationNone, "records older than a day are ignored");
}

int main()
{
    test_persisted_terminal_roundtrip();
    test_reserve_success();
    test_no_capacity();
    test_eligibility_denied();
    test_play_time_answers();
    test_build16_decisions();
    test_unauthenticated();
    test_required_update();
    test_conflict_and_rate_limit_and_server();
    test_malformed_and_timeout();
    test_current_none_and_present();
    test_cancel_success_and_failure();
    test_state_transitions_and_double_click();
    test_user_facing_errors_and_no_leak();
    test_user_agent_version();
    test_request_lanes_do_not_abort_unrelated();
    test_shutdown_and_stale_lifecycle();
    test_logout_and_connected_end_races();
    test_leftover_current_and_terminal_races();
    test_operator_multi_host_selection();
    test_product_operator_parity();
    test_connected_flag_resets_on_new_play();
    test_product_wakeup_from_material_once();
    test_stream_stall_and_backend_close();
    test_shutdown_lifetime_qpointer();
    if (g_failed)
    {
        std::fprintf(stderr, "%d NAX5 session tests failed\n", g_failed);
        return 1;
    }
    std::printf("All NAX5 session tests passed\n");
    return 0;
}
