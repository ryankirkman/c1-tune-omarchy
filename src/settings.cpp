#include "settings.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

const std::array<CameraSettings::ZoomStop, 20> CameraSettings::zoomStops = {{
    {100, 1, 2}, {107, 8, 15}, {108, 13, 24}, {110, 11, 20}, {117, 7, 12}, {120, 3, 5}, {125, 5, 8},
    {130, 13, 20}, {133, 2, 3}, {140, 7, 10}, {147, 11, 15}, {150, 3, 4}, {160, 4, 5}, {167, 5, 6},
    {173, 13, 15}, {175, 7, 8}, {180, 9, 10}, {183, 11, 12}, {187, 14, 15}, {200, 1, 1},
}};

int CameraSettings::nearestZoom(int percent) {
    int best = zoomStops[0].percent;
    for (auto &stop : zoomStops)   // the first of two equally near stops wins, as in the original
        if (std::abs(stop.percent - percent) < std::abs(best - percent)) best = stop.percent;
    return best;
}

Pipeline::Camera CameraSettings::pipelineCamera() const {
    Pipeline::Camera camera;
    camera.initialControl = fullControl();
    camera.orientation = orientation();
    camera.resolution = 2;   // THE_4_K
    int nearest = nearestZoom(zoom);
    ZoomStop stop = zoomStops[0];
    for (auto &s : zoomStops) if (s.percent == nearest) { stop = s; break; }
    if (stop.n != stop.d) { camera.ispScale[0] = camera.ispScale[2] = stop.n; camera.ispScale[1] = camera.ispScale[3] = stop.d; }
    float window = float(stop.d) / float(2 * stop.n);   // share of the scaled frame the 1080p window covers
    if (window < 1) {
        camera.sensorCropX = float(std::clamp(frameX, 0, 100)) / 100.0f * (1.0f - window);
        camera.sensorCropY = float(std::clamp(frameY, 0, 100)) / 100.0f * (1.0f - window);
    }
    return camera;
}

bool CameraSettings::needsRestart(const CameraSettings &n) const {
    return (sharpness && !n.sharpness) || (noiseReduction && !n.noiseReduction)
        || (colorNoiseReduction && !n.colorNoiseReduction) || flipped != n.flipped || microphone != n.microphone
        || zoom != n.zoom || (n.zoom != 100 && (frameX != n.frameX || frameY != n.frameY))
        || tuningFile != n.tuningFile;
}

static std::optional<int> parseInt(const std::string &s) {
    const char *first = s.data(), *last = s.data() + s.size();
    if (first != last && *first == '+') first++;
    int v = 0;
    auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc() || r.ptr != last || first == last) return {};
    return v;
}

CameraSettings CameraSettings::applying(const std::vector<std::pair<std::string, std::string>> &pairs) const {
    CameraSettings s = *this;
    for (auto &[key, raw] : pairs) {
        std::string value = raw;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        auto is = [&](std::initializer_list<const char *> words) {
            return std::any_of(words.begin(), words.end(), [&](const char *w) { return value == w; });
        };
        std::optional<bool> flag;
        if (is({"true", "on", "yes", "1"})) flag = true;
        else if (is({"false", "off", "no", "0"})) flag = false;
        bool clear = is({"default", "nil", ""});
        std::optional<int> number = parseInt(raw);
        auto within = [&](int lo, int hi, int current) { return number ? std::clamp(*number, lo, hi) : current; };
        auto level = [&](std::optional<int> current) -> std::optional<int> {
            if (clear) return {};
            return number ? std::optional(std::clamp(*number, 0, 4)) : current;
        };
        if (key == "autoExposure") s.autoExposure = flag.value_or(s.autoExposure);
        else if (key == "autoFocus") s.autoFocus = flag.value_or(s.autoFocus);
        else if (key == "autoWhiteBalance") s.autoWhiteBalance = flag.value_or(s.autoWhiteBalance);
        else if (key == "flipped") s.flipped = flag.value_or(s.flipped);
        else if (key == "microphone") s.microphone = flag.value_or(s.microphone);
        else if (key == "exposureUs") s.exposureUs = within(100, 33000, s.exposureUs);
        else if (key == "iso") s.iso = within(100, 1600, s.iso);
        else if (key == "exposureCompensation") s.exposureCompensation = within(-9, 9, s.exposureCompensation);
        else if (key == "lensPosition") s.lensPosition = within(0, 255, s.lensPosition);
        else if (key == "whiteBalanceK") s.whiteBalanceK = within(1000, 12000, s.whiteBalanceK);
        else if (key == "brightness") s.brightness = within(-10, 10, s.brightness);
        else if (key == "contrast") s.contrast = within(-10, 10, s.contrast);
        else if (key == "saturation") s.saturation = within(-10, 10, s.saturation);
        else if (key == "antiFlicker") s.antiFlicker = within(0, 3, s.antiFlicker);
        else if (key == "zoom") s.zoom = number ? nearestZoom(*number) : s.zoom;
        else if (key == "frameX") s.frameX = within(0, 100, s.frameX);
        else if (key == "frameY") s.frameY = within(0, 100, s.frameY);
        else if (key == "sharpness") s.sharpness = level(s.sharpness);
        else if (key == "noiseReduction") s.noiseReduction = level(s.noiseReduction);
        else if (key == "colorNoiseReduction") s.colorNoiseReduction = level(s.colorNoiseReduction);
        else if (key == "tuningFile") s.tuningFile = is({"default", "nil", "none", ""}) ? "" : raw;
    }
    return s;
}

