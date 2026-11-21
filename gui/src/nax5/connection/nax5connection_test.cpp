#include "nax5/connection/nax5connectionmaterial.h"
#include "nax5/connection/nax5connectionparser.h"
#include "nax5/nax5runtime.h"
#include "nax5/session/nax5sessionstate.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QString>
#include <QtGlobal>
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

static QByteArray b64(const QByteArray &raw)
{
    return raw.toBase64();
}

static QByteArray sixteen(char fill)
{
    return QByteArray(Nax5RegistKeySize, fill);
}

static QByteArray connectionBody(const QByteArray &regist, const QByteArray &morning, const QString &host = QStringLiteral("203.0.113.10"), int version = 1, int target = 1000100)
{
    return QStringLiteral(
        "{\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"CONNECTING\"},"
        "\"connection\":{\"version\":%1,\"target\":%2,\"host\":\"%3\",\"nickname\":\"PS5-439\","
        "\"registKey\":\"%4\",\"morning\":\"%5\",\"consolePin\":\"\"}}")
        .arg(version)
        .arg(target)
        .arg(host)
        .arg(QString::fromLatin1(b64(regist)))
        .arg(QString::fromLatin1(b64(morning)))
        .toUtf8();
}

static void test_valid_base64_keys()
{
    const QByteArray regist = sixteen('A');
    const QByteArray morning = sixteen('B');
    const Nax5ConnectionParseResult result = nax5ParseConnectionResponse(200, connectionBody(regist, morning));
    expect(result.error == Nax5SessionErrorNone, "valid parse");
    expect(result.has_material, "has material");
    expect(result.material.regist_key == regist, "regist key");
    expect(result.material.morning == morning, "morning");
    expect(result.material.host == QStringLiteral("203.0.113.10"), "host");
    expect(result.material.target == 1000100, "target");
    expect(result.material.session_status == QStringLiteral("CONNECTING"), "status");
}

static void test_invalid_base64_and_length()
{
    QByteArray bad = "{\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"CONNECTING\"},"
        "\"connection\":{\"version\":1,\"target\":1000100,\"host\":\"203.0.113.10\","
        "\"registKey\":\"@@@\",\"morning\":\"QkJCQkJCQkJCQkJCQkJCQg==\",\"consolePin\":\"\"}}";
    expect(nax5ParseConnectionResponse(200, bad).error == Nax5SessionErrorInvalidResponse, "invalid base64");

    const QByteArray short_key = QByteArray(8, 'A').toBase64();
    const QByteArray morning = sixteen('B').toBase64();
    const QByteArray short_body = QStringLiteral(
        "{\"session\":{\"id\":\"11111111-1111-4111-8111-111111111111\",\"status\":\"CONNECTING\"},"
        "\"connection\":{\"version\":1,\"target\":1000100,\"host\":\"203.0.113.10\","
        "\"registKey\":\"%1\",\"morning\":\"%2\",\"consolePin\":\"\"}}")
        .arg(QString::fromLatin1(short_key), QString::fromLatin1(morning)).toUtf8();
    expect(nax5ParseConnectionResponse(200, short_body).error == Nax5SessionErrorInvalidResponse, "wrong length");
}

static void test_missing_and_unsupported()
{
    expect(nax5ParseConnectionResponse(200, "{\"session\":{\"id\":\"x\",\"status\":\"CONNECTING\"},\"connection\":{\"version\":1,\"target\":1000100,\"registKey\":\"QQ==\",\"morning\":\"QQ==\"}}").error == Nax5SessionErrorInvalidResponse, "missing host");
    const QByteArray regist = sixteen('A');
    const QByteArray morning = sixteen('B');
    expect(nax5ParseConnectionResponse(200, connectionBody(regist, morning, QStringLiteral("203.0.113.10"), 2)).error == Nax5SessionErrorInvalidResponse, "unsupported version");
    expect(nax5ParseConnectionResponse(200, connectionBody(regist, morning, QStringLiteral("203.0.113.10"), 1, 800)).error == Nax5SessionErrorInvalidResponse, "unsupported target");
}

