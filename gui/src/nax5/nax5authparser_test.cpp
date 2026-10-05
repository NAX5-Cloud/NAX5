#include "nax5/nax5apiconfig.h"
#include "nax5/nax5autherror.h"
#include "nax5/nax5authparser.h"
#include "nax5/nax5authstate.h"
#include "nax5/nax5authstore.h"
#include "nax5/nax5runtime.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QSettings>
#include <QTemporaryDir>
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

static void test_login_success()
{
    const char *body =
        "{\"status\":200,\"data\":{\"user\":{\"id\":7,\"email\":\"a@b.c\"}},"
        "\"meta\":{\"is_authenticated\":true,\"session_token\":\"tok123\"}}";
    const Nax5LoginParseResult result = nax5ParseLoginResponse(200, body);
    expect(result.error == Nax5AuthErrorNone, "login success error");
    expect(result.session_token == QStringLiteral("tok123"), "login success token");
}

static void test_login_does_not_expose_token_on_failure()
{
    const char *body =
        "{\"status\":400,\"errors\":[{\"message\":\"nope\",\"code\":\"email_password_mismatch\",\"param\":\"password\"}]}";
    const Nax5LoginParseResult result = nax5ParseLoginResponse(400, body);
    expect(result.error == Nax5AuthErrorInvalidCredentials, "invalid password");
    expect(result.session_token.isEmpty(), "invalid login has no token");
}

static void test_unknown_email()
{
    const char *body =
        "{\"status\":400,\"errors\":[{\"message\":\"nope\",\"code\":\"email_password_mismatch\",\"param\":\"password\"}]}";
    const Nax5LoginParseResult result = nax5ParseLoginResponse(400, body);
    expect(result.error == Nax5AuthErrorInvalidCredentials, "unknown email");
}

static void test_unverified_email()
{
    const char *body =
        "{\"status\":401,\"data\":{\"flows\":[{\"id\":\"login\"},{\"id\":\"verify_email\",\"is_pending\":true}]},"
        "\"meta\":{\"is_authenticated\":false,\"session_token\":\"pending\"}}";
    const Nax5LoginParseResult result = nax5ParseLoginResponse(401, body);
    expect(result.error == Nax5AuthErrorEmailNotVerified, "unverified email");
    expect(result.session_token.isEmpty(), "unverified login does not keep token");
}

static void test_rate_limited()
{
    expect(nax5ParseLoginResponse(429, "{\"status\":429}").error == Nax5AuthErrorRateLimited, "429");
    expect(nax5ParseLoginResponse(403, "locked").error == Nax5AuthErrorRateLimited, "403 axes");
}

static void test_server_and_malformed()
{
    expect(nax5ParseLoginResponse(500, "internal").error == Nax5AuthErrorServerError, "http 500");
    expect(nax5ParseLoginResponse(200, "not-json").error == Nax5AuthErrorInvalidResponse, "malformed json");
    expect(nax5ParseLoginResponse(200, "{\"meta\":{\"is_authenticated\":true}}").error == Nax5AuthErrorInvalidResponse, "missing token");
}

static void test_me_payload()
{
    const char *body =
        "{\"id\":12,\"email\":\"alex@example.com\",\"city\":\"Moscow\",\"accessStatus\":\"WAITLISTED\",\"emailVerified\":true}";
    const Nax5MeParseResult result = nax5ParseMeResponse(200, body);
    expect(result.ok, "me ok");
    expect(result.user_id == 12, "me id");
    expect(result.email == QStringLiteral("alex@example.com"), "me email");
    expect(result.city == QStringLiteral("Moscow"), "me city");
    expect(result.access_status == QStringLiteral("WAITLISTED"), "me status");
    expect(result.email_verified, "me verified");
}

static void test_me_missing_fields()
{
    expect(!nax5ParseMeResponse(200, "{\"email\":\"a@b.c\"}").ok, "me missing fields");
    expect(!nax5ParseMeResponse(200, "{").ok, "me malformed");
    expect(!nax5ParseMeResponse(401, "{\"detail\":\"Authentication required\"}").ok, "me unauthorized");
}

