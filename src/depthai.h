// Wire formats for the DepthAI firmware Opal ships (device side 26f3d765, depthai-shared
// 12dc4c5): libnop for messages and node properties, msgpack for RPC, and a preboot
// header that carries the board config in front of the firmware image.
//
// Everything here is a byte-for-byte port of C1 Tune for macOS (Sources/DepthAI.swift);
// tests/ checks it against the same reference fixtures. The camera fails silently on a
// wrong byte, so change nothing here without a fixture that proves it.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

/// libnop encoder: a prefix byte then little-endian payload, always the smallest encoding.
struct Nop {
    Bytes bytes;

    void uint(uint64_t v);
    void sint(int64_t v);
    void boolean(bool v) { bytes.push_back(v ? 1 : 0); }
    void float32(float v);
    void structure(int members) { bytes.push_back(0xB9); bytes.push_back(uint8_t(members)); }
    void string(const std::string &s);
    void emptyMap() { bytes.push_back(0xBB); bytes.push_back(0x00); }
    void intMap(const std::vector<std::pair<int64_t, int64_t>> &m);
    void emptyArray() { bytes.push_back(0xBA); bytes.push_back(0x00); }
    void none() { bytes.push_back(0xBE); }   // empty tl::optional
    void append(const Bytes &b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
};

/// msgpack value, encoded the way nlohmann::json::to_msgpack does: object keys sorted,
/// floats as float32 when that's exact.
struct MsgPack {
    enum Kind { Null, Bool, Int, Float, String, Array, Object, Raw };
    Kind kind = Null;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<MsgPack> a;
    std::map<std::string, MsgPack> o;   // std::map orders keys bytewise, as nlohmann does
    Bytes raw;                          // pre-encoded bytes

    MsgPack() = default;
    static MsgPack null() { return {}; }
    static MsgPack boolean(bool v) { MsgPack m; m.kind = Bool; m.b = v; return m; }
    static MsgPack integer(int64_t v) { MsgPack m; m.kind = Int; m.i = v; return m; }
    static MsgPack real(double v) { MsgPack m; m.kind = Float; m.f = v; return m; }
    static MsgPack string(std::string v) { MsgPack m; m.kind = String; m.s = std::move(v); return m; }
    static MsgPack array(std::vector<MsgPack> v) { MsgPack m; m.kind = Array; m.a = std::move(v); return m; }
    static MsgPack object(std::map<std::string, MsgPack> v) { MsgPack m; m.kind = Object; m.o = std::move(v); return m; }
    static MsgPack rawBytes(Bytes v) { MsgPack m; m.kind = Raw; m.raw = std::move(v); return m; }

    Bytes encoded() const { Bytes out; encode(out); return out; }
    void encode(Bytes &out) const;
};

// MARK: - Camera control

/// DepthAI RawCameraControl. `encoded()` is the XLinkIn payload: libnop metadata, then
/// int32 datatype (5 = CameraControl in this firmware) and uint32 metadata size.
struct CameraControl {
    enum Command {
        startStream = 1, stopStream = 2, moveLens = 4, afTrigger = 5, aeManual = 6, aeAuto = 7,
        awbMode_ = 8, sceneMode = 9, antiBanding = 10, expCompensation_ = 11, aeLock = 13, awbLock = 16,
        effect = 24, afMode = 26, lumaDenoiseOld = 27, saturation_ = 28, brightness_ = 31, sharpness_ = 35,
        contrast_ = 44, aeRegion = 45, afRegion = 46, lumaDenoise_ = 47, chromaDenoise_ = 48, wbColorTemp_ = 49,
        afLensRange = 51
    };
    enum AutoFocus : uint8_t { afOff = 0, afAuto = 1, afMacro = 2, afContinuousVideo = 3, afContinuousPicture = 4 };
    enum WhiteBalance : uint8_t { wbOff = 0, wbAuto = 1 };
    enum AntiBanding : uint8_t { abOff = 0, ab50 = 1, ab60 = 2, abAuto = 3 };

    uint64_t cmdMask = 0;
    uint8_t autoFocusMode = afContinuousVideo;
    uint8_t lensPosition = 0;
    uint8_t lensPosAutoInfinity = 0;
    uint8_t lensPosAutoMacro = 0;
    uint8_t lensPosLimitMin = 0;
    uint8_t lensPosLimitMax = 255;
    uint32_t exposureTimeUs = 0;
    uint32_t sensitivityIso = 0;
    uint32_t frameDurationUs = 0;
    uint8_t awbMode = wbOff;
    uint8_t antiBandingMode = abOff;
    int8_t expCompensation = 0;
    int8_t brightness = 0;
    int8_t contrast = 0;
    int8_t saturation = 0;
    uint8_t sharpness = 0;
    uint8_t lumaDenoise = 0;
    uint8_t chromaDenoise = 0;
    uint16_t wbColorTemp = 0;

