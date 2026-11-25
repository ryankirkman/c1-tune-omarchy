// Protocol tests: `make test`. Compares the encoders with reference bytes produced by
// compiling DepthAI's own headers (tests/fixtures, shared with C1 Tune for macOS),
// then checks the settings
// logic. No camera needed. C1TUNE_TEST_DOWNLOAD=1 also checks the firmware download.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/depthai.h"
#include "../src/firmware.h"
#include "../src/settings.h"

static int failures = 0;
static std::string fixtures;

static void check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "✓" : "✗", what.c_str());
    if (!ok) failures++;
}

static Bytes hex(const std::string &s) {
    Bytes out;
    std::string digits;
    for (char c : s) if (c != ' ') digits += c;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(uint8_t(strtol(digits.substr(i, 2).c_str(), nullptr, 16)));
    return out;
}

static Bytes fixture(const std::string &name) {
    std::ifstream in(fixtures + "/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

static std::string repeat(const std::string &s, int n) { std::string out; while (n--) out += s; return out; }

static void same(const Bytes &a, const Bytes &b, const std::string &what) {
    check(a == b && !a.empty(), what + " (" + std::to_string(a.size()) + " bytes)");
    if (a == b) return;
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    auto dump = [&](const Bytes &v) {
        std::string out;
        for (size_t k = i; k < v.size() && k < i + 12; k++) { char buf[4]; snprintf(buf, sizeof buf, "%02X ", v[k]); out += buf; }
        return out;
    };
    printf("  first difference at byte %zu: got %s expected %s\n", i, dump(a).c_str(), dump(b).c_str());
}

int main(int argc, char **argv) {
    fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    const std::string regions = "B903000000" "B9050000000000" "B9050000000000";

    // The original's tests, unchanged.
    CameraControl focus;
    focus.set(CameraControl::moveLens); focus.lensPosition = 130;
    same(focus.encoded(), hex("B91D10038082000000" "80FF" + regions + repeat("00", 19) + "0500000031000000"),
         "CameraControl setManualFocus(130)");

    CameraControl wb;
    wb.set(CameraControl::wbColorTemp_); wb.wbColorTemp = 4000;
    same(wb.encoded(), hex("B91D830000000000000200" "0300000000" "80FF" + regions + repeat("00", 16)
                           + "81A00F0000" "050000003A000000"), "CameraControl setManualWhiteBalance(4000)");

    same(BootImage::image({}, BootImage::boardConfig(0, true)),
         hex("4D4132789AA8003220ADDED0F19A9C003220FFFFFFFF9AA8003220ADDED0F19AA4003220010000008A00000178"
             "0B00E617EA21CD856B041D000000B908B90581E703813BF681E703813DF604B90100BA0000BEBB00BB0001000000"),
         "Boot header (UVC on, watchdog off)");

    auto defaults = Pipeline::Camera::depthaiDefaults();
    same(Pipeline::setPipelineSchemaRequest(defaults, {}), fixture("setPipelineSchema_request.bin"),
         "setPipelineSchema (camera defaults)");
    same(Pipeline::setPipelineSchemaRequest(defaults, {}, "asset:camTuning", 195916),
         fixture("setPipelineSchema_tuning_request.bin"), "setPipelineSchema (with tuning blob)");
    same(Pipeline::setPipelineSchemaRequest(defaults, Pipeline::Mic(), "asset:camTuning", 195916),
         fixture("setPipelineSchema_audio_request.bin"), "setPipelineSchema (tuning blob, AudioMic → UAC as Opal sets it)");
    same(RPC::request("setAssets", {Assets::tuning(195916)}), fixture("setAssets_request.bin"), "setAssets");
    same(RPC::request("memAlloc", {MsgPack::integer(195916)}), fixture("memAlloc_request.bin"), "memAlloc");
    check(RPC::hash("setLogLevel") == 0x8D4F73891DCAA01DULL, "RPC name hash");

    // Encoder edges the fixtures don't reach.
    { Nop n; n.sint(-64); n.sint(-65); n.sint(127); n.sint(128); n.sint(-129); n.sint(40000); n.uint(127); n.uint(128); n.uint(70000);
      same(n.bytes, hex("C0" "84BF" "7F" "858000" "857FFF" "86409C0000" "7F" "8080" "8270110100"), "libnop integer widths"); }
    same(MsgPack::array({MsgPack::integer(-1), MsgPack::integer(-33), MsgPack::integer(255), MsgPack::integer(65536),
                         MsgPack::real(0.1), MsgPack::real(30)}).encoded(),
         hex("96" "FF" "D0DF" "CCFF" "CE00010000" "CB3FB999999999999A" "CA41F00000"), "msgpack integer widths and floats");

    // Settings → commands.
    CameraSettings s;
    auto full = s.fullControl();
    check(full.cmdMask == ((1ULL << 7) | (1ULL << 11) | (1ULL << 26) | (1ULL << 8) | (1ULL << 31) | (1ULL << 44) | (1ULL << 28)
                           | (1ULL << 35) | (1ULL << 10))
          && full.expCompensation == -1 && full.saturation == 1 && full.sharpness == 3 && full.antiBandingMode == 3
          && full.awbMode == 1 && full.autoFocusMode == 3, "Default settings → one control message");

    auto manual = s.applying({{"autoExposure", "off"}, {"exposureUs", "99999"}, {"iso", "50"}, {"autoFocus", "no"},
                              {"lensPosition", "300"}, {"autoWhiteBalance", "false"}, {"whiteBalanceK", "4000"},
                              {"sharpness", "default"}, {"noiseReduction", "9"}, {"bogus", "1"}, {"contrast", "x"}});
    check(manual.exposureUs == 33000 && manual.iso == 100 && manual.lensPosition == 255 && !manual.sharpness
          && manual.noiseReduction == 4 && manual.contrast == 0, "--set clamps to the sliders' ranges and ignores junk");
    auto exposure = manual.control(CameraSettings::exposure);
    check(exposure.cmdMask == (1ULL << 6) && exposure.exposureTimeUs == 33000 && exposure.sensitivityIso == 100, "Manual exposure command");
    auto lens = manual.control(CameraSettings::focus);
    check(lens.cmdMask == ((1ULL << 26) | (1ULL << 4)) && lens.autoFocusMode == 0 && lens.lensPosition == 255, "Manual focus command");
    same(manual.control(CameraSettings::whiteBalance).encoded(), wb.encoded(), "Manual white balance command matches the fixture");
    CameraControl merged;
    for (int g = 0; g < CameraSettings::groupCount; g++) merged.absorb(manual.control(CameraSettings::Group(g)));
    check(merged == manual.fullControl(), "Per-group commands merge into the full control");

    // Zoom: every step, its ISP scale and where the 1080p window lands.
    bool stops = CameraSettings::zoomStops.size() == 20;
    for (auto &stop : CameraSettings::zoomStops) {
        CameraSettings z; z.zoom = stop.percent;
        auto camera = z.pipelineCamera();
        bool scaled = stop.n != stop.d;
        stops = stops && stop.n <= 16 && (3840 * stop.n / stop.d) % 2 == 0 && (3840 * stop.n) % stop.d == 0
            && camera.resolution == 2 && camera.ispScale[0] == (scaled ? stop.n : 0) && camera.ispScale[1] == (scaled ? stop.d : 0)
            && camera.ispScale[2] == camera.ispScale[0] && camera.ispScale[3] == camera.ispScale[1]
            && (stop.percent == 100 ? camera.sensorCropX == -1 : camera.sensorCropX >= 0);
    }
    check(stops, "20 zoom steps, numerators ≤ 16, even scaled sizes");
    check(CameraSettings::nearestZoom(104) == 107 && CameraSettings::nearestZoom(103) == 100 && CameraSettings::nearestZoom(999) == 200
          && CameraSettings::nearestZoom(170) == 167, "Zoom snaps to the nearest step");
    { CameraSettings z; z.zoom = 200; z.frameX = 0; z.frameY = 100;
      auto c = z.pipelineCamera();
      check(c.sensorCropX == 0 && c.sensorCropY == 0.5f && c.ispScale[0] == 0, "2× is an unscaled crop; framing moves the window"); }

    CameraSettings a, b;
    b.zoom = 125; bool r1 = a.needsRestart(b);
    b = a; b.frameX = 10; bool r2 = !a.needsRestart(b);          // framing does nothing at 1×
    b = a; b.sharpness.reset(); bool r3 = a.needsRestart(b);
    b = a; b.saturation = 5; b.noiseReduction = 2; bool r4 = !a.needsRestart(b);
    b = a; b.microphone = true; bool r5 = a.needsRestart(b);
    b = a; b.flipped = true; bool r6 = a.needsRestart(b);
    check(r1 && r2 && r3 && r4 && r5 && r6, "Which changes restart the camera");

    manual.tuningFile = "/tmp/a \"b\"\\c.bin";
    check(CameraSettings::fromJson(manual.json()) == manual && CameraSettings::fromJson("{\"zoom\":150}").zoom == 150
          && CameraSettings::fromJson("{\"zoom\":150}").saturation == 1 && CameraSettings::fromJson("garbage") == CameraSettings(),
          "Settings survive a save and load; missing keys keep defaults");

    // Frame descriptor patch on a made-up descriptor block.
    { Bytes fw(64, 0xEE);
      Bytes format = hex("1B2404" "0104" "4E563132"); format.resize(0x1B, 0);
      fw.insert(fw.end(), format.begin(), format.end());
      const int sizes[4][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}};
      for (int k = 0; k < 4; k++) {
          Bytes frame(0x1E, uint8_t(k));
          frame[0] = 0x1E; frame[1] = 0x24; frame[2] = 0x05; frame[3] = uint8_t(k + 1);
          frame[5] = sizes[k][0] & 0xFF; frame[6] = sizes[k][0] >> 8; frame[7] = sizes[k][1] & 0xFF; frame[8] = sizes[k][1] >> 8;
          fw.insert(fw.end(), frame.begin(), frame.end());
      }
      Bytes before = fw;
      int n = BootImage::patchFrameSizes(fw);
      size_t first = 64 + 0x1B;
      bool ok = n == 3 && fw.size() == before.size();
      for (int k = 0; k < 4; k++)
          ok = ok && Bytes(fw.begin() + first + k * 0x1E, fw.begin() + first + (k + 1) * 0x1E)
                  == Bytes(before.begin() + first + 2 * 0x1E, before.begin() + first + 3 * 0x1E);
      check(ok && BootImage::patchFrameSizes(fw) == 0, "Frame descriptors all become the 1080p entry"); }

    check(Firmware::sha256Hex(Bytes{'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
          && Firmware::sha256Hex(Bytes(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256");

    if (getenv("C1TUNE_TEST_DOWNLOAD")) {
        std::string error;
        Bytes fw = Firmware::download(error);
        check(fw.size() == 25205008, "Firmware downloaded from Luxonis and verified" + (error.empty() ? "" : ": " + error));
        check(BootImage::patchFrameSizes(fw) > 0, "Real firmware has frame descriptors to patch");
    }

    printf(failures == 0 ? "PASS\n" : "FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
