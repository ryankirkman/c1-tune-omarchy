#include "depthai.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
template <typename T> void le(Bytes &out, T v) {
    for (size_t i = 0; i < sizeof(T); i++) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}
template <typename T> void be(Bytes &out, T v) {
    for (size_t i = sizeof(T); i-- > 0;) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}
uint32_t bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }
uint64_t bits(double v) { uint64_t u; std::memcpy(&u, &v, 8); return u; }
void append(Bytes &out, const Bytes &b) { out.insert(out.end(), b.begin(), b.end()); }
}

// MARK: - libnop

void Nop::uint(uint64_t v) {
    if (v < 0x80) bytes.push_back(uint8_t(v));
    else if (v < 0x100) { bytes.push_back(0x80); bytes.push_back(uint8_t(v)); }
    else if (v < 0x10000) { bytes.push_back(0x81); le(bytes, uint16_t(v)); }
    else if (v < 0x100000000ULL) { bytes.push_back(0x82); le(bytes, uint32_t(v)); }
    else { bytes.push_back(0x83); le(bytes, v); }
}

void Nop::sint(int64_t v) {
    if (v >= -64 && v < 0x80) bytes.push_back(uint8_t(int8_t(v)));
    else if (v >= INT8_MIN && v <= INT8_MAX) { bytes.push_back(0x84); bytes.push_back(uint8_t(int8_t(v))); }
    else if (v >= INT16_MIN && v <= INT16_MAX) { bytes.push_back(0x85); le(bytes, int16_t(v)); }
    else if (v >= INT32_MIN && v <= INT32_MAX) { bytes.push_back(0x86); le(bytes, int32_t(v)); }
    else { bytes.push_back(0x87); le(bytes, v); }
}

void Nop::float32(float v) { bytes.push_back(0x88); le(bytes, bits(v)); }