    void set(Command c) { cmdMask |= uint64_t(1) << c; }
    bool has(Command c) const { return cmdMask & (uint64_t(1) << c); }

    /// Adds another message's commands (and their values) to this one.
    void absorb(const CameraControl &o);

    /// libnop RawCameraControl, 29 members in the firmware's order.
    Bytes nop() const;
    Bytes encoded() const;

    bool operator==(const CameraControl &) const = default;
};

// MARK: - Boot image

namespace BootImage {
/// libnop BoardConfig: default USB ids, UVC on, and the watchdog timeout (nullopt keeps the
/// firmware's default, which is what the app boots with; 0 turns it off, but then the
/// firmware resets as soon as video starts).
Bytes boardConfig(std::optional<uint32_t> watchdogMs, bool uvc);

/// Makes the webcam advertise only 1920x1080. The firmware picks its output size from the
/// frame *index* the host requests (1 = 4K, 2 = 1440p, 3 = 1080p, 4 = 720p) and only streams
/// when that matches the pipeline, so every frame descriptor is rewritten to describe the
/// 1080p entry (same index, size and buffer). Any app then gets 1080p. Patches each copy
/// (USB 2 and USB 3); returns how many entries changed.
int patchFrameSizes(Bytes &firmware);

/// Preboot header carrying `config`, followed by the firmware.
Bytes image(const Bytes &firmware, const Bytes &config);
}

// MARK: - Pipeline

namespace Pipeline {
/// ColorCamera and node settings. Defaults mirror what Opal Composer sets (read from its
/// binary): RGB socket, planar output, 180° orientation, and UVC GPIOs 58/37/34 (the status
/// light) driven high while streaming. The app overrides `resolution` and `ispScale` to run
/// the sensor in 4K and scale it down (see CameraSettings::pipelineCamera); the output is
/// always the 1080p the webcam advertises (see patchFrameSizes).
struct Camera {
    CameraControl initialControl;
    int32_t resolution = 0;            // THE_1080_P
    float fps = 30;
    int32_t videoWidth = 1920;
    int32_t videoHeight = 1080;
    int32_t ispScale[4] = {0, 0, 0, 0};   // horiz num/den, vert num/den (0 = off)
    float sensorCropX = -1;            // top-left of the video window, 0…1 of the free range (-1 = centered)
    float sensorCropY = -1;
    int32_t boardSocket = 0;           // RGB
    bool interleaved = false;
    int32_t orientation = 3;           // ROTATE_180_DEG: the sensor is mounted upside down (0 = NORMAL)
    bool uvcGpios = true;
    uint32_t controlMaxDataSize = 1024;
    int64_t xlinkChunkSize = 0;

    /// depthai's untouched defaults (only used to compare against reference bytes).
    static Camera depthaiDefaults();
};

/// AudioMic settings. Defaults are what Opal Composer sets (read from its binary): front
/// mics, a fixed 30 dB gain with the firmware's experimental AGC off, and 4-byte samples
/// on the (unused) XLink output. Without an AudioMic feeding it, the USB mic sends silence.
struct Mic {
    float gainDecibels = 30;
    bool autoGain = false;
    bool backMics = false;
    bool xlinkApplyGain = true;
    uint8_t xlinkSampleSizeBytes = 4;
};

/// RPC request bytes for setPipelineSchema with XLinkIn("control") → ColorCamera → UVC,
/// plus AudioMic → UAC (the USB microphone) when `mic` is set.
Bytes setPipelineSchemaRequest(const Camera &camera, const std::optional<Mic> &mic,
                               const std::string &tuningBlobUri = "", std::optional<int64_t> tuningBlobSize = {});
MsgPack schemaJSON(const Camera &camera, const std::optional<Mic> &mic,
                   const std::string &tuningBlobUri, std::optional<int64_t> tuningBlobSize);
Bytes xlinkInProperties(const std::string &stream, uint32_t maxDataSize);
Bytes uvcProperties(bool gpios);
Bytes audioInConfig(float gain);
Bytes audioMicProperties(const Mic &m);
Bytes uacProperties();
Bytes colorCameraProperties(const Camera &c);
}

// MARK: - Assets

/// The pipeline's asset table. The only asset is the camera tuning blob, stored at offset 0.
namespace Assets {
inline const char *tuningUri = "asset:camTuning";
MsgPack tuning(int64_t size);
}

// MARK: - RPC

namespace RPC {
/// nanorpc request: [1, 1, hash(name), [args...]], or nil args for no arguments.
Bytes request(const std::string &name, const std::vector<MsgPack> &args = {});
uint64_t hash(const std::string &name);
/// True for `[1, 2, 1, ...]` (status good).
bool succeeded(const Bytes &reply);
/// The unsigned integer a reply ends with (msgpack uint of any width).
std::optional<uint64_t> trailingUInt(const Bytes &reply);
}
