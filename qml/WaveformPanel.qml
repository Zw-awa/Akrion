import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Akrion 1.0

Item {
    id: root

    required property var controller
    property var activeGroups: controller ? controller.groupInfo.filter(function(group) {
        return group.visibleChannels > 0
    }) : []

    function timeText(timeUs) {
        if (!controller) return "--"
        if (controller.timeAxis === "host") return Qt.formatTime(new Date(Number(timeUs) / 1000), "HH:mm:ss.zzz")
        return (Number(timeUs) / 1000000).toFixed(3) + " s"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Label {
                    text: "波形"
                    font.bold: true
                    font.pixelSize: 14
                }

                Label {
                    text: controller.paused ? "画面已暂停" : (controller.follow ? "实时跟随" : "浏览历史")
                    color: "#68707c"
                    font.pixelSize: 11
                }

                Item { Layout.fillWidth: true }

                ButtonGroup { id: axisGroup }
                ToolButton {
                    text: "设备时间"
                    checkable: true
                    checked: controller.timeAxis === "device"
                    ButtonGroup.group: axisGroup
                    onClicked: controller.timeAxis = "device"
                    ToolTip.visible: hovered
                    ToolTip.text: "使用设备上报时间"
                }
                ToolButton {
                    text: "主机时间"
                    checkable: true
                    checked: controller.timeAxis === "host"
                    ButtonGroup.group: axisGroup
                    onClicked: controller.timeAxis = "host"
                    ToolTip.visible: hovered
                    ToolTip.text: "使用主机接收时间"
                }

                ComboBox {
                    id: windowSelector
                    model: ["1 s", "5 s", "10 s", "30 s", "60 s"]
                    currentIndex: 2
                    implicitWidth: 82
                    onActivated: controller.windowSeconds = Number(currentText.split(" ")[0])
                }

                ToolButton {
                    text: controller.paused ? "继续" : "暂停"
                    checkable: true
                    checked: controller.paused
                    onClicked: controller.paused = checked
                }
                ToolButton {
                    text: "跟随"
                    enabled: !controller.follow || controller.paused
                    onClicked: controller.follow = true
                    ToolTip.visible: hovered
                    ToolTip.text: "回到最新数据；也可双击波形"
                }
                ToolButton {
                    text: channelPane.visible ? "隐藏通道" : "通道"
                    onClicked: channelPane.visible = !channelPane.visible
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Item {
                id: plotHost
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: "#ffffff"
                }

                WaveformView {
                    id: waveformView
                    anchors.fill: parent
                    controller: root.controller
                }

                Repeater {
                    model: root.activeGroups
                    delegate: Item {
                        required property int index
                        required property var modelData

                        x: 4
                        y: waveformView.topInset + index * groupSlotHeight
                        width: waveformView.leftInset - 8
                        height: groupSlotHeight - 8
                        property real groupSlotHeight: (waveformView.height - waveformView.topInset - waveformView.bottomInset + 8)
                                                       / Math.max(1, root.activeGroups.length)

                        Label {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.unit ? modelData.label + "\n[" + modelData.unit + "]" : modelData.label
                            horizontalAlignment: Text.AlignRight
                            wrapMode: Text.Wrap
                            color: "#57606a"
                            font.pixelSize: 10
                        }
                    }
                }

                Repeater {
                    model: 7
                    delegate: Label {
                        required property int index
                        x: waveformView.leftInset + (waveformView.width - waveformView.leftInset - waveformView.rightInset) * index / 6 - width / 2
                        y: waveformView.height - waveformView.bottomInset + 4
                        text: root.timeText(controller.viewStartUs + (controller.viewEndUs - controller.viewStartUs) * index / 6)
                        color: "#68707c"
                        font.pixelSize: 9
                    }
                }

                Rectangle {
                    id: cursorPopup
                    visible: controller.cursorVisible && controller.cursorValues.length > 0
                    x: Math.max(8, Math.min(plotHost.width - width - 8,
                                           waveformView.leftInset
                                           + controller.cursorPosition * (waveformView.width - waveformView.leftInset - waveformView.rightInset)
                                           + 10))
                    y: 16
                    width: 224
                    height: cursorColumn.implicitHeight + 16
                    color: "#ffffff"
                    border.color: "#afb8c1"
                    radius: 4

                    ColumnLayout {
                        id: cursorColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: controller.cursorTimeLabel
                            elide: Text.ElideRight
                            color: "#57606a"
                            font.pixelSize: 9
                        }
                        Repeater {
                            model: controller.cursorValues
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 6

                                Rectangle {
                                    implicitWidth: 8
                                    implicitHeight: 8
                                    color: modelData.color
                                }
                                Label {
                                    text: modelData.label
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    font.pixelSize: 10
                                }
                                Label {
                                    text: Number(modelData.value).toPrecision(7) + (modelData.unit ? " " + modelData.unit : "")
                                    font.family: "Consolas"
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }
                }

                Column {
                    anchors.centerIn: parent
                    visible: controller.totalFrames === 0
                    spacing: 4

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "等待数据"
                        font.bold: true
                        color: "#3d4651"
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "连接数据源或载入一次运行"
                        color: "#68707c"
                        font.pixelSize: 11
                    }
                }
            }

            Pane {
                id: channelPane
                Layout.preferredWidth: 250
                Layout.fillHeight: true
                padding: 0
                visible: true
                background: Rectangle {
                    color: "#f7f8fa"
                    border.color: "#dfe3e8"
                }

                ScrollView {
                    anchors.fill: parent
                    clip: true

                    ColumnLayout {
                        width: channelPane.width
                        spacing: 0

                        Label {
                            text: "通道"
                            font.bold: true
                            Layout.leftMargin: 12
                            Layout.topMargin: 12
                            Layout.bottomMargin: 6
                        }

                        Repeater {
                            model: controller.channelModel
                            delegate: Rectangle {
                                required property string key
                                required property string label
                                required property string unit
                                required property string groupKey
                                required property color channelColor
                                required property bool channelVisible

                                Layout.fillWidth: true
                                implicitHeight: 64
                                color: "transparent"

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 5

                                        Rectangle {
                                            implicitWidth: 9
                                            implicitHeight: 9
                                            color: channelColor
                                        }
                                        CheckBox {
                                            text: label + (unit ? " [" + unit + "]" : "")
                                            checked: channelVisible
                                            Layout.fillWidth: true
                                            onToggled: controller.setChannelVisible(key, checked)
                                        }
                                    }
                                    ComboBox {
                                        Layout.fillWidth: true
                                        implicitHeight: 26
                                        model: controller.groupKeys
                                        currentIndex: Math.max(0, controller.groupKeys.indexOf(groupKey))
                                        onActivated: controller.setChannelGroup(key, currentText)
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: "#dfe3e8"
                        }

                        Label {
                            text: "Y 轴"
                            font.bold: true
                            Layout.leftMargin: 12
                            Layout.topMargin: 10
                            Layout.bottomMargin: 4
                        }

                        Repeater {
                            model: controller.groupInfo
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                Layout.rightMargin: 10
                                Layout.bottomMargin: 8
                                spacing: 3

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: modelData.label
                                        font.pixelSize: 11
                                        Layout.fillWidth: true
                                    }
                                    CheckBox {
                                        text: "自动"
                                        checked: modelData.autoRange
                                        onToggled: controller.setGroupAutoRange(modelData.key, checked)
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    enabled: !modelData.autoRange

                                    TextField {
                                        id: minimumInput
                                        Layout.fillWidth: true
                                        text: String(modelData.minimum)
                                        placeholderText: "最小值"
                                        validator: DoubleValidator {}
                                    }
                                    TextField {
                                        id: maximumInput
                                        Layout.fillWidth: true
                                        text: String(modelData.maximum)
                                        placeholderText: "最大值"
                                        validator: DoubleValidator {}
                                    }
                                    ToolButton {
                                        text: "应用"
                                        onClicked: controller.setGroupRange(modelData.key,
                                                                            Number(minimumInput.text),
                                                                            Number(maximumInput.text))
                                    }
                                }
                            }
                        }

                        CheckBox {
                            text: "绘制算法未启用样本"
                            checked: controller.includeDisabledSamples
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            Layout.bottomMargin: 10
                            onToggled: controller.includeDisabledSamples = checked
                        }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#f7f8fa"
            border.color: "#dfe3e8"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12

                Label {
                    text: controller.follow ? "跟随" : "已离开实时位置"
                    color: controller.follow ? "#1a7f37" : "#9a6700"
                    font.pixelSize: 10
                }
                Label {
                    text: "滚轮缩放 · 拖拽平移 · 双击恢复跟随"
                    color: "#68707c"
                    font.pixelSize: 10
                    Layout.fillWidth: true
                }
                Label {
                    text: controller.totalFrames + " 帧 · 缓冲 " + controller.bufferedFrames + " · 缺口 " + controller.droppedFrames
                    color: "#68707c"
                    font.pixelSize: 10
                }
            }
        }
    }
}
