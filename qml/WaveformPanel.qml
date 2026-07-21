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
    property var timeTickModel: {
        if (!controller) return []
        controller.viewStartUs
        controller.viewEndUs
        const plotWidth = Math.max(1, waveformView.width - waveformView.leftInset - waveformView.rightInset)
        const maximumTicks = Math.max(4, Math.min(10, Math.floor(plotWidth / 90) + 1))
        return controller.timeTicks(maximumTicks)
    }

    function timeText(timeUs) {
        if (!controller) return "--"
        if (controller.timeAxis === "host") return Qt.formatTime(new Date(Number(timeUs) / 1000), "HH:mm:ss.zzz")
        const spanSeconds = Math.abs(Number(controller.viewEndUs - controller.viewStartUs)) / 1000000
        const decimals = spanSeconds >= 20 ? 0
                       : spanSeconds >= 2 ? 1
                       : spanSeconds >= 0.2 ? 2
                       : spanSeconds >= 0.02 ? 3
                       : spanSeconds >= 0.002 ? 4 : 6
        return (Number(timeUs) / 1000000).toFixed(decimals) + " s"
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
                    ToolTip.text: "回到最新数据"
                }
                ToolButton {
                    text: "适应 Y"
                    enabled: root.activeGroups.length > 0
                    onClicked: {
                        for (let index = 0; index < root.activeGroups.length; ++index)
                            controller.resetGroupRange(root.activeGroups[index].key)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "按已有数据重置所有 Y 轴"
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
                        width: waveformView.leftInset - 10
                        height: groupSlotHeight - 8
                        property real groupSlotHeight: (waveformView.height - waveformView.topInset - waveformView.bottomInset + 8)
                                                       / Math.max(1, root.activeGroups.length)
                        property var tickModel: root.controller.valueTicks(Number(modelData.minimum),
                                                                           Number(modelData.maximum), 6)

                        Repeater {
                            model: parent.tickModel
                            delegate: Label {
                                required property var modelData
                                x: 0
                                y: Number(modelData.position) * parent.height - height / 2
                                width: parent.width
                                text: modelData.label
                                horizontalAlignment: Text.AlignRight
                                color: "#68707c"
                                font.family: "Consolas"
                                font.pixelSize: 9
                            }
                        }

                        Label {
                            x: parent.width + 12
                            y: 4
                            text: modelData.unit ? modelData.label + " [" + modelData.unit + "]" : modelData.label
                            color: "#68707c"
                            font.pixelSize: 9
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            cursorShape: Qt.SizeVerCursor
                            preventStealing: true
                            property real previousY: 0

                            onPressed: function(mouse) {
                                previousY = mouse.y
                            }
                            onPositionChanged: function(mouse) {
                                if (!pressed || height <= 0) return
                                const delta = mouse.y - previousY
                                previousY = mouse.y
                                if (delta !== 0)
                                    root.controller.panGroupBy(modelData.key, delta / height)
                            }
                            onWheel: function(wheel) {
                                const steps = wheel.angleDelta.y / 120
                                if (steps !== 0)
                                    root.controller.zoomGroupAt(modelData.key,
                                                                Math.max(0, Math.min(1, wheel.y / height)),
                                                                Math.pow(1.25, steps))
                                wheel.accepted = true
                            }
                            onDoubleClicked: function(mouse) {
                                root.controller.resetGroupRange(modelData.key)
                                mouse.accepted = true
                            }
                        }
                    }
                }

                Repeater {
                    model: root.timeTickModel
                    delegate: Label {
                        required property var modelData
                        x: Math.max(waveformView.leftInset,
                                    Math.min(waveformView.width - waveformView.rightInset - width,
                                             waveformView.leftInset
                                             + (waveformView.width - waveformView.leftInset - waveformView.rightInset)
                                             * Number(modelData.position) - width / 2))
                        y: waveformView.height - waveformView.bottomInset + 4
                        text: root.timeText(modelData.timeUs)
                        color: "#68707c"
                        font.family: "Consolas"
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
                                  + (controller.cursorSnapped ? " · " + controller.cursorSnapLabel : "")
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
                            text: "通道  " + controller.channelCount
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
                                required property var latestValue
                                required property bool hasValue

                                Layout.fillWidth: true
                                implicitHeight: 70
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
                                            id: colorSwatch
                                            implicitWidth: 16
                                            implicitHeight: 16
                                            color: channelColor
                                            border.color: "#8c959f"

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: colorPopup.open()
                                            }
                                            Popup {
                                                id: colorPopup
                                                y: colorSwatch.height + 4
                                                padding: 7
                                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                                background: Rectangle {
                                                    color: "#ffffff"
                                                    border.color: "#afb8c1"
                                                    radius: 4
                                                }
                                                ColumnLayout {
                                                    spacing: 6
                                                    Grid {
                                                        columns: 4
                                                        spacing: 6
                                                        Repeater {
                                                            model: ["#0969da", "#cf222e", "#1a7f37", "#8250df",
                                                                    "#bf8700", "#00838f", "#d15704", "#57606a",
                                                                    "#e83e8c", "#24292f", "#00a6a6", "#6f42c1"]
                                                            delegate: Rectangle {
                                                                required property string modelData
                                                                width: 20
                                                                height: 20
                                                                color: modelData
                                                                border.width: channelColor.toString() === modelData ? 2 : 1
                                                                border.color: channelColor.toString() === modelData ? "#24292f" : "#d0d7de"
                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    cursorShape: Qt.PointingHandCursor
                                                                    onClicked: {
                                                                        controller.setChannelColor(key, modelData)
                                                                        colorPopup.close()
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                    RowLayout {
                                                        TextField {
                                                            id: customColor
                                                            implicitWidth: 78
                                                            implicitHeight: 25
                                                            text: channelColor.toString()
                                                            selectByMouse: true
                                                        }
                                                        Button {
                                                            text: "应用"
                                                            implicitHeight: 25
                                                            onClicked: {
                                                                controller.setChannelColor(key, customColor.text)
                                                                colorPopup.close()
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        CheckBox {
                                            text: ""
                                            checked: channelVisible
                                            onToggled: controller.setChannelVisible(key, checked)
                                        }
                                        TextField {
                                            Layout.fillWidth: true
                                            implicitHeight: 26
                                            text: label
                                            selectByMouse: true
                                            onEditingFinished: controller.setChannelLabel(key, text)
                                        }
                                        Label {
                                            text: hasValue
                                                  ? Number(latestValue).toPrecision(7) + (unit ? " " + unit : "")
                                                  : "--"
                                            horizontalAlignment: Text.AlignRight
                                            font.family: "Consolas"
                                            font.pixelSize: 10
                                            Layout.minimumWidth: 72
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
                                        text: "自动扩展"
                                        checked: modelData.autoRange
                                        onToggled: controller.setGroupAutoRange(modelData.key, checked)
                                    }
                                    ToolButton {
                                        text: "适应"
                                        onClicked: controller.resetGroupRange(modelData.key)
                                        ToolTip.visible: hovered
                                        ToolTip.text: "按已有数据重置此 Y 轴"
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
