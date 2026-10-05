#include "nax5/session/nax5sessionparser.h"

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

static bool parseSessionObject(const QJsonObject &session, Nax5SessionInfo *info)
{
    const QString id = session.value(QStringLiteral("id")).toString();
    const QString status = session.value(QStringLiteral("status")).toString();
    if (id.isEmpty() || status.isEmpty())
        return false;
    if (!session.contains(QStringLiteral("reservedAt")) || !session.value(QStringLiteral("reservedAt")).isString())
        return false;
    info->id = id;
    info->status = status;
    info->reserved_at = session.value(QStringLiteral("reservedAt")).toString();
    if (session.contains(QStringLiteral("leaseExpiresAt")) && session.value(QStringLiteral("leaseExpiresAt")).isString())
        info->lease_expires_at = session.value(QStringLiteral("leaseExpiresAt")).toString();
    else
        info->lease_expires_at.clear();
    return true;
}

static bool parseConsoleObject(const QJsonObject &console, Nax5AssignedConsole *assigned)
{
    const QString code = console.value(QStringLiteral("code")).toString();
    if (code.isEmpty())
        return false;
    if (!console.contains(QStringLiteral("region")) || !console.value(QStringLiteral("region")).isString())
        return false;
    assigned->code = code;
    assigned->region = console.value(QStringLiteral("region")).toString();
    return true;
}

static bool parseAssignment(const QJsonObject &root, Nax5SessionParseResult *result)
{
    const QJsonValue session_value = root.value(QStringLiteral("session"));
    if (!session_value.isObject())
        return false;
    if (!parseSessionObject(session_value.toObject(), &result->session))
        return false;
    result->has_session = true;
    const QJsonValue remaining = root.value(QStringLiteral("remainingSeconds"));
    if (remaining.isDouble() && remaining.toDouble() >= 0)
        result->remaining_seconds = remaining.toVariant().toLongLong();
    const QJsonValue console_value = root.value(QStringLiteral("console"));
    if (console_value.isObject() && parseConsoleObject(console_value.toObject(), &result->console))
        result->has_console = true;
    return true;
}

static Nax5SessionError errorFromCode(const QString &code, int http_status)
{
    if (code == QLatin1String("NO_CAPACITY"))
        return Nax5SessionErrorNoCapacity;
    if (code == QLatin1String("USER_NOT_ELIGIBLE"))
        return Nax5SessionErrorUserNotEligible;
    if (code == QLatin1String("INSUFFICIENT_BALANCE"))
        return Nax5SessionErrorInsufficientBalance;
    if (code == QLatin1String("CONSOLE_OFFLINE"))
        return Nax5SessionErrorConsoleOffline;
    if (code == QLatin1String("ACTIVE_SESSION_EXISTS"))
        return Nax5SessionErrorActiveSessionExists;
    if (code == QLatin1String("UNAUTHENTICATED"))
        return Nax5SessionErrorUnauthenticated;
    if (code == QLatin1String("SESSION_NOT_OWNED"))
        return Nax5SessionErrorForbidden;
    if (code == QLatin1String("SESSION_NOT_FOUND"))
        return Nax5SessionErrorNotFound;
    if (code == QLatin1String("CLIENT_UPDATE_REQUIRED"))
        return Nax5SessionErrorClientUpdateRequired;
    if (http_status == 401)
        return Nax5SessionErrorUnauthenticated;
    if (http_status == 403)
        return Nax5SessionErrorUserNotEligible;
    if (http_status == 404)
        return Nax5SessionErrorNotFound;
    if (http_status == 409)
        return Nax5SessionErrorNoCapacity;
    if (http_status == 429)
        return Nax5SessionErrorRateLimited;
    if (http_status >= 500)
        return Nax5SessionErrorServerError;
    return Nax5SessionErrorInvalidResponse;
}

Nax5SessionError nax5MapSessionHttpError(int http_status, bool timed_out, bool no_network)
{
    Q_UNUSED(http_status);
    if (timed_out || no_network)
        return Nax5SessionErrorNetworkError;
    return Nax5SessionErrorNetworkError;
}

