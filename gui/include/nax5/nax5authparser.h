#pragma once

#include "nax5/nax5autherror.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

struct Nax5LoginParseResult
{
    Nax5AuthError error;
    QString session_token;

    Nax5LoginParseResult()
        : error(Nax5AuthErrorInvalidResponse)
    {
    }
};

struct Nax5MeParseResult
{
    bool ok;
    bool network_failure;
    qint64 user_id;
    QString email;
    QString city;
    QString access_status;
    bool email_verified;
    qint64 balance_seconds;  // -1 when the server does not report play time
    bool billing_enforced;

    Nax5MeParseResult()
        : ok(false)
        , network_failure(false)
        , user_id(0)
        , email_verified(false)
        , balance_seconds(-1)
        , billing_enforced(false)
    {
    }
};

Nax5LoginParseResult nax5ParseLoginResponse(int http_status, const QByteArray &body);
Nax5AuthError nax5MapNetworkFailure(int http_status, bool timed_out, bool no_network);
Nax5MeParseResult nax5ParseMeResponse(int http_status, const QByteArray &body);
