import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// About the app: the Oviora mark and word mark with the version and how updates come, then who
// makes it (Arfep's word mark, light version, quiet) and the license note
SettingsCard {
    id: card
    property string updateText: ""

    title: qsTr("Hakkında")

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 6
        spacing: 18

        OvioraMark {
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            Label {
                textFormat: Text.PlainText
                text: "Oviora"
                font.family: Tema.display
                font.pixelSize: 24
                font.weight: Font.DemiBold
                font.letterSpacing: -0.5
                color: Tema.text
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Sürüm %1").arg(platform.installedVersion || Qt.application.version)
                wrapMode: Text.WordWrap
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: card.updateText.length > 0
                text: card.updateText
                wrapMode: Text.WordWrap
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
        }
    }

    // Support on Discord: people who use Oviora and the team answer there
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 20
        spacing: 14
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: qsTr("Bir sorun mu var? Discord sunucumuzda sor; Oviora kullananlar ve biz oradayız.")
            wrapMode: Text.WordWrap
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.muted
        }
        PillButton {
            objectName: "about-discord"
            compact: true
            text: qsTr("Discord'da destek al")
            onClicked: Qt.openUrlExternally("https://discord.gg/XRPXMCqCqe")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 22
        Layout.bottomMargin: 8
        spacing: 14
        Label {
            textFormat: Text.PlainText
            text: qsTr("Geliştirici")
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.faint
        }
        Image {
            Layout.preferredWidth: 64
            Layout.preferredHeight: 24
            source: "arfep-lettermark-light.svg"
            sourceSize: Qt.size(128, 48)
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: 0.7
        }
        Item { Layout.fillWidth: true }
    }
    Label {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        text: qsTr("Açık kaynaklı bir uzaktan oynatma projesi üzerine kurulmuştur. GNU Affero Genel Kamu Lisansı sürüm 3 ile dağıtılan özgür yazılımdır; hiçbir garanti verilmez.")
        wrapMode: Text.WordWrap
        font.family: Tema.body
        font.pixelSize: 13
        lineHeight: 1.25
        color: Tema.faint
    }
}
