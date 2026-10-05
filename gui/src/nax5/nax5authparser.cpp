#include "nax5/nax5authparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

static bool parseJsonObject(const QByteArray &body, QJsonObject *object)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *object = document.object();
    return true;
}

static bool hasPendingEmailVerification(const QJsonObject &root)
{
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QJsonArray flows = data.value(QStringLiteral("flows")).toArray();
    for (int i = 0; i < flows.size(); ++i) {
        const QJsonObject flow = flows.at(i).toObject();
        if (flow.value(QStringLiteral("id")).toString() == QLatin1String("verify_email")
            && flow.value(QStringLiteral("is_pending")).toBool())
            return true;
    }
    return false;
}

static bool hasCredentialError(const QJsonObject &root)
{
    const QJsonArray errors = root.value(QStringLiteral("errors")).toArray();
    for (int i = 0; i < errors.size(); ++i) {
        const QString code = errors.at(i).toObject().value(QStringLiteral("code")).toString();
        if (code == QLatin1String("email_password_mismatch")
            || code == QLatin1String("username_password_mismatch")
            || code.contains(QLatin1String("password_mismatch")))
            return true;
    }
    return !errors.isEmpty();
}

Nax5AuthError nax5MapNetworkFailure(int http_status, bool timed_out, bool no_network)
{
    Q_UNUSED(http_status);
    if (timed_out || no_network)
        return Nax5AuthErrorNetworkError;
    return Nax5AuthErrorNetworkError;
}

Nax5LoginParseResult nax5ParseLoginResponse(int http_status, const QByteArray &body)
{
    Nax5LoginParseResult result;
    if (http_status == 429)
    {
        result.error = Nax5AuthErrorRateLimited;
        return result;
    }
    if (http_status == 403)
    {
        result.error = Nax5AuthErrorRateLimited;
        return result;
    }
    if (http_status >= 500)
    {
        result.error = Nax5AuthErrorServerError;
        return result;
    }

    QJsonObject root;
    if (!parseJsonObject(body, &root))
    {
        result.error = (http_status >= 200 && http_status < 300)
            ? Nax5AuthErrorInvalidResponse
            : (http_status == 0 ? Nax5AuthErrorNetworkError : Nax5AuthErrorInvalidResponse);
        if (http_status >= 500)
            result.error = Nax5AuthErrorServerError;
        return result;
    }

    if (http_status == 200)
    {
        const QJsonObject meta = root.value(QStringLiteral("meta")).toObject();
        const QString token = meta.value(QStringLiteral("session_token")).toString();
        if (meta.value(QStringLiteral("is_authenticated")).toBool() && !token.isEmpty())
        {
            result.error = Nax5AuthErrorNone;
            result.session_token = token;
            return result;
        }
        result.error = Nax5AuthErrorInvalidResponse;
        return result;
    }

    if (http_status == 401)
    {
        if (hasPendingEmailVerification(root))
            result.error = Nax5AuthErrorEmailNotVerified;
        else
            result.error = Nax5AuthErrorInvalidCredentials;
        return result;
    }

    if (http_status == 400)
    {
        result.error = hasCredentialError(root)
            ? Nax5AuthErrorInvalidCredentials
            : Nax5AuthErrorInvalidCredentials;
        return result;
    }

    result.error = Nax5AuthErrorInvalidResponse;
    return result;
}

Nax5MeParseResult nax5ParseMeResponse(int http_status, const QByteArray &body)
{
    Nax5MeParseResult result;
    if (http_status == 401)
        return result;
    if (http_status == 429 || http_status == 403)
        return result;
    if (http_status != 200)
        return result;

    QJsonObject root;
    if (!parseJsonObject(body, &root))
        return result;

    const QJsonValue id_value = root.value(QStringLiteral("id"));
    if (!id_value.isDouble())
        return result;
    const QString email = root.value(QStringLiteral("email")).toString();
    if (email.isEmpty())
        return result;
    if (!root.contains(QStringLiteral("city")) || !root.value(QStringLiteral("city")).isString())
        return result;
    const QString access_status = root.value(QStringLiteral("accessStatus")).toString();
    if (access_status.isEmpty())
        return result;
    if (!root.contains(QStringLiteral("emailVerified")) || !root.value(QStringLiteral("emailVerified")).isBool())
        return result;

    result.ok = true;
    result.user_id = id_value.toVariant().toLongLong();
    result.email = email;
    result.city = root.value(QStringLiteral("city")).toString();
    result.access_status = access_status;
    result.email_verified = root.value(QStringLiteral("emailVerified")).toBool();
    // Optional: older backends do not send play time.
    const QJsonValue balance = root.value(QStringLiteral("balanceSeconds"));
    if (balance.isDouble() && balance.toDouble() >= 0)
        result.balance_seconds = balance.toVariant().toLongLong();
    result.billing_enforced = root.value(QStringLiteral("billingEnforced")).toBool(false);
    result.console_state = root.value(QStringLiteral("consoleState")).toString();
    const QJsonValue free_in = root.value(QStringLiteral("consoleFreeInSeconds"));
    if (free_in.isDouble() && free_in.toDouble() >= 0)
        result.console_free_in_seconds = free_in.toVariant().toLongLong();
    return result;
}
