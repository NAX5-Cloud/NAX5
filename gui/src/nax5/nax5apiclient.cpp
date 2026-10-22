#include "nax5/nax5apiclient.h"
#include "nax5/nax5apiconfig.h"
#include "nax5/nax5processlog.h"

#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

Q_LOGGING_CATEGORY(nax5Api, "nax5.api")

Nax5ApiClient::Nax5ApiClient(QObject *parent)
    : QObject(parent)
    , network(new QNetworkAccessManager(this))
    , next_request_id(1)
{
}

Nax5ApiClient::~Nax5ApiClient()
{
    abortAll();
}

void Nax5ApiClient::abortInFlight(InFlight &item)
{
    QNetworkReply *reply = item.reply.data();
    item.reply.clear();
    item.request_id = 0;
    if (!reply)
        return;
    reply->abort();
    reply->deleteLater();
}

void Nax5ApiClient::abortLane(Nax5ApiLane lane)
{
    for (int i = in_flight.size() - 1; i >= 0; --i)
    {
        if (in_flight[i].lane != lane || in_flight[i].request_id == 0)
            continue;
        abortInFlight(in_flight[i]);
        in_flight.removeAt(i);
    }
}

void Nax5ApiClient::abortAll()
{
    for (int i = 0; i < in_flight.size(); ++i)
        abortInFlight(in_flight[i]);
    in_flight.clear();
}

bool Nax5ApiClient::hasLane(Nax5ApiLane lane) const
{
    for (const InFlight &item : in_flight)
    {
        if (item.lane == lane && item.request_id != 0 && !item.reply.isNull())
            return true;
    }
    return false;
}

bool Nax5ApiClient::completeLive(quint64 request_id)
{
    for (int i = 0; i < in_flight.size(); ++i)
    {
        if (in_flight[i].request_id != request_id)
            continue;
        in_flight.removeAt(i);
        return true;
    }
    return false;
}

QNetworkReply *Nax5ApiClient::sendJson(Nax5ApiLane lane, quint64 request_id, const QString &method, const QString &path, const QByteArray &body, const QString &session_token, const QByteArray &idempotency_key)
{
    for (int i = in_flight.size() - 1; i >= 0; --i)
    {
        if (in_flight[i].request_id == 0)
        {
            in_flight.removeAt(i);
            continue;
        }
        if (nax5ApiShouldAbortExisting(in_flight[i].lane, lane))
        {
            abortInFlight(in_flight[i]);
            in_flight.removeAt(i);
        }
    }

    QString config_error;
    const QUrl base = Nax5ApiConfig::baseUrl(&config_error);
    if (!base.isValid())
        return nullptr;

    QUrl url = base.resolved(QUrl(path));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, Nax5ApiConfig::userAgent());
    request.setRawHeader("X-NAX5-Client-Version", nax5ClientVersion().toUtf8());
    request.setRawHeader("X-NAX5-Client-SHA", nax5ClientSha().toUtf8());
    request.setTransferTimeout(Nax5ApiConfig::requestTimeoutMs());
    if (!session_token.isEmpty())
        request.setRawHeader("X-Session-Token", session_token.toUtf8());
    if (!idempotency_key.isEmpty())
        request.setRawHeader("Idempotency-Key", idempotency_key);

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("POST"))
        reply = network->post(request, body);
    else if (method == QLatin1String("DELETE"))
        reply = network->sendCustomRequest(request, "DELETE", body.isEmpty() ? QByteArray("{}") : body);
    else
        reply = network->get(request);

    InFlight item;
    item.request_id = request_id;
    item.lane = lane;
    item.reply = reply;
    in_flight.append(item);

    if (path != QLatin1String("/_allauth/app/v1/auth/login"))
        qCInfo(nax5Api) << method << path << "lane" << static_cast<int>(lane) << "id" << request_id;
    return reply;
}

bool Nax5ApiClient::isNoNetwork(QNetworkReply *reply)
{
    const QNetworkReply::NetworkError error = reply->error();
    return error == QNetworkReply::HostNotFoundError
        || error == QNetworkReply::ConnectionRefusedError
        || error == QNetworkReply::RemoteHostClosedError
        || error == QNetworkReply::TemporaryNetworkFailureError
        || error == QNetworkReply::NetworkSessionFailedError
        || error == QNetworkReply::UnknownNetworkError
        || error == QNetworkReply::TimeoutError
        || error == QNetworkReply::OperationCanceledError;
}

