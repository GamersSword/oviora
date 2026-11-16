import QtQuick
import QtQuick.Layouts

// Oviora: the game's frame rate for frame generation, chosen by hand. The PS5 always sends 60;
// a 30 fps game arrives as every picture twice, and "30 FPS" keeps one of each pair (the
// engine's 60->30). "60 FPS" keeps every picture.
VRow {
    id: row
    required property var backend
    property string namePrefix: "list"
    property bool showHelp: true
    readonly property bool thirty: backend && backend.contentRate !== undefined && Number(backend.contentRate) === 5
    label: qsTr("Oyunun kare hızı")
    hint: showHelp ? qsTr("PS5 her zaman 60 kare gönderir. Oyun 30 karede çalışıyorsa 30 FPS seç: kopya kareler atlanır ve kare üretimi gerçek karelerden çalışır.") : ""
    hintBelow: true
    VSeg {
        objectName: row.namePrefix + "-content-rate"
        options: [{ id: "5", label: "30 FPS" }, { id: "0", label: "60 FPS" }]
        current: row.thirty ? "5" : "0"
        onPicked: id => row.backend.contentRate = Number(id)
    }
}
