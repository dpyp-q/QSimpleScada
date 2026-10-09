// QSimpleScada 自定义 QML 图元：带标签、数值、单位与阈值变色的指示灯。
// 库通过 QScadaObjectQML::updateValue 调用本组件的 update(value) 函数；
// metaData 声明了可从 C++ 侧配置的属性名（QScadaObjectQML::QMLProperties 读取）。
import QtQuick 2.12

Item {
    property var metaData: ["caption", "unit", "threshold"]

    property string caption: "TAG"
    property string unit: ""
    property double threshold: 100.0
    property real currentValue: 0.0

    function update(v) {
        currentValue = Number(v);
    }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "transparent"
        border.color: "#9AA7B5"
        border.width: 1
    }

    Rectangle {
        id: lamp
        width: Math.max(10, parent.height * 0.5)
        height: width
        anchors.left: parent.left
        anchors.leftMargin: parent.width * 0.07
        anchors.verticalCenter: parent.verticalCenter
        radius: width / 2
        color: currentValue > threshold ? "#D64545" : "#2E9E44"
        border.color: "#333333"
        border.width: 2
    }

    Text {
        id: captionText
        text: caption
        anchors.top: parent.top
        anchors.topMargin: parent.height * 0.08
        anchors.left: lamp.right
        anchors.leftMargin: parent.width * 0.05
        font.pixelSize: Math.max(10, parent.height * 0.2)
        font.bold: true
        color: "#3A4A5A"
    }

    Text {
        text: currentValue.toFixed(1) + " " + unit
        anchors.bottom: parent.bottom
        anchors.bottomMargin: parent.height * 0.1
        anchors.left: lamp.right
        anchors.leftMargin: parent.width * 0.05
        font.pixelSize: Math.max(12, parent.height * 0.26)
        font.bold: true
        color: currentValue > threshold ? "#D64545" : "#1F3A5F"
    }
}
