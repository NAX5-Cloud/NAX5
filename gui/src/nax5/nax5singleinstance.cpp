#include "nax5/nax5singleinstance.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QWindow>

namespace {
const char kRaise[] = "raise\n";

void raiseWindows()
{
    for (QWindow *window : QGuiApplication::topLevelWindows())
    {
        if (!window->isVisible())
            continue;
        if (window->windowState() & Qt::WindowMinimized)
            window->showNormal();
        window->raise();
        window->requestActivate();
    }
}
}

QString nax5SingleInstanceName()
{
    const QByteArray user = qgetenv("USERNAME") + '|' + qgetenv("USERDOMAIN");
    return QStringLiteral("NAX5-client-") + QString::fromLatin1(
        QCryptographicHash::hash(user, QCryptographicHash::Sha256).toHex().left(16));
}

bool nax5AcquireSingleInstance(const QString &name)
{
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(500))
        {
            probe.write(kRaise);
            probe.waitForBytesWritten(500);
            probe.disconnectFromServer();
            return false;
        }
    }
    // Nobody answered: a server name left by a crashed client is stale.
    QLocalServer::removeServer(name);
    auto *server = new QLocalServer(qApp);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name))
        return true;  // cannot guard (e.g. no permission): never block the player
    QObject::connect(server, &QLocalServer::newConnection, server, [server]() {
        while (QLocalSocket *socket = server->nextPendingConnection())
        {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() {
                if (socket->readAll().contains("raise"))
                    raiseWindows();
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    return true;
}
