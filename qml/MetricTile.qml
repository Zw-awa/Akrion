import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    property string label: ""
    property string value: "--"
    implicitWidth: 105
    implicitHeight: 49
    color: "#f5f6f8"
    border.color: "#e8eaed"
    radius: 4
    ColumnLayout { anchors.fill: parent; anchors.margins: 8; spacing: 2; Label { text: label; color: "#68707c"; font.pixelSize: 10 } Label { text: value; font.family: "Consolas"; font.bold: true; font.pixelSize: 14 } }
}