static void test_network_mapping()
{
    expect(nax5MapNetworkFailure(0, true, false) == Nax5AuthErrorNetworkError, "timeout");
    expect(nax5MapNetworkFailure(0, false, true) == Nax5AuthErrorNetworkError, "offline");
}

static void test_state_machine()
{
    expect(nax5AuthReduce(Nax5AuthStateUnauthenticated, Nax5AuthActionLoginClicked) == Nax5AuthStateAuthenticating, "start login");
    expect(nax5AuthCanStartLogin(Nax5AuthStateUnauthenticated), "can login");
    expect(!nax5AuthCanStartLogin(Nax5AuthStateAuthenticating), "busy login");
    expect(nax5AuthReduce(Nax5AuthStateAuthenticating, Nax5AuthActionLoginClicked) == Nax5AuthStateAuthenticating, "ignore double click");
    expect(nax5AuthReduce(Nax5AuthStateAuthenticating, Nax5AuthActionLoginSucceeded) == Nax5AuthStateAuthenticated, "success");
    expect(nax5AuthReduce(Nax5AuthStateAuthenticating, Nax5AuthActionLoginFailed) == Nax5AuthStateUnauthenticated, "failure");
    expect(nax5AuthReduce(Nax5AuthStateAuthenticated, Nax5AuthActionLogout) == Nax5AuthStateUnauthenticated, "logout");
}

static void test_remote_play_requires_authenticated()
{
    expect(!nax5AuthAllowsRemotePlay(Nax5AuthStateUnauthenticated), "no remote play while logged out");
    expect(!nax5AuthAllowsRemotePlay(Nax5AuthStateAuthenticating), "no remote play while logging in");
    expect(nax5AuthAllowsRemotePlay(Nax5AuthStateAuthenticated), "remote play after login");
}

static void test_error_messages_are_user_facing()
{
    const QString invalid = nax5AuthErrorMessage(Nax5AuthErrorInvalidCredentials);
    expect(invalid.contains(QStringLiteral("email")), "credentials message");
    expect(!invalid.contains(QStringLiteral("HTTP")), "no http code");
    expect(!nax5AuthErrorMessage(Nax5AuthErrorNetworkError).contains(QStringLiteral("QNetworkReply")), "no qt error");
}

class EnvGuard
{
public:
    EnvGuard()
        : had(qEnvironmentVariableIsSet("NAX5_API_BASE_URL"))
        , old(qgetenv("NAX5_API_BASE_URL"))
    {
    }
    ~EnvGuard()
    {
        if (had)
            qputenv("NAX5_API_BASE_URL", old);
        else
            qunsetenv("NAX5_API_BASE_URL");
    }
    bool had;
    QByteArray old;
};

static QUrl parseBase(QString *error)
{
    if (error)
        error->clear();
    return Nax5ApiConfig::baseUrl(error);
}

static void test_default_api_url_is_https()
{
    EnvGuard guard;
    qunsetenv("NAX5_API_BASE_URL");
    expect(Nax5ApiConfig::defaultBaseUrl() == QStringLiteral("https://cloudgta6.com"), "production default");
    expect(Nax5ApiConfig::userAgent().startsWith(QStringLiteral("NAX5/0.4")), "user agent");
    QString error;
    const QUrl url = parseBase(&error);
    expect(url.toString() == QStringLiteral("https://cloudgta6.com"), "no env uses production https");
    expect(error.isEmpty(), "no env has no error");
}

static void test_localhost_http_is_allowed()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "http://127.0.0.1:8000");
    QString error;
    const QUrl url = parseBase(&error);
    expect(url.toString() == QStringLiteral("http://127.0.0.1:8000"), "localhost http");
    expect(error.isEmpty(), "localhost http no error");
}

static void test_trailing_slash_is_stripped()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "http://127.0.0.1:8000/");
    QString error;
    expect(parseBase(&error).toString() == QStringLiteral("http://127.0.0.1:8000"), "trailing slash");
}

