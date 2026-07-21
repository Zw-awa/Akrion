import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Akrion 1.0

ApplicationWindow {
    id: window
    visible: true
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 680
    title: "Akrion"
    color: "#f3f4f6"

    function toast(message) {
        toastText.text = message
        toastPopup.open()
    }

    function startRecording() {
        appController.startRecording(
            runName.text,
            deviceId.text,
            algorithmName.text,
            parameters.text,
            Number(algorithmPeriod.text),
            Number(emitPeriod.text))
    }

    Connections {
        target: appController
        function onErrorOccurred(message) { window.toast(message) }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: "#ffffff"
            border.color: "#d8dee4"

            ScrollView {
                id: sourceScroll
                anchors.fill: parent
                clip: true

                ColumnLayout {
                    width: Math.max(0, sourceScroll.availableWidth - 24)
                    x: 12
                    spacing: 8

                    Label {
                        text: "数据源"
                        font.bold: true
                        font.pixelSize: 13
                        topPadding: 14
                    }
                    TabBar {
                        id: sourceMode
                        Layout.fillWidth: true
                        currentIndex: 1
                        enabled: !appController.connected
                        TabButton { text: "串口" }
                        TabButton { text: "内置演示" }
                    }
                    StackLayout {
                        Layout.fillWidth: true
                        currentIndex: sourceMode.currentIndex

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            RowLayout {
                                Layout.fillWidth: true
                                ComboBox {
                                    id: portSelector
                                    Layout.fillWidth: true
                                    model: appController.ports
                                    enabled: !appController.connected
                                }
                                ToolButton {
                                    text: "刷新"
                                    enabled: !appController.connected
                                    onClicked: appController.refreshPorts()
                                }
                            }
                            ComboBox {
                                id: baudSelector
                                Layout.fillWidth: true
                                model: ["115200", "230400", "460800", "921600"]
                            }
                            Button {
                                Layout.fillWidth: true
                                text: appController.connected ? "断开串口" : "连接串口"
                                enabled: appController.connected || portSelector.currentText.length > 0
                                onClicked: appController.connected
                                           ? appController.disconnectSource()
                                           : appController.connectSerial(portSelector.currentText,
                                                                         Number(baudSelector.currentText))
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 8
                                rowSpacing: 6
                                enabled: !appController.connected

                                Label { text: "输入"; color: "#57606a"; font.pixelSize: 11 }
                                ComboBox {
                                    id: inputSelector
                                    Layout.fillWidth: true
                                    model: appController.inputComponents
                                    textRole: "label"
                                    valueRole: "id"
                                }
                                Label { text: "噪声"; color: "#57606a"; font.pixelSize: 11 }
                                ComboBox {
                                    id: noiseSelector
                                    Layout.fillWidth: true
                                    model: appController.noiseComponents
                                    textRole: "label"
                                    valueRole: "id"
                                }
                                Label { text: "算法"; color: "#57606a"; font.pixelSize: 11 }
                                ComboBox {
                                    id: algorithmSelector
                                    Layout.fillWidth: true
                                    model: appController.algorithmComponents
                                    textRole: "label"
                                    valueRole: "id"
                                }
                            }
                            Button {
                                Layout.fillWidth: true
                                text: appController.demo ? "停止演示" : "运行演示"
                                onClicked: appController.demo
                                           ? appController.disconnectSource()
                                           : appController.startDemo(inputSelector.currentValue,
                                                                     noiseSelector.currentValue,
                                                                     algorithmSelector.currentValue)
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#d8dee4"; Layout.topMargin: 8 }
                    Label { text: "运行记录"; font.bold: true; font.pixelSize: 13; Layout.topMargin: 4 }
                    TextField {
                        id: runName
                        Layout.fillWidth: true
                        placeholderText: "运行名称"
                        text: appController.demo ? "demo-" + inputSelector.currentValue + "-" + noiseSelector.currentValue : "device-run"
                    }
                    TextField {
                        id: deviceId
                        Layout.fillWidth: true
                        placeholderText: "设备 ID"
                        text: appController.demo ? "demo-device" : "device-01"
                    }
                    TextField {
                        id: algorithmName
                        Layout.fillWidth: true
                        placeholderText: "算法名称"
                        text: appController.demo ? algorithmSelector.currentText : "PID"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: algorithmPeriod
                            Layout.fillWidth: true
                            placeholderText: "算法周期 us"
                            text: "1000"
                            validator: IntValidator { bottom: 1 }
                        }
                        TextField {
                            id: emitPeriod
                            Layout.fillWidth: true
                            placeholderText: "发送周期 us"
                            text: "10000"
                            validator: IntValidator { bottom: 1 }
                        }
                    }
                    TextArea {
                        id: parameters
                        Layout.fillWidth: true
                        Layout.preferredHeight: 76
                        placeholderText: "算法参数 JSON"
                        text: !appController.demo ? '{}'
                              : algorithmSelector.currentValue === "p_control" ? '{"kp":1.2}'
                              : algorithmSelector.currentValue === "pid" ? '{"kp":1.2,"ki":0.08,"kd":0.02}'
                              : algorithmSelector.currentValue === "median_filter" ? '{"window":5}'
                              : '{}'
                        wrapMode: TextEdit.WrapAnywhere
                        font.family: "Consolas"
                        font.pixelSize: 10
                        background: Rectangle { color: "#ffffff"; border.color: "#afb8c1"; radius: 3 }
                    }
                    Button {
                        Layout.fillWidth: true
                        text: appController.recording ? "停止并保存" : "开始录制"
                        enabled: appController.connected
                        highlighted: appController.recording
                        onClicked: appController.recording ? appController.stopRecording() : window.startRecording()
                    }

                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#d8dee4"; Layout.topMargin: 8 }
                    Label { text: "采集状态"; font.bold: true; font.pixelSize: 13; Layout.topMargin: 4 }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 5
                        Label { text: "速率"; color: "#68707c"; font.pixelSize: 11 }
                        Label { text: Number(appController.frameRate).toFixed(1) + " Hz"; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.family: "Consolas"; font.pixelSize: 11 }
                        Label { text: "已接收"; color: "#68707c"; font.pixelSize: 11 }
                        Label { text: appController.receivedFrames; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.family: "Consolas"; font.pixelSize: 11 }
                        Label { text: "解析错误"; color: "#68707c"; font.pixelSize: 11 }
                        Label { text: appController.parseErrors; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.family: "Consolas"; font.pixelSize: 11 }
                        Label { text: "显示缓存"; color: "#68707c"; font.pixelSize: 11 }
                        Label { text: appController.waveform.bufferedFrames; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.family: "Consolas"; font.pixelSize: 11 }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 34
                color: "#f6f8fa"
                border.color: "#d8dee4"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    Label {
                        text: appController.statusMessage
                        color: "#57606a"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    ToolButton { text: "清空"; onClicked: appController.clearWaveform() }
                }
            }
            WaveformPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                controller: appController.waveform
            }
        }

        Rectangle {
            Layout.preferredWidth: 292
            Layout.fillHeight: true
            color: "#ffffff"
            border.color: "#d8dee4"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "运行历史"; font.bold: true; font.pixelSize: 13; Layout.fillWidth: true }
                    ToolButton { text: "刷新"; onClicked: appController.refreshHistory() }
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    ColumnLayout {
                        width: 258
                        spacing: 0
                        Repeater {
                            model: appController.history
                            delegate: Item {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 92
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.topMargin: 8
                                    anchors.bottomMargin: 8
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: modelData.name || modelData.run_id; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: 11 }
                                        Label { text: modelData.frames + " 帧"; color: "#68707c"; font.pixelSize: 9 }
                                    }
                                    Label { text: modelData.run_id; color: "#68707c"; elide: Text.ElideMiddle; Layout.fillWidth: true; font.family: "Consolas"; font.pixelSize: 9 }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: modelData.created_at; color: "#68707c"; Layout.fillWidth: true; font.pixelSize: 9 }
                                        Button { text: "回放"; onClicked: appController.replayRun(modelData.run_id) }
                                    }
                                }
                                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: "#d8dee4" }
                            }
                        }
                        Label {
                            visible: appController.history.length === 0
                            text: "暂无运行记录"
                            color: "#68707c"
                            Layout.alignment: Qt.AlignHCenter
                            topPadding: 20
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: toastPopup
        x: (window.width - width) / 2
        y: window.height - height - 24
        padding: 10
        modal: false
        closePolicy: Popup.NoAutoClose
        Timer { interval: 2800; running: toastPopup.visible; onTriggered: toastPopup.close() }
        background: Rectangle { color: "#252b33"; radius: 4 }
        contentItem: Label {
            id: toastText
            color: "#ffffff"
            font.pixelSize: 11
            wrapMode: Text.Wrap
            maximumLineCount: 3
        }
    }
}
