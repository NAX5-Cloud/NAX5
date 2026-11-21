#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>

// CHIAKI_SESSION_AUTH_SIZE and ChiakiConnectInfo::morning are 0x10 in v1.10.0
// (lib/include/chiaki/session.h). Keep this in lockstep with that header.
enum {
    Nax5RegistKeySize = 0x10,
    Nax5MorningSize = 0x10,
    Nax5ConnectionContractVersion = 1,
    Nax5DirectPortsContractVersion = 2
};

struct Nax5ConnectionMaterial
{
    int version;
    int target;
    uint16_t session_control_port = 0;
    uint16_t stream_port = 0;
    uint16_t senkusha_port = 0;
    uint16_t discovery_port = 0;
    QString host;
    QString nickname;
    QByteArray regist_key;
    QByteArray morning;
    QString console_pin;
    QString session_id;
    QString session_status;

    Nax5ConnectionMaterial()
        : version(0)
        , target(0)
    {
    }

    void clear();
};

bool nax5ConnectionMaterialIsEmpty(const Nax5ConnectionMaterial &material);
