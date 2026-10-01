// Client <-> backend integration test. Drives the real Nax5ApiClient (HTTP,
// headers, parsers) against a live nax5-backend seeded by
// scripts/tests/integration/seed_backend.py. Run through
// scripts/tests/integration/run-backend-integration.ps1, never against prod.
//
// Environment: NAX5_API_BASE_URL (loopback), NAX5_IT_USER_A, NAX5_IT_USER_B,
// NAX5_IT_PASSWORD, NAX5_IT_CONSOLE, NAX5_IT_LEASE_SECONDS,
// NAX5_IT_PHASE=main|update-required.

#include "nax5/nax5apiclient.h"
#include "nax5/nax5clientreport.h"
#include "nax5/nax5reportqueue.h"
#include "nax5/nax5telemetry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <functional>

static QString test_log_dir;
QString GetLogBaseDir() { return test_log_dir; }

static int g_failed = 0;
static void expect(bool condition, const char *name, const QString &detail = QString())
{
    std::printf("%s %s%s%s\n", condition ? "ok" : "FAIL", name,
        detail.isEmpty() ? "" : " -- ", qPrintable(detail));
    std::fflush(stdout);
    if (!condition) ++g_failed;
}

template<typename Signal, typename Result>
static bool waitFor(Nax5ApiClient &api, Signal signal, quint64 id, Result *out, int timeout_ms = 15000)
{
    QEventLoop loop;
    bool done = false;
    auto conn = QObject::connect(&api, signal, &loop, [&](quint64 got, const Result &result) {
        if (got != id) return;
        *out = result;
        done = true;
        loop.quit();
    });
    QTimer::singleShot(timeout_ms, &loop, &QEventLoop::quit);
    if (!done) loop.exec();
    QObject::disconnect(conn);
    return done;
}

