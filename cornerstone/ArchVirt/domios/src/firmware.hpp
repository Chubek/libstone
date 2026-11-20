#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace domios {
enum class FirmwareSection : std::uint32_t { dit = 1, logo, background, lua, manifest };
struct FirmwarePart { FirmwareSection kind; std::vector<std::uint8_t> bytes; };
std::vector<std::uint8_t> pack_firmware(const std::vector<FirmwarePart>& parts);
bool unpack_firmware(std::span<const std::uint8_t> image, std::vector<FirmwarePart>& parts, std::string& error);
}