// MARK: Camera commands

CameraControl CameraSettings::fullControl() const {
    CameraControl c;
    for (int g = 0; g < groupCount; g++) add(Group(g), c);
    return c;
}

CameraControl CameraSettings::control(Group group) const {
    CameraControl c;
    add(group, c);
    return c;
}

void CameraSettings::add(Group group, CameraControl &c) const {
    auto clamped = [](int v, int lo, int hi) { return std::clamp(v, lo, hi); };
    switch (group) {
    case exposure:
        if (autoExposure) {
            c.set(CameraControl::aeAuto);
            c.set(CameraControl::expCompensation_); c.expCompensation = int8_t(clamped(exposureCompensation, -128, 127));
        } else {
            c.set(CameraControl::aeManual);
            c.exposureTimeUs = uint32_t(std::max(exposureUs, 0));
            c.sensitivityIso = uint32_t(std::max(iso, 0));
        }
        break;
    case focus:
        c.set(CameraControl::afMode);
        if (autoFocus) {
            c.autoFocusMode = CameraControl::afContinuousVideo;
        } else {
            c.autoFocusMode = CameraControl::afOff;
            c.set(CameraControl::moveLens); c.lensPosition = uint8_t(clamped(lensPosition, 0, 255));
        }
        break;
    case whiteBalance:
        if (autoWhiteBalance) {
            c.set(CameraControl::awbMode_); c.awbMode = CameraControl::wbAuto;
        } else {
            c.set(CameraControl::wbColorTemp_); c.wbColorTemp = uint16_t(clamped(whiteBalanceK, 0, 65535));
        }
        break;
    case image:
        c.set(CameraControl::brightness_); c.brightness = int8_t(clamped(brightness, -128, 127));
        c.set(CameraControl::contrast_); c.contrast = int8_t(clamped(contrast, -128, 127));
        c.set(CameraControl::saturation_); c.saturation = int8_t(clamped(saturation, -128, 127));
        if (sharpness) { c.set(CameraControl::sharpness_); c.sharpness = uint8_t(clamped(*sharpness, 0, 255)); }
        if (noiseReduction) { c.set(CameraControl::lumaDenoise_); c.lumaDenoise = uint8_t(clamped(*noiseReduction, 0, 255)); }
        if (colorNoiseReduction) { c.set(CameraControl::chromaDenoise_); c.chromaDenoise = uint8_t(clamped(*colorNoiseReduction, 0, 255)); }
        break;
    case antiFlickerGroup:
        c.set(CameraControl::antiBanding);
        // As the original: clamp to a byte, and anything that isn't a mode means auto.
        c.antiBandingMode = clamped(antiFlicker, 0, 255) <= 3 ? uint8_t(clamped(antiFlicker, 0, 255)) : uint8_t(CameraControl::abAuto);
        break;
    default: break;
    }
}

// MARK: Persistence

static std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += char(c);
    }
    return out + "\"";
}

