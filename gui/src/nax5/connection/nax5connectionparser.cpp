#include "nax5/connection/nax5connectionparser.h"

#include "nax5/session/nax5sessionparser.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

static bool parseJsonObject(const QByteArray &body, QJsonObject *object)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *object = document.object();
    return true;
}

bool nax5TargetIsSupportedDirectPs5(int target)
{
    // ChiakiTarget PS5 values from lib/include/chiaki/common.h
    return target == 1000000 || target == 1000100;
}

bool nax5DecodeRpKey(const QString &base64, int expected_size, QByteArray *out)
{
    if (base64.trimmed().isEmpty() || out == nullptr)
        return false;
    QByteArray decoded = QByteArray::fromBase64(base64.toUtf8(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.size() != expected_size)
        return false;
    *out = decoded;
    return true;
}

static Nax5SessionError connectionErrorFromCode(const QString &code, int http_status)
{
    if (code == QLatin1String("UNAUTHENTICATED"))
        return Nax5SessionErrorUnauthenticated;
    if (code == QLatin1String("SESSION_NOT_OWNED") || code == QLatin1String("OPERATOR_FORBIDDEN"))
        return Nax5SessionErrorForbidden;
    if (code == QLatin1String("SESSION_NOT_FOUND"))
        return Nax5SessionErrorNotFound;
    if (code == QLatin1String("CONSOLE_HOST_NOT_CONFIGURED"))
        return Nax5SessionErrorHostNotConfigured;
    if (code == QLatin1String("OPERATOR_TEST_REQUIRED"))
        return Nax5SessionErrorOperatorTestRequired;
    if (code == QLatin1String("INVALID_CONNECTION_MATERIAL")
        || code == QLatin1String("UNSUPPORTED_CONNECTION_VERSION")
        || code == QLatin1String("CONNECTION_NOT_AVAILABLE")
        || code == QLatin1String("CONSOLE_UNAVAILABLE")
        || code == QLatin1String("CONNECTION_MATERIAL_MISSING")
        || code == QLatin1String("INVALID_STATE"))
        return Nax5SessionErrorInvalidConnectionMaterial;
    if (http_status == 401)
        return Nax5SessionErrorUnauthenticated;
    if (http_status == 403)
        return Nax5SessionErrorForbidden;
    if (http_status == 404)
        return Nax5SessionErrorNotFound;
    if (http_status == 429)
        return Nax5SessionErrorRateLimited;
    if (http_status >= 500)
        return Nax5SessionErrorServerError;
    return Nax5SessionErrorInvalidResponse;
}

static bool parsePort(const QJsonObject &ports, const QString &name, uint16_t *out)
{
    const int value = ports.value(name).toInt();
    if (value < 1 || value > 65535)
        return false;
    *out = static_cast<uint16_t>(value);
    return true;
}

static bool parseConnectionObject(const QJsonObject &connection, Nax5ConnectionMaterial *material)
{
    if (!connection.contains(QStringLiteral("version")) || !connection.value(QStringLiteral("version")).isDouble())
        return false;
    const int version = connection.value(QStringLiteral("version")).toInt();
    if (version != Nax5ConnectionContractVersion && version != Nax5DirectPortsContractVersion)
        return false;
    if (version == Nax5ConnectionContractVersion && connection.contains(QStringLiteral("ports")))
        return false;
    uint16_t session_control_port = 0, stream_port = 0, senkusha_port = 0, discovery_port = 0;
    if (version == Nax5DirectPortsContractVersion)
    {
        const auto ports = connection.value(QStringLiteral("ports")).toObject();
        if (!parsePort(ports, QStringLiteral("sessionControlTcp"), &session_control_port)
            || !parsePort(ports, QStringLiteral("streamUdp"), &stream_port)
            || !parsePort(ports, QStringLiteral("senkushaUdp"), &senkusha_port)
            || !parsePort(ports, QStringLiteral("discoveryUdp"), &discovery_port)
            || stream_port == senkusha_port || stream_port == discovery_port || senkusha_port == discovery_port)
            return false;
    }
    if (!connection.contains(QStringLiteral("target")) || !connection.value(QStringLiteral("target")).isDouble())
        return false;
    const int target = connection.value(QStringLiteral("target")).toInt();
    if (!nax5TargetIsSupportedDirectPs5(target))
        return false;
    const QString host = connection.value(QStringLiteral("host")).toString().trimmed();
    if (host.isEmpty())
        return false;
    QByteArray regist_key;
    QByteArray morning;
    if (!nax5DecodeRpKey(connection.value(QStringLiteral("registKey")).toString(), Nax5RegistKeySize, &regist_key))
        return false;
    if (!nax5DecodeRpKey(connection.value(QStringLiteral("morning")).toString(), Nax5MorningSize, &morning))
        return false;

    material->version = version;
    material->target = target;
    material->session_control_port = session_control_port;
    material->stream_port = stream_port;
    material->senkusha_port = senkusha_port;
    material->discovery_port = discovery_port;
    material->host = host;
    material->nickname = connection.value(QStringLiteral("nickname")).toString();
    material->regist_key = regist_key;
    material->morning = morning;
    material->console_pin = connection.value(QStringLiteral("consolePin")).toString();
    return true;
}

Nax5ConnectionParseResult nax5ParseConnectionResponse(int http_status, const QByteArray &body)
{
    Nax5ConnectionParseResult result;
    QJsonObject root;
    if (!parseJsonObject(body, &root))
    {
        if (http_status == 0)
            result.error = Nax5SessionErrorNetworkError;
        else if (http_status >= 500)
            result.error = Nax5SessionErrorServerError;
        else
            result.error = Nax5SessionErrorInvalidResponse;
        return result;
    }

    if (http_status != 200)
    {
        result.error = connectionErrorFromCode(root.value(QStringLiteral("code")).toString(), http_status);
        return result;
    }

    const QJsonValue session_value = root.value(QStringLiteral("session"));
    if (!session_value.isObject())
    {
        result.error = Nax5SessionErrorInvalidResponse;
        return result;
    }
    const QJsonObject session = session_value.toObject();
    const QString session_id = session.value(QStringLiteral("id")).toString();
    const QString status = session.value(QStringLiteral("status")).toString();
    if (session_id.isEmpty() || status.isEmpty())
    {
        result.error = Nax5SessionErrorInvalidResponse;
        return result;
    }

    const QJsonValue connection_value = root.value(QStringLiteral("connection"));
    if (!connection_value.isObject() || !parseConnectionObject(connection_value.toObject(), &result.material))
    {
        result.error = Nax5SessionErrorInvalidResponse;
        result.material.clear();
        return result;
    }

    result.material.session_id = session_id;
    result.material.session_status = status;
    result.has_material = true;
    result.error = Nax5SessionErrorNone;
    return result;
}

Nax5ConnectionParseResult nax5ParseOperatorProvisionResponse(int http_status, const QByteArray &body)
{
    Nax5ConnectionParseResult result;
    QJsonObject root;
    if (!parseJsonObject(body, &root))
    {
        result.error = http_status >= 500 ? Nax5SessionErrorServerError : Nax5SessionErrorInvalidResponse;
        return result;
    }
    if (http_status != 200)
    {
        result.error = connectionErrorFromCode(root.value(QStringLiteral("code")).toString(), http_status);
        return result;
    }
    result.error = Nax5SessionErrorNone;
    return result;
}
