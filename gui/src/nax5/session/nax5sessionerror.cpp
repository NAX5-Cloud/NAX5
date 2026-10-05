#include "nax5/session/nax5sessionerror.h"

static QString productMessage(Nax5SessionError error)
{
    switch (error) {
    case Nax5SessionErrorNoCapacity:
        return QStringLiteral("Все консоли сейчас заняты.\nПопробуйте немного позже.");
    case Nax5SessionErrorUserNotEligible:
        return QStringLiteral("Этот аккаунт пока не может получить консоль.");
    case Nax5SessionErrorActiveSessionExists:
        return QStringLiteral("У вас уже есть выделенная консоль.");
    case Nax5SessionErrorUnauthenticated:
        return QStringLiteral("Сессия входа истекла. Войдите снова.");
    case Nax5SessionErrorNetworkError:
        return QStringLiteral("Нет подключения к интернету.");
    case Nax5SessionErrorRateLimited:
        return QStringLiteral("Слишком много попыток. Попробуйте позже.");
    case Nax5SessionErrorServerError:
        return QStringLiteral("Сервис временно недоступен.");
    case Nax5SessionErrorInvalidResponse:
        return QStringLiteral("Сервис временно недоступен.");
    case Nax5SessionErrorForbidden:
        return QStringLiteral("Нельзя освободить чужую сессию.");
    case Nax5SessionErrorNotFound:
        return QStringLiteral("Игровая сессия не найдена.");
    case Nax5SessionErrorInvalidConnectionMaterial:
    case Nax5SessionErrorHostNotConfigured:
    case Nax5SessionErrorHostUnreachable:
    case Nax5SessionErrorConnectionTimeout:
    case Nax5SessionErrorOperatorTestRequired:
        return QStringLiteral("Не удалось подключиться к консоли.");
    case Nax5SessionErrorClientUpdateRequired:
        return QStringLiteral("Доступно обязательное обновление NAX5. Установите новую версию, чтобы продолжить игру.");
    case Nax5SessionErrorInsufficientBalance:
        return QStringLiteral("Игровое время закончилось.\nПополните его в аккаунте на сайте и нажмите «Играть».");
    case Nax5SessionErrorConsoleOffline:
        return QStringLiteral("Консоль сейчас выключена." "\n" "Мы уже знаем об этом и включим её. Попробуйте позже.");
    case Nax5SessionErrorSessionCooldown:
        return QStringLiteral("Консоль ждут другие игроки.\nВы играли больше 3 часов, попробуйте снова через 10 минут.");
    case Nax5SessionErrorNone:
        return QString();
    }
    return QStringLiteral("Сервис временно недоступен.");
}

static QString operatorMessage(Nax5SessionError error)
{
    switch (error) {
    case Nax5SessionErrorHostNotConfigured:
        return QStringLiteral("Test failed: console host not configured");
    case Nax5SessionErrorHostUnreachable:
        return QStringLiteral("Test failed: PS5 unreachable");
    case Nax5SessionErrorInvalidConnectionMaterial:
        return QStringLiteral("Test failed: Remote Play authentication rejected");
    case Nax5SessionErrorConnectionTimeout:
    case Nax5SessionErrorNetworkError:
        return QStringLiteral("Test failed: timeout");
    case Nax5SessionErrorOperatorTestRequired:
        return QStringLiteral("Test failed: a successful Remote Play test is required before activate");
    case Nax5SessionErrorNone:
        return QString();
    default:
        break;
    }
    return productMessage(error);
}

QString nax5SessionErrorMessage(Nax5SessionError error)
{
    return nax5SessionErrorMessage(error, false);
}

QString nax5SessionErrorMessage(Nax5SessionError error, bool operator_mode)
{
    return operator_mode ? operatorMessage(error) : productMessage(error);
}
