import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Rectangle {
    id: playPanel
    height: column.implicitHeight + 24
    color: "#1a1a1a"
    Material.theme: Material.Dark
    Material.accent: "#00a7ff"

    // Back from the browser after a top-up: show the new balance without a restart.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive && Nax5Auth.authenticated && Nax5Auth.billingEnforced)
                Nax5Auth.refreshAccount();
        }
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("Версия launcher: %1").arg(Qt.application.version)
            color: "#9e9e9e"
            font.pixelSize: 12
        }

        Label {
            visible: Nax5Auth.authenticated && (!Nax5Auth.emailVerified || Nax5Auth.accessStatus !== "ACTIVE")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#ffcc80"
            text: !Nax5Auth.emailVerified ? qsTr("Подтвердите email") : qsTr("Аккаунт ещё не активен")
        }

        RowLayout {
            visible: Nax5Auth.authenticated && Nax5Auth.billingEnforced
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: Nax5Session.remainingText.length > 0
                      ? qsTr("Осталось в этой игре: %1").arg(Nax5Session.remainingText)
                      : qsTr("Игровое время: %1").arg(Nax5Auth.balanceText.length > 0 ? Nax5Auth.balanceText : "—")
                color: Nax5Session.topUpSuggested ? "#ffcc80" : "#eeeeee"
                font.pixelSize: 16
                font.bold: true
            }

            Button {
                text: qsTr("Пополнить")
                flat: !Nax5Session.topUpSuggested
                Material.background: Nax5Session.topUpSuggested ? Material.accent : undefined
                Material.roundedScale: Material.SmallScale
                onClicked: Qt.openUrlExternally(Nax5Session.topUpUrl)
            }

            Item { Layout.fillWidth: true }
        }

        Label {
            visible: Nax5Session.statusText.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: Nax5Session.statusText
            color: Nax5Session.reserved ? "#b9f6ca" : "#eeeeee"
            font.pixelSize: 18
        }

        Label {
            visible: Nax5Session.retrySeconds > 0
            Layout.fillWidth: true
            color: "#c5c5c5"
            text: qsTr("Повторить можно через %1 с").arg(Nax5Session.retrySeconds)
        }

        Label {
            visible: Nax5Session.reserved && Nax5Session.consoleCode.length > 0
            text: Nax5Session.consoleRegion.length > 0
                  ? qsTr("%1 · %2").arg(Nax5Session.consoleCode).arg(Nax5Session.consoleRegion)
                  : Nax5Session.consoleCode
            color: "#c5c5c5"
        }

        Button {
            visible: Nax5Session.updateRequired
            enabled: Nax5Session.updateUrl.length > 0
            text: qsTr("Скачать обязательное обновление")
            Material.background: Material.accent
            onClicked: Qt.openUrlExternally(Nax5Session.updateUrl)
        }

        Label {
            visible: Nax5Session.updateRequired
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#ff5252"
            font.bold: true
            font.pixelSize: 16
            text: qsTr("Требуется обновление launcher. Консоль может быть свободна, но эта версия больше не допускается к игре. Нажмите кнопку выше, установите новую версию и перезапустите NAX5.")
        }

        Label {
            visible: !Nax5Session.reserved && Nax5Session.errorMessage.length > 0 && Nax5Session.statusText !== Nax5Session.errorMessage
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#ff8a80"
            text: Nax5Session.errorMessage
        }

        RowLayout {
            spacing: 12

            Button {
                text: Nax5Session.reserving ? qsTr("Ищем...") : (Nax5Session.state === 3 || Nax5Session.state === 4 ? qsTr("Игра идёт") : qsTr("Играть"))
                enabled: Nax5Session.playEnabled
                Material.background: Material.accent
                Material.roundedScale: Material.SmallScale
                onClicked: Nax5Session.play()
            }

            Button {
                text: Nax5Session.cancelling ? qsTr("Освобождаем...") : qsTr("Освободить")
                visible: Nax5Session.reserved || Nax5Session.cancelling
                enabled: Nax5Session.releaseEnabled
                flat: true
                Material.roundedScale: Material.SmallScale
                onClicked: Nax5Session.release()
            }
        }
    }
}