static void test_explicit_direct_ports()
{
    auto body = connectionBody(sixteen('A'), sixteen('B'), QStringLiteral("203.0.113.10"), 2);
    body.chop(2);
    body += R"(,"ports":{"sessionControlTcp":19295,"streamUdp":19296,"senkushaUdp":19297,"discoveryUdp":19302}}})";
    auto result = nax5ParseConnectionResponse(200, body);
    expect(result.has_material && result.material.session_control_port == 19295
        && result.material.stream_port == 19296 && result.material.senkusha_port == 19297
        && result.material.discovery_port == 19302, "v2 routes all destinations");
    result.material.clear();
    expect(result.material.session_control_port == 0 && result.material.discovery_port == 0,
        "discarded material discards ports");
    const auto legacy = nax5ParseConnectionResponse(200, connectionBody(sixteen('A'), sixteen('B')));
    expect(legacy.has_material && legacy.material.session_control_port == 0
        && legacy.material.stream_port == 0 && legacy.material.senkusha_port == 0
        && legacy.material.discovery_port == 0, "v1 uses native defaults");
    auto invalid = body;
    invalid.replace("19302", "65536");
    expect(!nax5ParseConnectionResponse(200, invalid).has_material, "v2 rejects out of range port");
    invalid = body;
    invalid.replace("19302", "19302.5");
    expect(!nax5ParseConnectionResponse(200, invalid).has_material, "v2 rejects fractional port");
    invalid = body;
    invalid.replace("19302", "19296");
    expect(!nax5ParseConnectionResponse(200, invalid).has_material, "v2 rejects duplicate UDP destinations");
}

static void test_http_errors()
{
    expect(nax5ParseConnectionResponse(401, "{\"code\":\"UNAUTHENTICATED\"}").error == Nax5SessionErrorUnauthenticated, "401");
    expect(nax5ParseConnectionResponse(403, "{\"code\":\"SESSION_NOT_OWNED\"}").error == Nax5SessionErrorForbidden, "owner mismatch");
    expect(nax5ParseConnectionResponse(404, "{\"code\":\"SESSION_NOT_FOUND\"}").error == Nax5SessionErrorNotFound, "missing session");
    expect(nax5ParseConnectionResponse(409, "{\"code\":\"INVALID_STATE\"}").error == Nax5SessionErrorInvalidConnectionMaterial, "expired or terminal");
    expect(nax5ParseConnectionResponse(409, "{\"code\":\"CONSOLE_HOST_NOT_CONFIGURED\"}").error == Nax5SessionErrorHostNotConfigured, "host not configured");
    expect(nax5ParseConnectionResponse(409, "{\"code\":\"OPERATOR_TEST_REQUIRED\"}").error == Nax5SessionErrorOperatorTestRequired, "test required");
    expect(nax5ParseConnectionResponse(0, "x").error == Nax5SessionErrorNetworkError, "timeout");
    expect(nax5ParseConnectionResponse(200, "not-json").error == Nax5SessionErrorInvalidResponse, "malformed json");
}

static void test_cleanup_and_duplicate_state()
{
    Nax5ConnectionMaterial material;
    material.regist_key = sixteen('A');
    material.morning = sixteen('B');
    material.host = QStringLiteral("203.0.113.10");
    material.clear();
    expect(nax5ConnectionMaterialIsEmpty(material), "cleared");
    expect(nax5SessionReduce(Nax5GameSessionStateConnecting, Nax5GameSessionActionConnectionReceived) == Nax5GameSessionStateConnecting, "duplicate connecting");
    expect(nax5SessionReduce(Nax5GameSessionStateIdle, Nax5GameSessionActionConnectionReceived) == Nax5GameSessionStateIdle, "stale connection");
    expect(nax5SessionReduce(Nax5GameSessionStateFetchingConnection, Nax5GameSessionActionReset) == Nax5GameSessionStateIdle, "logout cleanup");
}

static void test_decode_and_operator_test_payload()
{
    QByteArray out;
    expect(!nax5DecodeRpKey(QString(), Nax5RegistKeySize, &out), "empty key");
    expect(!nax5DecodeRpKey(QStringLiteral("not-base64"), Nax5RegistKeySize, &out), "not base64");
    expect(!nax5DecodeRpKey(QString::fromLatin1(QByteArray("deadbeefdeadbeef").toHex()), Nax5RegistKeySize, &out), "hex rejected");
    expect(nax5DecodeRpKey(QString::fromLatin1(sixteen('S').toBase64()), Nax5RegistKeySize, &out), "sentinel decode");
    expect(out == sixteen('S'), "sentinel bytes");
    expect(nax5TargetIsSupportedDirectPs5(1000000), "ps5 unknown");
    expect(nax5TargetIsSupportedDirectPs5(1000100), "ps5 1");
    expect(!nax5TargetIsSupportedDirectPs5(800), "ps4 rejected");

    const QByteArray body = QStringLiteral(
        "{\"session\":{\"id\":\"operator-test\",\"status\":\"CONNECTING\"},"
        "\"connection\":{\"version\":1,\"target\":1000100,\"host\":\"203.0.113.10\",\"nickname\":\"Lab\","
        "\"registKey\":\"%1\",\"morning\":\"%2\",\"consolePin\":\"\"}}")
        .arg(QString::fromLatin1(b64(sixteen('A'))), QString::fromLatin1(b64(sixteen('B')))).toUtf8();
    const Nax5ConnectionParseResult result = nax5ParseConnectionResponse(200, body);
    expect(result.error == Nax5SessionErrorNone, "operator-test parse");
    expect(result.material.session_id == QStringLiteral("operator-test"), "operator-test id");
}

