#pragma once

#include <cstddef>
#include <cstdint>

namespace domvirt::boot {

struct EmbeddedImage {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    explicit operator bool() const noexcept { return data != nullptr && size != 0; }
};

EmbeddedImage embedded_domios_image() noexcept;
EmbeddedImage embedded_domix_image(std::uint16_t machine = 62) noexcept;

} // namespace domvirt::boot
