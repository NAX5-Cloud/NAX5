#pragma once

#include <QString>

enum Nax5AuthError
{
    Nax5AuthErrorNone = 0,
    Nax5AuthErrorInvalidCredentials,
    Nax5AuthErrorEmailNotVerified,
    Nax5AuthErrorRateLimited,
    Nax5AuthErrorNetworkError,
    Nax5AuthErrorServerError,
    Nax5AuthErrorInvalidResponse
};

QString nax5AuthErrorMessage(Nax5AuthError error, const QString &detail = QString());
// Short Russian reason for a request that got no HTTP answer; `qt_network_error` is QNetworkReply::NetworkError.
QString nax5NetworkFailureDetail(int qt_network_error);
