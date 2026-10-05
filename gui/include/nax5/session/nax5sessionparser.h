#pragma once

#include "nax5/session/nax5sessionerror.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

struct Nax5AssignedConsole
{
    QString code;
    QString region;
};

struct Nax5SessionInfo
{
    QString id;
    QString status;
    QString reserved_at;
    QString lease_expires_at;
};

struct Nax5SessionParseResult
{
    Nax5SessionError error;
    bool has_session;
    bool has_console;
    Nax5SessionInfo session;
    Nax5AssignedConsole console;
    QString minimum_version;
    QString update_url;
    qint64 remaining_seconds;  // play time left in the running session; -1 when not reported
    qint64 balance_seconds;    // sent with INSUFFICIENT_BALANCE; -1 otherwise
    QString reason;            // why the server ended or refused the session, when it says so
    qint64 retry_after_seconds; // the server's pause before the next reserve; -1 when not sent

    Nax5SessionParseResult()
        : error(Nax5SessionErrorInvalidResponse)
        , has_session(false)
        , has_console(false)
        , remaining_seconds(-1)
        , balance_seconds(-1)
        , retry_after_seconds(-1)
    {
    }
};

Nax5SessionError nax5MapSessionHttpError(int http_status, bool timed_out, bool no_network);
Nax5SessionParseResult nax5ParseReserveResponse(int http_status, const QByteArray &body);
Nax5SessionParseResult nax5ParseCurrentResponse(int http_status, const QByteArray &body);
Nax5SessionParseResult nax5ParseCancelResponse(int http_status, const QByteArray &body);
bool nax5SessionPayloadLooksLeaky(const QByteArray &body);
