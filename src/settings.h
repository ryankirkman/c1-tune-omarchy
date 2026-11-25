// What the user picked, and how it maps to camera commands. Ported from C1 Tune for macOS
// (Sources/CameraSettings.swift): same keys, ranges, defaults, zoom steps and commands.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "depthai.h"

/// Optional image values mean "leave the tuning's default".
/// Defaults were tuned on a live picture under warm (3000 K) bounced key light: a touch
/// darker than the auto-exposure target so skin and a white wall don't wash out, a little
/// more color, and slightly crisper than the tuning's own sharpness (≈2).
struct CameraSettings {
    bool autoExposure = true;
    int exposureUs = 16667;           // manual shutter, 100 µs–33 ms; 1/60 s avoids banding under 60 Hz light
    int iso = 400;                    // manual ISO, 100–1600
    int exposureCompensation = -1;    // auto exposure bias, -9…9

    bool autoFocus = true;
    int lensPosition = 130;           // manual focus, 0 (far)…255 (near)

    bool autoWhiteBalance = true;
    int whiteBalanceK = 4500;         // manual, 1000–12000 (the firmware's scale)

    int brightness = 0;               // -10…10
    int contrast = 0;                 // -10…10
    int saturation = 1;               // -10…10
    std::optional<int> sharpness = 3; // 0…4
    std::optional<int> noiseReduction;      // luma denoise 0…4
    std::optional<int> colorNoiseReduction; // chroma denoise 0…4

    int antiFlicker = 3;              // 0 off, 1 50 Hz, 2 60 Hz, 3 auto

    /// The Opal C1's sensor is mounted upside down, so the picture is normally rotated 180°
    /// (as Opal does). `flipped` turns that off, e.g. for a camera mounted upside down.
    bool flipped = false;
    int32_t orientation() const { return flipped ? 0 : 3; }   // 0 NORMAL, 3 ROTATE_180_DEG

    /// The camera's built-in microphone. Off, the pipeline has no audio nodes, so the camera's
    /// USB mic (which the firmware always advertises) only sends silence.
    bool microphone = false;
    std::optional<Pipeline::Mic> pipelineMic() const { return microphone ? std::optional(Pipeline::Mic()) : std::nullopt; }

    /// Zoom in percent (100…200), one of `zoomStops`. The sensor always runs at 4K; the ISP
    /// scales it by n/d and a 1920x1080 window is taken from the result, so there's no upscaling.
    int zoom = 100;
    /// Where the zoomed window sits, in percent of the free range (0 = left/top, 100 = right/bottom).
    int frameX = 50;
    int frameY = 50;

    /// Linux only: a tuning file copied from a Mac (Opal Composer's Opal_v0.12.bin). Empty
    /// uses the firmware's own tuning. Not a picture setting, so Reset keeps it.
    std::string tuningFile;

    bool operator==(const CameraSettings &) const = default;

    struct ZoomStop { int percent; int32_t n, d; };
    /// Every zoom the camera can do without upscaling, each tested on hardware: ISP scale n/d of
    /// the 4K sensor image, zoom = 2·n/d. The scaler accepts numerators up to 16 (MAX_N); larger
    /// ones (17/30, 19/24, …) make the firmware silently disable scaling.
    static const std::array<ZoomStop, 20> zoomStops;
    static int nearestZoom(int percent);

    /// ColorCamera settings for this zoom and framing. 1× is the 4K image scaled by 1/2, which
    /// measured less noisy and more detailed than the sensor's own 1080p mode.
    Pipeline::Camera pipelineCamera() const;

    /// True when going from this to `next` needs the camera's pipeline restarted: zoom, framing,
    /// flip, the microphone and the tuning file are part of the pipeline, and returning
    /// sharpness or denoise to the tuning's default has no command.
    bool needsRestart(const CameraSettings &next) const;

    /// Applies `key=value` pairs (from `c1-tune --set`). Unknown keys and unparseable values
    /// are ignored, numbers are held to the same ranges as the panel's sliders, and "default"
    /// clears sharpness or a denoise value.
    CameraSettings applying(const std::vector<std::pair<std::string, std::string>> &pairs) const;

    // Persistence: a flat JSON object. Saved values go over the defaults, so settings added in
    // later versions keep their defaults.
    std::string json() const;
    static CameraSettings fromJson(const std::string &text);
    static std::string path();
    static CameraSettings load();
    bool save() const;

    // Camera commands
    enum Group { exposure, focus, whiteBalance, image, antiFlickerGroup, groupCount };

    /// One control message setting everything; used as the pipeline's initial control.
    CameraControl fullControl() const;
    /// A control message for just one group of settings.
    CameraControl control(Group group) const;

private:
    void add(Group group, CameraControl &c) const;
};