static void test_non_local_http_is_rejected()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "http://example.com");
    QString error;
    const QUrl url = parseBase(&error);
    expect(!url.isValid(), "non-local http rejected");
    expect(error.contains(QStringLiteral("HTTPS")), "https required message");
}

static void test_malformed_url_is_controlled_error()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "not a url");
    QString error;
    expect(!parseBase(&error).isValid(), "malformed rejected");
    expect(!error.isEmpty(), "malformed has message");
}

static void test_invalid_env_does_not_fall_back_to_production()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "http://203.0.113.10");
    QString error;
    const QUrl url = parseBase(&error);
    expect(!url.isValid(), "invalid development url rejected");
    expect(url.toString() != Nax5ApiConfig::defaultBaseUrl(), "no silent production fallback");
}

static void test_spaces_and_empty_env_are_controlled()
{
    EnvGuard guard;
    qputenv("NAX5_API_BASE_URL", "   ");
    QString error;
    expect(!parseBase(&error).isValid(), "whitespace-only env is invalid");
    qputenv("NAX5_API_BASE_URL", "http://127.0.0.1:8000 extra");
    expect(!parseBase(&error).isValid(), "invalid characters rejected");
}

static void test_remembered_login_never_persists_secrets()
{
    QTemporaryDir dir;
    expect(dir.isValid(), "auth settings temp dir");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    nax5AuthClearCredentials();
    nax5AuthSaveCredentials(QStringLiteral(" Tester@Example.com "), QStringLiteral("plain-password"),
        QStringLiteral("session-token"));
    expect(nax5AuthRememberEnabled(), "remember preference persists");
    expect(nax5AuthSavedEmail() == QStringLiteral("Tester@Example.com"), "remembered email persists");
    expect(nax5AuthSavedPassword().isEmpty(), "password is never persisted");
    expect(nax5AuthSavedSessionToken().isEmpty(), "session token is never persisted");
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        Nax5Runtime::settingsOrganizationName(), Nax5Runtime::settingsApplicationName());
    expect(!settings.contains(QStringLiteral("auth/password")), "legacy password key removed");
    expect(!settings.contains(QStringLiteral("auth/session_token")), "legacy token key removed");
}

static void test_me_play_time()
{
    const char *old_backend =
        "{\"id\":12,\"email\":\"a@example.com\",\"city\":\"\",\"accessStatus\":\"ACTIVE\",\"emailVerified\":true}";
    const Nax5MeParseResult before = nax5ParseMeResponse(200, old_backend);
    expect(before.ok && before.balance_seconds == -1 && !before.billing_enforced, "me without play time still parses");
    const char *body =
        "{\"id\":12,\"email\":\"a@example.com\",\"city\":\"\",\"accessStatus\":\"ACTIVE\",\"emailVerified\":true,"
        "\"balanceSeconds\":4320,\"billingEnforced\":true}";
    const Nax5MeParseResult result = nax5ParseMeResponse(200, body);
    expect(result.ok && result.balance_seconds == 4320 && result.billing_enforced, "me carries play time");
}

int main()
{
    test_login_success();
    test_login_does_not_expose_token_on_failure();
    test_unknown_email();
    test_unverified_email();
    test_rate_limited();
    test_server_and_malformed();
    test_me_payload();
    test_me_missing_fields();
    test_network_mapping();
    test_state_machine();
    test_remote_play_requires_authenticated();
    test_error_messages_are_user_facing();
    test_default_api_url_is_https();
    test_localhost_http_is_allowed();
    test_trailing_slash_is_stripped();
    test_non_local_http_is_rejected();
    test_malformed_url_is_controlled_error();
    test_invalid_env_does_not_fall_back_to_production();
    test_spaces_and_empty_env_are_controlled();
    test_remembered_login_never_persists_secrets();
    test_me_play_time();
    if (g_failed)
    {
        std::fprintf(stderr, "%d NAX5 auth tests failed\n", g_failed);
        return 1;
    }
    std::printf("All NAX5 auth tests passed\n");
    return 0;
}