static Nax5SessionParseResult parseDomainError(int http_status, const QByteArray &body)
{
    Nax5SessionParseResult result;
    if (http_status == 429)
    {
        QJsonObject limited;
        const bool cooldown = parseJsonObject(body, &limited)
            && limited.value(QStringLiteral("reason")).toString() == QLatin1String("SESSION_COOLDOWN");
        result.error = cooldown ? Nax5SessionErrorSessionCooldown : Nax5SessionErrorRateLimited;
        const QJsonValue retry = limited.value(QStringLiteral("retryAfterSeconds"));
        if (retry.isDouble() && retry.toDouble() > 0)
            result.retry_after_seconds = retry.toVariant().toLongLong();
        return result;
    }
    if (http_status >= 500)
    {
        result.error = Nax5SessionErrorServerError;
        return result;
    }

    QJsonObject root;
    if (!parseJsonObject(body, &root))
    {
        if (http_status == 401)
            result.error = Nax5SessionErrorUnauthenticated;
        else if (http_status == 0)
            result.error = Nax5SessionErrorNetworkError;
        else
            result.error = Nax5SessionErrorInvalidResponse;
        return result;
    }

    result.error = errorFromCode(root.value(QStringLiteral("code")).toString(), http_status);
    result.reason = root.value(QStringLiteral("reason")).toString();
    const QJsonValue retry = root.value(QStringLiteral("retryAfterSeconds"));
    if (retry.isDouble() && retry.toDouble() > 0)
        result.retry_after_seconds = retry.toVariant().toLongLong();
    const QJsonValue balance = root.value(QStringLiteral("balanceSeconds"));
    if (balance.isDouble() && balance.toDouble() >= 0)
        result.balance_seconds = balance.toVariant().toLongLong();
    if (result.error == Nax5SessionErrorClientUpdateRequired)
    {
        result.minimum_version = root.value(QStringLiteral("minimumVersion")).toString();
        result.update_url = root.value(QStringLiteral("updateUrl")).toString();
    }
    if (result.error == Nax5SessionErrorActiveSessionExists)
        parseAssignment(root, &result);
    return result;
}

Nax5SessionParseResult nax5ParseReserveResponse(int http_status, const QByteArray &body)
{
    if (http_status != 200)
        return parseDomainError(http_status, body);

    Nax5SessionParseResult result;
    QJsonObject root;
    if (!parseJsonObject(body, &root) || !parseAssignment(root, &result) || !result.has_console)
    {
        result.error = Nax5SessionErrorInvalidResponse;
        result.has_session = false;
        result.has_console = false;
        return result;
    }
    result.error = Nax5SessionErrorNone;
    return result;
}

Nax5SessionParseResult nax5ParseCurrentResponse(int http_status, const QByteArray &body)
{
    if (http_status != 200)
        return parseDomainError(http_status, body);

    Nax5SessionParseResult result;
    QJsonObject root;
    if (!parseJsonObject(body, &root) || !root.contains(QStringLiteral("session")))
    {
        result.error = Nax5SessionErrorInvalidResponse;
        return result;
    }
    if (root.value(QStringLiteral("session")).isNull())
    {
        result.error = Nax5SessionErrorNone;
        result.has_session = false;
        return result;
    }
    if (!parseAssignment(root, &result))
    {
        result.error = Nax5SessionErrorInvalidResponse;
        result.has_session = false;
        result.has_console = false;
        return result;
    }
    result.error = Nax5SessionErrorNone;
    return result;
}

Nax5SessionParseResult nax5ParseCancelResponse(int http_status, const QByteArray &body)
{
    if (http_status != 200)
        return parseDomainError(http_status, body);

    Nax5SessionParseResult result;
    QJsonObject root;
    if (!parseJsonObject(body, &root) || !parseAssignment(root, &result))
    {
        result.error = Nax5SessionErrorInvalidResponse;
        result.has_session = false;
        result.has_console = false;
        return result;
    }
    result.error = Nax5SessionErrorNone;
    return result;
}

bool nax5SessionPayloadLooksLeaky(const QByteArray &body)
{
    const QString text = QString::fromUtf8(body);
    return text.contains(QLatin1String("public_host"))
        || text.contains(QLatin1String("publicHost"))
        || text.contains(QLatin1String("regist_key"))
        || text.contains(QLatin1String("morning"))
        || text.contains(QLatin1String("registKey"));
}