static bool waitStatus(Nax5ApiClient &api, quint64 id, int *status)
{
    QEventLoop loop;
    bool done = false;
    auto conn = QObject::connect(&api, &Nax5ApiClient::clientReportFinished, &loop, [&](quint64 got, int http) {
        if (got != id) return;
        *status = http;
        done = true;
        loop.quit();
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    if (!done) loop.exec();
    QObject::disconnect(conn);
    return done;
}

static void sleepMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static QString login(Nax5ApiClient &api, const QString &email, const QString &password)
{
    Nax5LoginParseResult r;
    const bool done = waitFor(api, &Nax5ApiClient::loginFinished, api.login(email, password), &r);
    return done && r.error == Nax5AuthErrorNone ? r.session_token : QString();
}

static Nax5SessionParseResult reserve(Nax5ApiClient &api, const QString &token, const QString &key = QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    Nax5SessionParseResult r;
    waitFor(api, &Nax5ApiClient::reserveFinished, api.reserveSession(token, key), &r);
    return r;
}

static void phaseMain(Nax5ApiClient &api)
{
    const QString user_a = qEnvironmentVariable("NAX5_IT_USER_A");
    const QString user_b = qEnvironmentVariable("NAX5_IT_USER_B");
    const QString password = qEnvironmentVariable("NAX5_IT_PASSWORD");
    const QString console = qEnvironmentVariable("NAX5_IT_CONSOLE");
    const int lease_s = qEnvironmentVariableIntValue("NAX5_IT_LEASE_SECONDS");

    // Login and account
    Nax5LoginParseResult bad;
    waitFor(api, &Nax5ApiClient::loginFinished, api.login(user_a, password + "-wrong"), &bad);
    expect(bad.error == Nax5AuthErrorInvalidCredentials, "wrong password is reported as invalid credentials",
        QString::number(int(bad.error)));
    const QString token_a = login(api, user_a, password);
    const QString token_b = login(api, user_b, password);
    expect(!token_a.isEmpty() && !token_b.isEmpty(), "both test users log in");
    Nax5MeParseResult me;
    waitFor(api, &Nax5ApiClient::meFinished, api.fetchMe(token_a), &me);
    expect(me.ok && me.email_verified && me.access_status == "ACTIVE", "me: verified ACTIVE account", me.access_status);

    // Reserve, idempotency, one console for two players
    const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto first = reserve(api, token_a, key);
    expect(first.error == Nax5SessionErrorNone && first.has_session, "A reserves a console", QString::number(int(first.error)));
    expect(first.console.code == console, "A is assigned the seeded console", first.console.code);
    const auto again = reserve(api, token_a, key);
    expect(again.error == Nax5SessionErrorNone && again.session.id == first.session.id, "same idempotency key returns the same session");
    const auto busy = reserve(api, token_b);
    expect(busy.error == Nax5SessionErrorNoCapacity, "B gets NO_CAPACITY while A holds the only console",
        QString::number(int(busy.error)));

    // Connect, play, end
    Nax5ConnectionParseResult conn;
    waitFor(api, &Nax5ApiClient::connectionFinished, api.fetchConnection(token_a, first.session.id), &conn);
    expect(conn.error == Nax5SessionErrorNone && conn.has_material, "connection material parses", QString::number(int(conn.error)));
    expect(conn.material.regist_key.size() == 16 && conn.material.morning.size() == 16 && !conn.material.host.isEmpty(),
        "material has host, 16-byte regist key and morning");
    Nax5SessionParseResult connected;
    waitFor(api, &Nax5ApiClient::connectedFinished, api.markConnected(token_a, first.session.id), &connected);
    expect(connected.error == Nax5SessionErrorNone && connected.session.status == "ACTIVE", "connected -> ACTIVE", connected.session.status);
    for (int i = 0; i < 2; ++i)
    {
        sleepMs(1000);
        Nax5SessionParseResult hb;
        waitFor(api, &Nax5ApiClient::heartbeatFinished, api.heartbeatSession(token_a, first.session.id), &hb);
        expect(hb.error == Nax5SessionErrorNone, "heartbeat accepted", QString::number(int(hb.error)));
    }
    Nax5SessionParseResult ended;
    waitFor(api, &Nax5ApiClient::endFinished, api.endSession(token_a, first.session.id), &ended);
    expect(ended.error == Nax5SessionErrorNone && ended.session.status == "ENDED", "end -> ENDED", ended.session.status);
    Nax5SessionParseResult end_twice;
    waitFor(api, &Nax5ApiClient::endFinished, api.endSession(token_a, first.session.id), &end_twice);
    expect(end_twice.error == Nax5SessionErrorNone, "repeated /end/ (client retry) is harmless", QString::number(int(end_twice.error)));

    // B plays, then its network drops: no heartbeat, no /end/.
    // After NO_CAPACITY the backend rate-limits B's reserve for 5 s.
    Nax5SessionParseResult b_too_soon = reserve(api, token_b);
    expect(b_too_soon.error == Nax5SessionErrorRateLimited, "immediate retry after NO_CAPACITY is rate limited",
        QString::number(int(b_too_soon.error)));
    sleepMs(6000);
    const auto b = reserve(api, token_b);
    expect(b.error == Nax5SessionErrorNone && b.has_session, "console is free again for B after A ends", QString::number(int(b.error)));
    Nax5ConnectionParseResult b_conn;
    waitFor(api, &Nax5ApiClient::connectionFinished, api.fetchConnection(token_b, b.session.id), &b_conn);
    Nax5SessionParseResult b_connected;
    waitFor(api, &Nax5ApiClient::connectedFinished, api.markConnected(token_b, b.session.id), &b_connected);
    expect(b_connected.session.status == "ACTIVE", "B connected");
    sleepMs(2000);
    Nax5SessionParseResult b_hb;
    waitFor(api, &Nax5ApiClient::heartbeatFinished, api.heartbeatSession(token_b, b.session.id), &b_hb);
    std::printf("-- B network drops for %d s (lease + 3 s)\n", lease_s + 3);
    std::fflush(stdout);
    sleepMs((lease_s + 3) * 1000);
    const auto a_after = reserve(api, token_a);
    expect(a_after.error == Nax5SessionErrorNone && a_after.has_session,
        "after B's lease expires the console is reusable without waiting for B", QString::number(int(a_after.error)));
    Nax5SessionParseResult b_late_end;
    waitFor(api, &Nax5ApiClient::endFinished, api.endSession(token_b, b.session.id), &b_late_end);
    expect(b_late_end.error != Nax5SessionErrorServerError && b_late_end.error != Nax5SessionErrorInvalidResponse,
        "B's late /end/ after reconnecting gets a clean answer, not a 5xx", QString::number(int(b_late_end.error)));
    Nax5SessionParseResult a_cancel;
    waitFor(api, &Nax5ApiClient::cancelFinished, api.cancelSession(token_a, a_after.session.id), &a_cancel);
    expect(a_cancel.error == Nax5SessionErrorNone, "cancel before connecting frees the console", QString::number(int(a_cancel.error)));
    std::printf("SESSION_B=%s\nSESSION_A1=%s\n", qPrintable(b.session.id), qPrintable(first.session.id));

    // Telemetry and crash report upload, exactly as the client builds them
    const QByteArray events = nax5BuildClientEventBatch(QStringLiteral("PLAY_REQUESTED"), QString());
    // Fire-and-forget like the real client; the runner checks the backend stored it.
    api.postClientEvents(token_a, events);
    sleepMs(2000);

    QTemporaryDir dir;
    const QString root = dir.filePath("queue"), dumps = dir.filePath("crash-dumps");
    QDir().mkpath(dumps);
    for (const auto &[name, body] : QList<QPair<QString, QByteArray>>{
             {"NAX5-20260927-000000-4242.dmp", QByteArray("MDMP") + QByteArray(200 * 1024, '\x07')},
             {"NAX5-20260927-000000-4242.txt", "reason=unhandled_exception\nexception_code=0xC0000005\nfault_offset=0x1234\n"}})
    {
        QFile f(QDir(dumps).filePath(name));
        f.open(QIODevice::WriteOnly);
        f.write(body);
        f.close();
        f.open(QIODevice::ReadWrite);
        f.setFileTime(QDateTime::currentDateTime().addSecs(-60), QFileDevice::FileModificationTime);
    }
    expect(nax5QueueCrashDumps(root, 1, dumps, "client_version=alpha-0.5-build-13\n") == 1, "crash dump queued");
    const auto part = nax5NextReportPart(root, 1);
    int report_status = 0;
    waitStatus(api, api.postClientReportArchive(token_a, nax5ClientReportKindFromName(part.kind), QString(), part.archive,
        part.client_version, part.client_sha), &report_status);
    expect(report_status == 201, "crash report with minidump accepted by the server", QString::number(report_status));

    // Logout invalidates the token
    QEventLoop logout_loop;
    QObject::connect(&api, &Nax5ApiClient::logoutFinished, &logout_loop, &QEventLoop::quit);
    api.logout(token_a);
    QTimer::singleShot(10000, &logout_loop, &QEventLoop::quit);
    logout_loop.exec();
    Nax5MeParseResult me_after;
    waitFor(api, &Nax5ApiClient::meFinished, api.fetchMe(token_a), &me_after);
    expect(!me_after.ok && !me_after.network_failure, "token rejected after logout");
}

static void phaseUpdateRequired(Nax5ApiClient &api)
{
    const QString token = login(api, qEnvironmentVariable("NAX5_IT_USER_A"), qEnvironmentVariable("NAX5_IT_PASSWORD"));
    expect(!token.isEmpty(), "login still works when the client is outdated");
    const auto r = reserve(api, token);
    expect(r.error == Nax5SessionErrorClientUpdateRequired, "reserve returns CLIENT_UPDATE_REQUIRED", QString::number(int(r.error)));
    expect(r.minimum_version == "alpha-0.5-build-99", "minimum version is passed to the client", r.minimum_version);
    expect(!r.update_url.isEmpty(), "update URL is passed to the client", r.update_url);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir log_dir;
    test_log_dir = log_dir.path();
    Nax5ApiClient api;
    const QString phase = qEnvironmentVariable("NAX5_IT_PHASE", "main");
    if (phase == "update-required") phaseUpdateRequired(api);
    else phaseMain(api);
    std::printf("%s: %d failed\n", qPrintable(phase), g_failed);
    return g_failed == 0 ? 0 : 1;
}