quint64 Nax5ApiClient::login(const QString &email, const QString &password)
{
    const quint64 request_id = next_request_id++;
    qCInfo(nax5Api) << "login start";
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("password"), password);
    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    payload.insert(QStringLiteral("password"), QString());

    QNetworkReply *reply = sendJson(Nax5ApiLaneAuth, request_id, QStringLiteral("POST"), QStringLiteral("/_allauth/app/v1/auth/login"), body, QString());
    body.fill(' ');
    if (!reply)
    {
        Nax5LoginParseResult result;
        result.error = Nax5AuthErrorServerError;
        emit loginFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishLogin(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::fetchMe(const QString &session_token)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(Nax5ApiLaneAuth, request_id, QStringLiteral("GET"), QStringLiteral("/api/v1/auth/me/"), QByteArray(), session_token);
    if (!reply)
    {
        emit meFinished(request_id, Nax5MeParseResult());
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishMe(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::logout(const QString &session_token)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(Nax5ApiLaneAuth, request_id, QStringLiteral("DELETE"), QStringLiteral("/_allauth/app/v1/auth/session"), QByteArray(), session_token);
    if (!reply)
    {
        emit logoutFinished(request_id);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishLogout(request_id, reply);
    });
    return request_id;
}

void Nax5ApiClient::finishLogin(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }

    Nax5LoginParseResult result;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && status == 0)
    {
        result.error = nax5MapNetworkFailure(status, reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError, isNoNetwork(reply));
        result.detail = nax5NetworkFailureDetail(static_cast<int>(reply->error()));
        qCWarning(nax5Api) << "login fail" << status << "qt_error" << static_cast<int>(reply->error()) << reply->errorString();
        emit loginFinished(request_id, result);
        reply->deleteLater();
        return;
    }

    result = nax5ParseLoginResponse(status, reply->readAll());
    if (result.error != Nax5AuthErrorNone)
        qCWarning(nax5Api) << "login fail" << status;
    else
        qCInfo(nax5Api) << "login ok" << status;
    emit loginFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishMe(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    Nax5MeParseResult result;
    if (reply->error() != QNetworkReply::NoError && status == 0)
        result.network_failure = true;
    else
        result = nax5ParseMeResponse(status, reply->readAll());
    emit meFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishLogout(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    emit logoutFinished(request_id);
    reply->deleteLater();
}

quint64 Nax5ApiClient::reserveSession(const QString &session_token, const QString &idempotency_key)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(
        Nax5ApiLaneReserve,
        request_id,
        QStringLiteral("POST"),
        QStringLiteral("/api/v1/sessions/reserve/"),
        QByteArray("{}"),
        session_token,
        idempotency_key.toUtf8());
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit reserveFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishReserve(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::fetchCurrentSession(const QString &session_token)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(Nax5ApiLaneQuery, request_id, QStringLiteral("GET"), QStringLiteral("/api/v1/sessions/current/"), QByteArray(), session_token);
    if (!reply)
    {
        emit currentFinished(request_id, Nax5SessionParseResult());
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishCurrent(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::cancelSession(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/cancel/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneTerminal, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit cancelFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishCancel(request_id, reply);
    });
    return request_id;
}

Nax5SessionParseResult Nax5ApiClient::finishSessionNetwork(QNetworkReply *reply, bool *used_body)
{
    Nax5SessionParseResult result;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && status == 0)
    {
        result.error = nax5MapSessionHttpError(status, reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError, isNoNetwork(reply));
        *used_body = false;
        return result;
    }
    *used_body = true;
    result.error = Nax5SessionErrorNone;
    return result;
}

void Nax5ApiClient::finishReserve(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }

    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseReserveResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    if (result.error != Nax5SessionErrorNone)
        qCWarning(nax5Api) << "reserve failed:" << result.error;
    else
        qCInfo(nax5Api) << "reserve ok" << result.session.id << result.console.code;
    emit reserveFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishCurrent(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }

    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCurrentResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    emit currentFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishCancel(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }

    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCancelResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    if (result.error == Nax5SessionErrorNone)
        qCInfo(nax5Api) << "cancel ok" << result.session.id;
    else
        qCWarning(nax5Api) << "cancel failed:" << result.error;
    emit cancelFinished(request_id, result);
    reply->deleteLater();
}

quint64 Nax5ApiClient::fetchConnection(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/connection/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneConnection, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5ConnectionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit connectionFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishConnection(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::markConnected(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/connected/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneTerminal, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit connectedFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishConnected(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::heartbeatSession(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/heartbeat/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneTerminal, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit heartbeatFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishHeartbeat(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::failSession(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/fail/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneTerminal, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit failFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishFail(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::endSession(const QString &session_token, const QString &public_session_id)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/sessions/%1/end/").arg(public_session_id);
    QNetworkReply *reply = sendJson(Nax5ApiLaneTerminal, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5SessionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit endFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishEnd(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::operatorProvision(const QString &session_token, const QString &console_code, const QByteArray &body)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/operator/consoles/%1/provision/").arg(console_code);
    QNetworkReply *reply = sendJson(Nax5ApiLaneOperator, request_id, QStringLiteral("POST"), path, body, session_token);
    if (!reply)
    {
        Nax5ConnectionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit operatorFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishOperator(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::operatorActivate(const QString &session_token, const QString &console_code)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/operator/consoles/%1/activate/").arg(console_code);
    QNetworkReply *reply = sendJson(Nax5ApiLaneOperator, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5ConnectionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit operatorFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishOperator(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::operatorTestConnection(const QString &session_token, const QString &console_code)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/operator/consoles/%1/test-connection/").arg(console_code);
    QNetworkReply *reply = sendJson(Nax5ApiLaneConnection, request_id, QStringLiteral("POST"), path, QByteArray("{}"), session_token);
    if (!reply)
    {
        Nax5ConnectionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit connectionFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishConnection(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::operatorTestResult(const QString &session_token, const QString &console_code, const QByteArray &body)
{
    const quint64 request_id = next_request_id++;
    const QString path = QStringLiteral("/api/v1/operator/consoles/%1/test-result/").arg(console_code);
    QNetworkReply *reply = sendJson(Nax5ApiLaneOperator, request_id, QStringLiteral("POST"), path, body, session_token);
    if (!reply)
    {
        Nax5ConnectionParseResult result;
        result.error = Nax5SessionErrorServerError;
        emit operatorFinished(request_id, result);
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishOperator(request_id, reply);
    });
    return request_id;
}

void Nax5ApiClient::finishConnection(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    Nax5ConnectionParseResult result;
    if (reply->error() != QNetworkReply::NoError && status == 0)
        result.error = Nax5SessionErrorNetworkError;
    else
        result = nax5ParseConnectionResponse(status, body);
    qCInfo(nax5Api) << "connection" << status;
    emit connectionFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishConnected(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCurrentResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    emit connectedFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishHeartbeat(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCancelResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    if (result.error != Nax5SessionErrorNone)
        qCWarning(nax5Api) << "heartbeat failed:" << result.error;
    emit heartbeatFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishFail(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCancelResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    emit failFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishEnd(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    bool used_body = false;
    Nax5SessionParseResult result = finishSessionNetwork(reply, &used_body);
    if (used_body)
        result = nax5ParseCancelResponse(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
    emit endFinished(request_id, result);
    reply->deleteLater();
}

void Nax5ApiClient::finishOperator(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    Nax5ConnectionParseResult result = nax5ParseOperatorProvisionResponse(status, body);
    qCInfo(nax5Api) << "operator" << status;
    emit operatorFinished(request_id, result);
    reply->deleteLater();
}

quint64 Nax5ApiClient::submitClientEvents(const QString &session_token, const QByteArray &body)
{
    return postClientEvents(session_token, body);
}

quint64 Nax5ApiClient::postClientEvents(const QString &session_token, const QByteArray &body)
{
    return postClientEventsAttempt(session_token, body, 0);
}

quint64 Nax5ApiClient::postClientEventsAttempt(const QString &session_token, const QByteArray &body, int attempt)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(
        Nax5ApiLaneTelemetry,
        request_id,
        QStringLiteral("POST"),
        QStringLiteral("/api/v1/client-events/"),
        body,
        session_token);
    if (!reply)
        return request_id;
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply, session_token, body, attempt]() {
        if (!completeLive(request_id))
        {
            reply->deleteLater();
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && status != 201 && status != 204)
            qCWarning(nax5Api) << "client-events upload failed" << status << "attempt" << attempt;
        const bool retryable = status == 500 || status == 503;
        if (retryable && attempt < 2)
        {
            const int delay_ms = 250 * (1 << attempt);
            QTimer::singleShot(delay_ms, this, [this, session_token, body, attempt]() {
                postClientEventsAttempt(session_token, body, attempt + 1);
            });
        }
        reply->deleteLater();
    });
    return request_id;
}

quint64 Nax5ApiClient::postClientReportArchive(
    const QString &session_token,
    Nax5ClientReportKind kind,
    const QString &session_public_id,
    const QByteArray &zip_bytes,
    const QString &report_version,
    const QString &report_sha)
{
    const quint64 request_id = next_request_id++;
    QString config_error;
    const QUrl base = Nax5ApiConfig::baseUrl(&config_error);
    if (!base.isValid() || zip_bytes.isEmpty())
    {
        qCWarning(nax5Api) << "client-report archive upload skipped";
        QTimer::singleShot(0, this, [this, request_id]() { emit clientReportFinished(request_id, 0); });
        return request_id;
    }

    QHttpMultiPart *multi_part = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto appendField = [&](const QString &name, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant(QStringLiteral("form-data; name=\"%1\"").arg(name)));
        part.setBody(value);
        multi_part->append(part);
    };
    appendField(QStringLiteral("kind"), nax5ClientReportKindName(kind).toUtf8());
    appendField(QStringLiteral("client_version"), (report_version.isEmpty() ? nax5ClientVersion() : report_version).toUtf8());
    appendField(QStringLiteral("client_sha"), (report_sha.isEmpty() ? nax5ClientSha() : report_sha).toUtf8());
    if (!session_public_id.isEmpty())
        appendField(QStringLiteral("session_id"), session_public_id.toUtf8());

    QHttpPart file_part;
    file_part.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QVariant(QStringLiteral("form-data; name=\"archive\"; filename=\"report.zip\"")));
    file_part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QStringLiteral("application/zip")));
    file_part.setBody(zip_bytes);
    multi_part->append(file_part);

    QUrl url = base.resolved(QUrl(QStringLiteral("/api/v1/client-reports/")));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, Nax5ApiConfig::userAgent());
    request.setTransferTimeout(Nax5ApiConfig::requestTimeoutMs());
    if (!session_token.isEmpty())
        request.setRawHeader("X-Session-Token", session_token.toUtf8());

    QNetworkReply *reply = network->post(request, multi_part);
    if (!reply)
    {
        delete multi_part;
        qCWarning(nax5Api) << "client-report archive upload failed" << 0;
        QTimer::singleShot(0, this, [this, request_id]() { emit clientReportFinished(request_id, 0); });
        return request_id;
    }
    multi_part->setParent(reply);

    InFlight item;
    item.request_id = request_id;
    item.lane = Nax5ApiLaneReport;
    item.reply = reply;
    in_flight.append(item);
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishClientReport(request_id, reply);
    });
    return request_id;
}

quint64 Nax5ApiClient::postClientReport(const QString &session_token, const QByteArray &body)
{
    const quint64 request_id = next_request_id++;
    QNetworkReply *reply = sendJson(
        Nax5ApiLaneReport,
        request_id,
        QStringLiteral("POST"),
        QStringLiteral("/api/v1/client-reports/"),
        body,
        session_token);
    if (!reply)
    {
        qCWarning(nax5Api) << "client-report upload failed" << 0;
        QTimer::singleShot(0, this, [this, request_id]() { emit clientReportFinished(request_id, 0); });
        return request_id;
    }
    connect(reply, &QNetworkReply::finished, this, [this, request_id, reply]() {
        finishClientReport(request_id, reply);
    });
    return request_id;
}

void Nax5ApiClient::finishClientReport(quint64 request_id, QNetworkReply *reply)
{
    if (!completeLive(request_id))
    {
        reply->deleteLater();
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200 && status != 201 && status != 204)
        qCWarning(nax5Api) << "client-report upload failed" << status;
    else
        qCInfo(nax5Api) << "client-report ok" << status;
    emit clientReportFinished(request_id, status);
    reply->deleteLater();
}
