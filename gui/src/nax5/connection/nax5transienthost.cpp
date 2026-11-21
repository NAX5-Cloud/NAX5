#include "nax5/connection/nax5transienthost.h"
#include "nax5/connection/nax5connectionparser.h"

#include "settings.h"
#include "streamsession.h"

bool nax5FillStreamSessionConnectInfo(
    Settings *settings,
    const Nax5ConnectionMaterial &material,
    StreamSessionConnectInfo *out)
{
    if (!settings || !out)
        return false;
    if (material.host.trimmed().isEmpty())
        return false;
    if (material.regist_key.size() != Nax5RegistKeySize)
        return false;
    if (material.morning.size() != Nax5MorningSize)
        return false;
    if (!nax5TargetIsSupportedDirectPs5(material.target))
        return false;

    bool fullscreen = false;
    bool zoom = false;
    bool stretch = false;
    switch (settings->GetWindowType()) {
    case WindowType::Fullscreen:
        fullscreen = true;
        break;
    case WindowType::Zoom:
        zoom = true;
        break;
    case WindowType::Stretch:
        stretch = true;
        break;
    default:
        break;
    }

    *out = StreamSessionConnectInfo(
        settings,
        static_cast<ChiakiTarget>(material.target),
        material.host,
        material.nickname,
        material.regist_key,
        material.morning,
        material.console_pin,
        QString(),
        false,
        fullscreen,
        zoom,
        stretch);
    out->direct_ports = {material.session_control_port, material.stream_port, material.senkusha_port};
    return true;
}