static void test_operator_namespace()
{
    qunsetenv("NAX5_OPERATOR_MODE");
    expect(Nax5Runtime::settingsOrganizationName() == QStringLiteral("NAX5"), "org");
    expect(Nax5Runtime::settingsApplicationName() == QStringLiteral("NAX5"), "product app");
    qputenv("NAX5_OPERATOR_MODE", "1");
    expect(Nax5Runtime::settingsApplicationName() == QStringLiteral("NAX5-Operator"), "operator app");
    const QString previous = Nax5Runtime::lastOperatorConsoleCode();
    Nax5Runtime::setLastOperatorConsoleCode(QStringLiteral("PS5-439"));
    expect(Nax5Runtime::lastOperatorConsoleCode() == QStringLiteral("PS5-439"), "operator persist");
    Nax5Runtime::setLastOperatorConsoleCode(previous);
    qunsetenv("NAX5_OPERATOR_MODE");
    Nax5Runtime::setLastOperatorConsoleCode(QStringLiteral("PS5-439"));
    expect(Nax5Runtime::lastOperatorConsoleCode().isEmpty(), "product namespace skipped");
}

static void test_vanilla_vs_transient_connect_info_parity()
{
    const QByteArray regist = sixteen('K');
    const QByteArray morning = sixteen('M');
    const int target = 1000100;
    const QString host = QStringLiteral("192.168.1.9");
    const QString nickname = QStringLiteral("PS5-439");
    const QString pin = QStringLiteral("0000");

    Nax5ConnectionMaterial vanilla;
    vanilla.version = Nax5ConnectionContractVersion;
    vanilla.target = target;
    vanilla.host = host;
    vanilla.nickname = nickname;
    vanilla.regist_key = regist;
    vanilla.morning = morning;
    vanilla.console_pin = pin;

    const QByteArray dto = QStringLiteral(
        "{\"session\":{\"id\":\"operator-test\",\"status\":\"CONNECTING\"},"
        "\"connection\":{\"version\":1,\"target\":%1,\"host\":\"%2\",\"nickname\":\"%3\","
        "\"registKey\":\"%4\",\"morning\":\"%5\",\"consolePin\":\"%6\"}}")
        .arg(target)
        .arg(host)
        .arg(nickname)
        .arg(QString::fromLatin1(b64(regist)))
        .arg(QString::fromLatin1(b64(morning)))
        .arg(pin)
        .toUtf8();
    const Nax5ConnectionParseResult parsed = nax5ParseConnectionResponse(200, dto);
    expect(parsed.error == Nax5SessionErrorNone, "transient parse");
    expect(parsed.material.target == vanilla.target, "target parity");
    expect(parsed.material.host == vanilla.host, "host parity");
    expect(parsed.material.nickname == vanilla.nickname, "nickname parity");
    expect(parsed.material.console_pin == vanilla.console_pin, "pin parity");
    expect(parsed.material.regist_key == vanilla.regist_key, "regist bytes");
    expect(parsed.material.morning == vanilla.morning, "morning bytes");
    expect(parsed.material.regist_key.size() == Nax5RegistKeySize, "regist length");
    expect(parsed.material.morning.size() == Nax5MorningSize, "morning length");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settings_dir;
    if (!settings_dir.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    test_valid_base64_keys();
    test_invalid_base64_and_length();
    test_missing_and_unsupported();
    test_explicit_direct_ports();
    test_http_errors();
    test_cleanup_and_duplicate_state();
    test_decode_and_operator_test_payload();
    test_operator_namespace();
    test_vanilla_vs_transient_connect_info_parity();
    if (g_failed)
    {
        std::fprintf(stderr, "%d NAX5 connection tests failed\n", g_failed);
        return 1;
    }
    std::printf("All NAX5 connection tests passed\n");
    return 0;
}
