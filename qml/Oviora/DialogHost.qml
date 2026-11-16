// Dialog routing and scrim. Each dialog owns its own editable content.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: host
    // Compatibility type used by the professional and node views.
    component VTextField: DialogTextField {}
    anchors.fill: parent
    z: 100

    // Active dialog: ps5/subtitle/audio/save/manage.
    property string dialog: ""

    function ensureLoaded(key) {
        if (key === "ps5") ps5Loader.active = true
        else if (key === "subtitle") subtitleLoader.active = true
        else if (key === "audio") audioLoader.active = true
        else if (key === "save") saveLoader.active = true
        else if (key === "manage") manageLoader.active = true
    }
    onDialogChanged: if (dialog !== "") ensureLoaded(dialog)
    function open(key) {
        ensureLoaded(key)
        if (key === "save" && saveLoader.item) saveLoader.item.resetForOpen()
        if (key === "manage" && manageLoader.item) manageLoader.item.feedback = ""
        oviora.logUi("ui-dialog", "open " + key + " (was " + (host.dialog || "none") + ")")
        host.dialog = key
    }
    function close() { host.dialog = "" }

    signal startPs5()

    // Scrim (M16): the design dims the page behind the dialog, opacity .2s linear.
    // The native video window cannot be dimmed from QML; only the dialog panel is cut
    // out of it (videoCover below), so the picture beside the dialog stays undimmed.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)
        opacity: host.dialog !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(200) } }
        TapHandler { onTapped: host.close() }
    }
    // For the motion probe: the open dialog's scale.
    property real motionScale: 1

    // --- PS5 串流 ---------------------------------------------------------
    // The 1.4.4 Remote Play panel: saved pairings are encrypted in the user's
    // %LOCALAPPDATA%\Oviora\remoteplay and shared with 1.4.4.
    Loader {
        id: ps5Loader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            Ps5SourceDialog {
                id: ps5Dialog
                dialogHost: host
            }
        }
    }

    // --- 字幕设置 ---------------------------------------------------------
    Loader {
        id: subtitleLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            SubtitleSettingsDialog {
                id: subtitleDialog
                dialogHost: host
            }
        }
    }

    // --- 音频设置 ---------------------------------------------------------
    Loader {
        id: audioLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            AudioSettingsDialog {
                id: audioDialog
                dialogHost: host
            }
        }
    }

    // --- 另存为预设 -------------------------------------------------------
    // The design lists every part that will be saved before asking for a name, so
    // the user can see what they are about to store.
    Loader {
        id: saveLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            SavePresetDialog {
                id: saveDialog
                dialogHost: host
            }
        }
    }

    // --- 管理预设 ---------------------------------------------------------
    Loader {
        id: manageLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            ManagePresetsDialog {
                id: manageDialog
                dialogHost: host
            }
        }
    }

    // --- small building blocks the design's dialogs use --------------------

}
