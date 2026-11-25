// Where the camera's firmware and (optionally) Opal's tuning file come from on Linux.
#pragma once

#include <optional>
#include <string>

#include "depthai.h"

namespace Firmware {
inline const char *commit = "26f3d76548489ac14bc1c8ea9ef5ffc842e8adf7";
inline const char *name = "depthai-device-openvino-2021.4-26f3d76548489ac14bc1c8ea9ef5ffc842e8adf7.cmd";

/// Opal ships Luxonis's stock build of this commit (the copy inside Opal Composer is
/// byte-identical to the one in Luxonis's public archive), so the archive is the source here.
inline const char *sha256 = "a31f3b0f4284ba6352e83f6abae26539098553db5cab9a62294fa37935281f7c";
inline const size_t size = 25205008;
inline const char *luxonisArchive =
    "https://artifacts.luxonis.com/artifactory/luxonis-myriad-snapshot-local/depthai-device-side/"
    "26f3d76548489ac14bc1c8ea9ef5ffc842e8adf7/depthai-device-fwp-26f3d76548489ac14bc1c8ea9ef5ffc842e8adf7.tar.xz";

/// ~/.local/share/c1-tune: the firmware is kept here after the first download.
std::string dataDirectory();
/// ~/.config/c1-tune: settings, and tuning files dropped in by name.
std::string configDirectory();

/// The device firmware: from the data directory, else downloaded from Luxonis (8 MB, once).
/// Empty with `error` set when neither works. Blocks.
Bytes load(std::string &error);

/// Fetches Luxonis's archive with curl, unpacks the device .cmd with bsdtar, and returns it
/// only if it is exactly the firmware this app was written against.
Bytes download(std::string &error);

/// Opal's image tuning, if the user copied it over from a Mac: `explicitPath` when set,
/// else the file for the camera's sensor in the config directory (the original Opal C1 has
/// an IMX378 → Opal_v0.12.bin, a later revision an IMX582 → Opal582_v2.bin). `sensorReply`
/// is the getCameraSensorNames RPC reply. nullopt means: use the firmware's own tuning.
std::optional<Bytes> tuningBlob(const Bytes &sensorReply, const std::string &explicitPath, std::string &source);

std::optional<Bytes> readFile(const std::string &path);
std::string sha256Hex(const Bytes &data);
}
