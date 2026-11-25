import QtQuick
import QtMultimedia

// A live view of the camera, read like any other app would: as a plain webcam. Loaded only
// while the preview is switched on, so the panel doesn't pull in Qt Multimedia otherwise.
Rectangle {
    id: preview
    color: "black"

    // The node is found by USB id, not by name, so another webcam is never shown by mistake.
    readonly property string node: backend.cameraNode()
    readonly property var device: {
        var inputs = devices.videoInputs
        for (var i = 0; i < inputs.length; i++)
            if (node !== "" && String(inputs[i].id) === node)
                return inputs[i]
        return null
    }

    MediaDevices { id: devices }

    Loader {
        anchors.fill: parent
        active: preview.device !== null
        sourceComponent: Item {
            CaptureSession {
                camera: Camera {
                    cameraDevice: preview.device
                    active: true
                }
                videoOutput: output
            }
            VideoOutput {
                id: output
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectCrop
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: preview.device === null
        text: qsTr("Camera not found")
        color: "#888888"
        font.pixelSize: 12
    }
}