std::string CameraSettings::json() const {
    std::ostringstream o;
    auto b = [](bool v) { return v ? "true" : "false"; };
    auto opt = [](const std::optional<int> &v) { return v ? std::to_string(*v) : std::string("null"); };
    o << "{\"autoExposure\":" << b(autoExposure) << ",\"exposureUs\":" << exposureUs << ",\"iso\":" << iso
      << ",\"exposureCompensation\":" << exposureCompensation << ",\"autoFocus\":" << b(autoFocus)
      << ",\"lensPosition\":" << lensPosition << ",\"autoWhiteBalance\":" << b(autoWhiteBalance)
      << ",\"whiteBalanceK\":" << whiteBalanceK << ",\"brightness\":" << brightness << ",\"contrast\":" << contrast
      << ",\"saturation\":" << saturation << ",\"sharpness\":" << opt(sharpness)
      << ",\"noiseReduction\":" << opt(noiseReduction) << ",\"colorNoiseReduction\":" << opt(colorNoiseReduction)
      << ",\"antiFlicker\":" << antiFlicker << ",\"zoom\":" << zoom << ",\"frameX\":" << frameX << ",\"frameY\":" << frameY
      << ",\"flipped\":" << b(flipped) << ",\"microphone\":" << b(microphone)
      << ",\"tuningFile\":" << quoted(tuningFile) << "}";
    return o.str();
}

/// Reads the flat object `json()` writes: string keys with number, true/false, null or
/// string values. Anything unexpected ends the parse, keeping what was read so far.
CameraSettings CameraSettings::fromJson(const std::string &t) {
    CameraSettings s;
    size_t i = 0;
    auto space = [&] { while (i < t.size() && isspace((unsigned char)t[i])) i++; };
    auto string = [&](std::string &out) {
        if (i >= t.size() || t[i] != '"') return false;
        for (i++; i < t.size() && t[i] != '"'; i++) {
            if (t[i] == '\\' && i + 1 < t.size()) {
                i++;
                if (t[i] == 'u' && i + 4 < t.size()) { out += char(strtol(t.substr(i + 1, 4).c_str(), nullptr, 16)); i += 4; }
                else out += t[i] == 'n' ? '\n' : t[i] == 't' ? '\t' : t[i];
            } else out += t[i];
        }
        if (i >= t.size()) return false;
        i++;
        return true;
    };
    space();
    if (i >= t.size() || t[i] != '{') return s;
    i++;
    while (true) {
        space();
        std::string key, text;
        if (!string(key)) break;
        space();
        if (i >= t.size() || t[i] != ':') break;
        i++;
        space();
        bool isString = i < t.size() && t[i] == '"';
        if (isString) { if (!string(text)) break; }
        else { while (i < t.size() && t[i] != ',' && t[i] != '}' && !isspace((unsigned char)t[i])) text += t[i++]; }

        auto integer = [&](int &field) { if (auto v = parseInt(text)) field = *v; };
        auto flag = [&](bool &field) { if (text == "true") field = true; else if (text == "false") field = false; };
        auto level = [&](std::optional<int> &field) { if (text == "null") field.reset(); else if (auto v = parseInt(text)) field = *v; };
        if (key == "autoExposure") flag(s.autoExposure);
        else if (key == "exposureUs") integer(s.exposureUs);
        else if (key == "iso") integer(s.iso);
        else if (key == "exposureCompensation") integer(s.exposureCompensation);
        else if (key == "autoFocus") flag(s.autoFocus);
        else if (key == "lensPosition") integer(s.lensPosition);
        else if (key == "autoWhiteBalance") flag(s.autoWhiteBalance);
        else if (key == "whiteBalanceK") integer(s.whiteBalanceK);
        else if (key == "brightness") integer(s.brightness);
        else if (key == "contrast") integer(s.contrast);
        else if (key == "saturation") integer(s.saturation);
        else if (key == "sharpness") level(s.sharpness);
        else if (key == "noiseReduction") level(s.noiseReduction);
        else if (key == "colorNoiseReduction") level(s.colorNoiseReduction);
        else if (key == "antiFlicker") integer(s.antiFlicker);
        else if (key == "zoom") integer(s.zoom);
        else if (key == "frameX") integer(s.frameX);
        else if (key == "frameY") integer(s.frameY);
        else if (key == "flipped") flag(s.flipped);
        else if (key == "microphone") flag(s.microphone);
        else if (key == "tuningFile" && isString) s.tuningFile = text;
        space();
        if (i < t.size() && t[i] == ',') { i++; continue; }
        break;
    }
    return s;
}

std::string CameraSettings::path() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string dir = xdg && *xdg ? xdg : std::string(home ? home : "") + "/.config";
    return dir + "/c1-tune/settings.json";
}

CameraSettings CameraSettings::load() {
    std::ifstream in(path());
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return fromJson(text.str());
}

bool CameraSettings::save() const {
    // Written beside the target, then renamed, so a crash never leaves half a file.
    std::string target = path(), dir = target.substr(0, target.rfind('/')), tmp = target + ".tmp";
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << json() << "\n";
        if (!out.flush()) return false;
    }
    return rename(tmp.c_str(), target.c_str()) == 0;
}