void Nop::string(const std::string &s) {
    bytes.push_back(0xBD);
    uint(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
}

void Nop::intMap(const std::vector<std::pair<int64_t, int64_t>> &m) {
    bytes.push_back(0xBB);
    uint(m.size());
    for (auto &[k, v] : m) { sint(k); sint(v); }
}

// MARK: - msgpack

static void packInt(int64_t v, Bytes &out) {
    if (v >= 0) {
        if (v < 0x80) out.push_back(uint8_t(v));
        else if (v < 0x100) { out.push_back(0xCC); out.push_back(uint8_t(v)); }
        else if (v < 0x10000) { out.push_back(0xCD); be(out, uint16_t(v)); }
        else if (v < 0x100000000LL) { out.push_back(0xCE); be(out, uint32_t(v)); }
        else { out.push_back(0xCF); be(out, uint64_t(v)); }
    } else {
        if (v >= -32) out.push_back(uint8_t(int8_t(v)));
        else if (v >= INT8_MIN) { out.push_back(0xD0); out.push_back(uint8_t(int8_t(v))); }
        else if (v >= INT16_MIN) { out.push_back(0xD1); be(out, int16_t(v)); }
        else if (v >= INT32_MIN) { out.push_back(0xD2); be(out, int32_t(v)); }
        else { out.push_back(0xD3); be(out, v); }
    }
}

static void packHeader(size_t n, uint8_t fix, uint8_t b16, Bytes &out) {
    if (n < 16) out.push_back(fix | uint8_t(n));
    else { out.push_back(b16); be(out, uint16_t(n)); }
}

static void packString(const std::string &s, Bytes &out) {
    size_t n = s.size();
    if (n < 32) out.push_back(0xA0 | uint8_t(n));
    else if (n < 0x100) { out.push_back(0xD9); out.push_back(uint8_t(n)); }
    else { out.push_back(0xDA); be(out, uint16_t(n)); }
    out.insert(out.end(), s.begin(), s.end());
}

void MsgPack::encode(Bytes &out) const {
    switch (kind) {
    case Null: out.push_back(0xC0); break;
    case Bool: out.push_back(b ? 0xC3 : 0xC2); break;
    case Int: packInt(i, out); break;
    case Float:
        if (double(float(f)) == f) { out.push_back(0xCA); be(out, bits(float(f))); }
        else { out.push_back(0xCB); be(out, bits(f)); }
        break;
    case String: packString(s, out); break;
    case Array:
        packHeader(a.size(), 0x90, 0xDC, out);
        for (auto &item : a) item.encode(out);
        break;
    case Object:
        packHeader(o.size(), 0x80, 0xDE, out);
        for (auto &[key, value] : o) { packString(key, out); value.encode(out); }
        break;
    case Raw: append(out, raw); break;
    }
}

// MARK: - Camera control

void CameraControl::absorb(const CameraControl &o) {
    cmdMask |= o.cmdMask;
    if (o.has(afMode)) autoFocusMode = o.autoFocusMode;
    if (o.has(moveLens)) lensPosition = o.lensPosition;
    if (o.has(aeManual)) { exposureTimeUs = o.exposureTimeUs; sensitivityIso = o.sensitivityIso; frameDurationUs = o.frameDurationUs; }
    if (o.has(expCompensation_)) expCompensation = o.expCompensation;
    if (o.has(awbMode_)) awbMode = o.awbMode;
    if (o.has(wbColorTemp_)) wbColorTemp = o.wbColorTemp;
    if (o.has(antiBanding)) antiBandingMode = o.antiBandingMode;
    if (o.has(brightness_)) brightness = o.brightness;
    if (o.has(contrast_)) contrast = o.contrast;
    if (o.has(saturation_)) saturation = o.saturation;
    if (o.has(sharpness_)) sharpness = o.sharpness;
    if (o.has(lumaDenoise_)) lumaDenoise = o.lumaDenoise;
    if (o.has(chromaDenoise_)) chromaDenoise = o.chromaDenoise;
}

Bytes CameraControl::nop() const {
    Nop n;
    n.structure(29);
    n.uint(cmdMask);
    n.uint(autoFocusMode); n.uint(lensPosition);
    n.uint(lensPosAutoInfinity); n.uint(lensPosAutoMacro);
    n.uint(lensPosLimitMin); n.uint(lensPosLimitMax);
    n.structure(3); n.uint(exposureTimeUs); n.uint(sensitivityIso); n.uint(frameDurationUs);
    for (int r = 0; r < 2; r++) { n.structure(5); for (int k = 0; k < 5; k++) n.uint(0); }   // aeRegion, afRegion
    n.uint(awbMode); n.uint(0); n.uint(antiBandingMode);        // awb, scene, antibanding
    n.boolean(false); n.boolean(false);                         // aeLock, awbLock
    n.uint(0); n.uint(0); n.uint(0); n.uint(0);                 // captureIntent, controlMode, effect, frameSync
    n.sint(expCompensation); n.sint(brightness); n.sint(contrast); n.sint(saturation);
    n.uint(sharpness); n.uint(lumaDenoise); n.uint(chromaDenoise);
    n.uint(wbColorTemp);
    n.uint(0); n.uint(0);                                       // lowPower burst/discard
    return n.bytes;
}

Bytes CameraControl::encoded() const {
    Bytes d = nop();
    uint32_t size = uint32_t(d.size());
    le(d, int32_t(5));
    le(d, size);
    return d;
}

// MARK: - Boot image

Bytes BootImage::boardConfig(std::optional<uint32_t> watchdogMs, bool uvc) {
    Nop n;
    n.structure(8);
    n.structure(5); n.uint(0x03E7); n.uint(0xF63B); n.uint(0x03E7); n.uint(0xF63D); n.sint(4);   // usb, SUPER speed
    n.structure(1); n.uint(0);                                                                   // network mtu
    n.emptyArray();                                                                              // sysctl
    if (watchdogMs) n.uint(*watchdogMs); else n.none();
    n.none();                                                                                    // watchdogInitialDelayMs
    n.emptyMap(); n.emptyMap();                                                                  // gpio, uart
    if (uvc) n.boolean(true); else n.none();
    return n.bytes;
}

int BootImage::patchFrameSizes(Bytes &fw) {
    // NV12 uncompressed format descriptor (subtype 0x04); 4 frame descriptors follow it.
    static const uint8_t format[] = {0x1B, 0x24, 0x04, 0x01, 0x04, 0x4E, 0x56, 0x31, 0x32};
    const size_t size = 0x1E;
    int patched = 0;
    auto search = fw.begin();
    while (true) {
        auto r = std::search(search, fw.end(), std::begin(format), std::end(format));
        if (r == fw.end()) break;
        search = r + sizeof format;
        size_t first = size_t(r - fw.begin()) + 0x1B;
        std::vector<size_t> frames;
        for (size_t index = 1; index <= 4; index++) {
            size_t f = first + (index - 1) * size;
            if (!(f + size <= fw.size() && fw[f] == 0x1E && fw[f + 1] == 0x24 && fw[f + 2] == 0x05 && fw[f + 3] == index)) break;
            frames.push_back(f);
        }
        // The 1080p entry: width 0x0780, height 0x0438.
        auto source = std::find_if(frames.begin(), frames.end(), [&](size_t f) {
            return fw[f + 5] == 0x80 && fw[f + 6] == 0x07 && fw[f + 7] == 0x38 && fw[f + 8] == 0x04;
        });
        if (source == frames.end()) continue;
        Bytes entry(fw.begin() + *source, fw.begin() + *source + size);
        for (size_t f : frames) {
            if (f == *source) continue;
            std::copy(entry.begin(), entry.end(), fw.begin() + f);
            patched++;
        }
    }
    return patched;
}

Bytes BootImage::image(const Bytes &firmware, const Bytes &config) {
    Bytes h = {0x4D, 0x41, 0x32, 0x78,
        0x9A, 0xA8, 0x00, 0x32, 0x20, 0xAD, 0xDE, 0xD0, 0xF1, 0x9A, 0x9C, 0x00, 0x32, 0x20, 0xFF, 0xFF, 0xFF, 0xFF,
        0x9A, 0xA8, 0x00, 0x32, 0x20, 0xAD, 0xDE, 0xD0, 0xF1, 0x9A, 0xA4, 0x00, 0x32, 0x20, 0x01, 0x00, 0x00, 0x00, 0x8A};
    size_t pad = (4 - (12 + config.size()) % 4) % 4;
    uint32_t sum = 5381;
    for (uint8_t b : config) sum = sum * 33 + b;
    le(h, uint32_t(0x78010000)); le(h, uint16_t((12 + config.size() + pad) / 4)); le(h, uint32_t(0x21EA17E6));
    le(h, sum); le(h, uint32_t(config.size()));
    append(h, config);
    h.insert(h.end(), pad, 0);
    h.reserve(h.size() + firmware.size());
    append(h, firmware);
    return h;
}

// MARK: - Pipeline

Pipeline::Camera Pipeline::Camera::depthaiDefaults() {
    Camera c;
    c.resolution = 0; c.videoWidth = -1; c.videoHeight = -1; c.boardSocket = -1;
    c.interleaved = true; c.orientation = -1; c.uvcGpios = false; c.controlMaxDataSize = 5 << 20; c.xlinkChunkSize = -1;
    return c;
}

Bytes Pipeline::setPipelineSchemaRequest(const Camera &camera, const std::optional<Mic> &mic,
                                         const std::string &tuningBlobUri, std::optional<int64_t> tuningBlobSize) {
    return RPC::request("setPipelineSchema", {schemaJSON(camera, mic, tuningBlobUri, tuningBlobSize)});
}

MsgPack Pipeline::schemaJSON(const Camera &camera, const std::optional<Mic> &mic,
                             const std::string &tuningBlobUri, std::optional<int64_t> tuningBlobSize) {
    using M = MsgPack;
    auto io = [](const std::string &name, int64_t type, bool blocking, int64_t queueSize = 8, bool waitForMessage = false) {
        return M::array({M::array({M::string(""), M::string(name)}),
                         M::object({{"blocking", M::boolean(blocking)}, {"group", M::string("")}, {"name", M::string(name)},
                                    {"queueSize", M::integer(queueSize)}, {"type", M::integer(type)},
                                    {"waitForMessage", M::boolean(waitForMessage)}})});
    };
    auto node = [](int64_t id, const std::string &name, const Bytes &properties, std::vector<M> ioInfo) {
        std::vector<M> props;
        props.reserve(properties.size());
        for (uint8_t b : properties) props.push_back(M::integer(b));
        return M::array({M::integer(id), M::object({{"id", M::integer(id)}, {"ioInfo", M::array(std::move(ioInfo))},
                                                    {"name", M::string(name)}, {"properties", M::array(std::move(props))}})});
    };
    auto connection = [](int64_t a, const std::string &out, int64_t b, const std::string &input) {
        return M::object({{"node1Id", M::integer(a)}, {"node1Output", M::string(out)}, {"node1OutputGroup", M::string("")},
                          {"node2Id", M::integer(b)}, {"node2Input", M::string(input)}, {"node2InputGroup", M::string("")}});
    };
    const int64_t sender = 0, receiver = 3;
    // Nodes and their IO are in the order depthai-core's unordered_maps serialize them.
    std::vector<M> nodes;
    std::vector<M> connections = {connection(0, "out", 1, "inputControl"), connection(1, "video", 2, "in")};
    if (mic) {
        // IO as registered by the AudioMic/UAC constructors in Opal's libdepthai-core;
        // UAC.inputConfig's queue size of 1 is what Opal sets.
        nodes.push_back(node(4, "UAC", uacProperties(), {
            io("input", receiver, false, 4),
            io("inputConfig", receiver, false, 1),
        }));
        nodes.push_back(node(3, "AudioMic", audioMicProperties(*mic), {
            io("outBack", sender, false), io("out", sender, false),
            io("inputConfig", receiver, false, 4),
        }));
        connections.push_back(connection(3, "out", 4, "input"));
    }
    nodes.push_back(node(2, "UVC", uvcProperties(camera.uvcGpios), {io("in", receiver, true, 8, true)}));
    nodes.push_back(node(1, "ColorCamera", colorCameraProperties(camera), {
        io("isp", sender, false), io("still", sender, false),
        io("preview", sender, false), io("video", sender, false),
        io("raw", sender, false), io("inputControl", receiver, true),
        io("inputConfig", receiver, false),
    }));
    nodes.push_back(node(0, "XLinkIn", xlinkInProperties("control", camera.controlMaxDataSize), {io("out", sender, false)}));
    M global = M::object({
        {"calibData", M::null()},
        {"cameraTuningBlobSize", tuningBlobSize ? M::integer(*tuningBlobSize) : M::null()},
        {"cameraTuningBlobUri", M::string(tuningBlobUri)},
        {"imageManipAdjustCmxSize", M::integer(0)},
        {"leonCssFrequencyHz", M::real(700000000)}, {"leonMssFrequencyHz", M::real(700000000)},
        {"pipelineName", M::null()}, {"pipelineVersion", M::null()}, {"xlinkChunkSize", M::integer(camera.xlinkChunkSize)},
    });
    return M::object({
        {"connections", M::array(std::move(connections))},
        {"globalProperties", std::move(global)},
        {"nodes", M::array(std::move(nodes))},
    });
}

Bytes Pipeline::xlinkInProperties(const std::string &stream, uint32_t maxDataSize) {
    Nop n;
    n.structure(3); n.string(stream); n.uint(maxDataSize); n.uint(8);
    return n.bytes;
}

/// UVC node: GPIO levels at init, stream on and stream off (pin → level).
Bytes Pipeline::uvcProperties(bool gpios) {
    Nop n;
    n.structure(3);
    if (gpios) {
        n.intMap({{58, 0}, {37, 0}, {34, 0}}); n.intMap({{58, 1}, {37, 1}, {34, 1}}); n.intMap({{58, 0}, {37, 0}, {34, 0}});
    } else {
        n.emptyMap(); n.emptyMap(); n.emptyMap();
    }
    return n.bytes;
}

/// RawAudioInConfig: { { micGain, disableOutput, passThrough } }.
Bytes Pipeline::audioInConfig(float gain) {
    Nop n;
    n.structure(1); n.structure(3); n.float32(gain); n.boolean(false); n.boolean(false);
    return n.bytes;
}

Bytes Pipeline::audioMicProperties(const Mic &m) {
    Nop n;
    n.structure(9);
    // AudioInConfig::setMicGainDecibels: exp10(dB / 20) in double, stored as float.
    float gain = float(std::pow(10.0, double(m.gainDecibels / 20)));
    n.append(audioInConfig(gain));
    n.boolean(m.backMics); n.boolean(m.autoGain);
    n.uint(48000); n.uint(1440); n.uint(1440);     // sample rate, I2S read and XLink send sizes (samples)
    n.boolean(true); n.boolean(m.xlinkApplyGain);  // XLink interleaved, apply gain on XLink
    n.uint(m.xlinkSampleSizeBytes);
    return n.bytes;
}

Bytes Pipeline::uacProperties() {
    Nop n;
    n.structure(1);
    n.append(audioInConfig(1));
    return n.bytes;
}

Bytes Pipeline::colorCameraProperties(const Camera &c) {
    Nop n;
    n.structure(24);
    n.append(c.initialControl.nop());
    n.sint(c.boardSocket); n.sint(c.orientation); n.sint(0);   // board socket, orientation, colorOrder BGR
    n.boolean(c.interleaved); n.boolean(false);                // interleaved, fp16
    n.uint(300); n.uint(300);                                  // preview height, width
    n.sint(c.videoWidth); n.sint(c.videoHeight); n.sint(-1); n.sint(-1);   // video, still
    n.sint(c.resolution); n.float32(c.fps);
    n.float32(c.sensorCropX); n.float32(c.sensorCropY);        // sensor crop (video window position)
    n.boolean(true); n.boolean(false); n.boolean(false);       // previewKeepAspectRatio, rawStreamMetadataOnly, rawPdafWindows8x6
    n.uint(0); n.uint(0); n.uint(0); n.uint(0);                // pdOffX/Y, pdWinW/H
    n.structure(4);                                            // ispScale
    n.sint(c.ispScale[0]); n.sint(c.ispScale[1]); n.sint(c.ispScale[2]); n.sint(c.ispScale[3]);
    return n.bytes;
}

// MARK: - Assets

MsgPack Assets::tuning(int64_t size) {
    using M = MsgPack;
    return M::object({{"map", M::object({{"/pipeline/camTuning", M::object({
        {"alignment", M::integer(64)}, {"offset", M::integer(0)}, {"size", M::integer(size)},
    })}})}});
}

// MARK: - RPC

Bytes RPC::request(const std::string &name, const std::vector<MsgPack> &args) {
    Bytes h = {0xCF};
    be(h, hash(name));
    return MsgPack::array({MsgPack::integer(1), MsgPack::integer(1), MsgPack::rawBytes(h),
                           args.empty() ? MsgPack::null() : MsgPack::array(args)}).encoded();
}

uint64_t RPC::hash(const std::string &name) {
    uint64_t h = 1125899906842597ULL;
    for (unsigned char c : name) h = 31 * h + c;
    return h;
}

bool RPC::succeeded(const Bytes &r) {
    return r.size() >= 4 && (r[0] == 0x93 || r[0] == 0x94) && r[3] == 0x01;
}

std::optional<uint64_t> RPC::trailingUInt(const Bytes &b) {
    if (b.size() < 5) return {};
    const uint8_t *v = b.data() + 4;
    size_t n = b.size() - 4;
    auto big = [&](size_t count) { uint64_t r = 0; for (size_t k = 1; k <= count; k++) r = r << 8 | v[k]; return r; };
    if (v[0] <= 0x7F) return v[0];
    if (v[0] == 0xCC && n >= 2) return big(1);
    if (v[0] == 0xCD && n >= 3) return big(2);
    if (v[0] == 0xCE && n >= 5) return big(4);
    if (v[0] == 0xCF && n >= 9) return big(8);
    return {};
}
