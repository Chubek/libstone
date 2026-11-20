#include "firmware.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <toml++/toml.hpp>

int main(int argc, char** argv) {
    try {
        std::map<std::string, std::filesystem::path> args;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc || !args.emplace(argv[i], argv[i + 1]).second)
                throw std::runtime_error("Expected unique --section PATH arguments");
        }
        const auto manifest = toml::parse_file(args.at("--manifest").string());
        if (manifest["image_format_version"].value<int>() != 2)
            throw std::runtime_error("Expected image_format_version=2");
        std::vector<domios::FirmwarePart> parts;
        for (const auto& [name, kind] : std::map<std::string, domios::FirmwareSection>{
                {"--dit", domios::FirmwareSection::dit}, {"--logo", domios::FirmwareSection::logo},
                {"--background", domios::FirmwareSection::background}, {"--lua", domios::FirmwareSection::lua},
                {"--manifest", domios::FirmwareSection::manifest}}) {
            std::ifstream in(args.at(name), std::ios::binary);
            if (!in) throw std::runtime_error("Cannot read " + args.at(name).string());
            parts.push_back({kind, {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}});
        }
        const auto image = domios::pack_firmware(parts);
        const auto output = args.at("--output");
        const auto temporary = output.string() + ".tmp";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()), image.size());
        out.close();
        if (!out) throw std::runtime_error("Failed writing firmware");
        std::filesystem::rename(temporary, output);
    } catch (const std::exception& error) {
        std::cerr << "mkimg: " << error.what() << '\n'; return 1;
    }
    return 0;
}
