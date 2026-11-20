#include "embedded_images.hpp"

namespace domvirt::boot {

#ifdef DOMVIRT_EMBED_IMAGES
namespace embedded {
extern const unsigned char domios_image[];
extern const unsigned long long domios_image_size;
extern const unsigned char domix_image[];
extern const unsigned long long domix_image_size;
extern const unsigned char domix_aarch64_image[];
extern const unsigned long long domix_aarch64_image_size;
extern const unsigned char domix_riscv64_image[];
extern const unsigned long long domix_riscv64_image_size;
} // namespace embedded
#endif

EmbeddedImage embedded_domios_image() noexcept {
#ifdef DOMVIRT_EMBED_IMAGES
    return {embedded::domios_image,
            static_cast<std::size_t>(embedded::domios_image_size)};
#else
    return {};
#endif
}

EmbeddedImage embedded_domix_image([[maybe_unused]] std::uint16_t machine) noexcept {
#ifdef DOMVIRT_EMBED_IMAGES
    if (machine == 243) return {embedded::domix_riscv64_image,
                               static_cast<std::size_t>(embedded::domix_riscv64_image_size)};
    if (machine == 183) return {embedded::domix_aarch64_image,
                               static_cast<std::size_t>(embedded::domix_aarch64_image_size)};
    if (machine != 62) return {};
    return {embedded::domix_image,
            static_cast<std::size_t>(embedded::domix_image_size)};
#else
    return {};
#endif
}

} // namespace domvirt::boot
