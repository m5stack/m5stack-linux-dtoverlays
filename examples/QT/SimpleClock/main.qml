import QtQuick 2.15
import QtQuick.Window 2.15
import "content" as Content

Window {
    width: 240
    height: 320
    visible: true
    color: "#ededed"
    title: qsTr("CM4 Demo")

    Content.Clock { x: 20; y: 94; city: ""; shift: 8 }

    Text {
        id: text_Clock
        x: 73
        y: 291
        width: 95
        height: 27
        color: "#bababa"
        text: "12:34:56"
        font.pixelSize: 20
        horizontalAlignment: Text.AlignHCenter
    }

    property string current_time: qsTr("asdasd")
    property int hours
    property int minutes
    property int seconds
    property real shift
    property bool night: false
    property bool internationalTime: true //Unset for local time

    Text {
        id: text_Label
        x: 16
        y: 3
        width: 208
        height: 47
        color: "#8e8e8e"
        text: "CM4STACK"
        font.pixelSize: 40
        horizontalAlignment: Text.AlignHCenter
        font.underline: true
        font.bold: false
    }

    Text {
        id: text_Label1
        x: 68
        y: 50
        width: 104
        height: 28
        color: "#8e8e8e"
        text: "QT DEMO"
        font.pixelSize: 20
        horizontalAlignment: Text.AlignHCenter
        font.bold: false
    }
}
