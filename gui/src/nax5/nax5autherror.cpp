#include "nax5/nax5autherror.h"

#include <QNetworkReply>

QString nax5NetworkFailureDetail(int qt_network_error)
{
    QString reason;
    switch (qt_network_error) {
    case QNetworkReply::HostNotFoundError:
        reason = QStringLiteral("не найден адрес сервера (DNS)");
        break;
    case QNetworkReply::ConnectionRefusedError:
        reason = QStringLiteral("сервер отклонил соединение");
        break;
    case QNetworkReply::RemoteHostClosedError:
        reason = QStringLiteral("сервер оборвал соединение");
        break;
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        reason = QStringLiteral("сервер не ответил вовремя");
        break;
    case QNetworkReply::SslHandshakeFailedError:
        reason = QStringLiteral("ошибка защищённого соединения, проверьте дату и время на ПК, антивирус и VPN");
        break;
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
        reason = QStringLiteral("ошибка прокси");
        break;
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        reason = QStringLiteral("сеть недоступна");
        break;
    default:
        reason = QStringLiteral("соединение не удалось");
        break;
    }
    return QStringLiteral("%1 (код %2)").arg(reason).arg(qt_network_error);
}

QString nax5AuthErrorMessage(Nax5AuthError error, const QString &detail)
{
    switch (error) {
    case Nax5AuthErrorInvalidCredentials:
        return QStringLiteral("Неверный email или пароль.");
    case Nax5AuthErrorEmailNotVerified:
        return QStringLiteral("Подтвердите email перед входом.");
    case Nax5AuthErrorRateLimited:
        return QStringLiteral("Слишком много попыток. Попробуйте позже.");
    case Nax5AuthErrorNetworkError:
        if (!detail.isEmpty())
            return QStringLiteral("Нет связи с сервером: %1.").arg(detail);
        return QStringLiteral("Нет подключения к интернету.");
    case Nax5AuthErrorServerError:
        return QStringLiteral("Сервис временно недоступен.");
    case Nax5AuthErrorInvalidResponse:
        return QStringLiteral("Сервис временно недоступен.");
    case Nax5AuthErrorNone:
        return QString();
    }
    return QStringLiteral("Сервис временно недоступен.");
}
