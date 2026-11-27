#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ttytk::color {

/** An opaque sRGB color with 8-bit components. */
struct Rgb {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};

    [[nodiscard]] friend constexpr auto operator==(const Rgb&, const Rgb&) -> bool = default;
};

namespace detail {

inline constexpr std::array<Rgb, 16> ansi_16 = {{
    {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
    {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {229, 229, 229},
    {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
    {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255},
}};

[[nodiscard]] constexpr auto cube_value(const unsigned int index) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(index == 0 ? 0 : 55 + index * 40);
}

[[nodiscard]] constexpr auto palette_256(const std::uint8_t index) noexcept -> Rgb {
    if (index < 16) return ansi_16[index];
    if (index >= 232) {
        const auto shade = static_cast<std::uint8_t>(8 + (index - 232) * 10);
        return {shade, shade, shade};
    }
    const auto value = static_cast<unsigned int>(index - 16);
    return {cube_value(value / 36), cube_value((value / 6) % 6), cube_value(value % 6)};
}

[[nodiscard]] constexpr auto hex_value(const char value) noexcept -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

[[nodiscard]] constexpr auto hex_component(std::string_view text, bool scale) noexcept -> int {
    if (text.empty() || text.size() > 4) return -1;
    unsigned value = 0;
    for (const char digit : text) {
        const auto hex = hex_value(digit);
        if (hex < 0) return -1;
        value = value * 16 + static_cast<unsigned>(hex);
    }
    if (scale) {
        const auto max = (1U << (4 * text.size())) - 1;
        return static_cast<int>((value * 255 + max / 2) / max);
    }
    if (text.size() == 1) return static_cast<int>(value << 4);
    return static_cast<int>(value >> (4 * (text.size() - 2)));
}

struct Named {
    std::string_view name;
    Rgb rgb;
};

// X.Org rgb 1.1.0 rgb.txt, normalized to lowercase without spaces.
inline constexpr std::array<Named, 676> names = {{
    Named{"aliceblue", {240, 248, 255}}, Named{"antiquewhite", {250, 235, 215}},
    Named{"antiquewhite1", {255, 239, 219}}, Named{"antiquewhite2", {238, 223, 204}},
    Named{"antiquewhite3", {205, 192, 176}}, Named{"antiquewhite4", {139, 131, 120}},
    Named{"aqua", {0, 255, 255}}, Named{"aquamarine", {127, 255, 212}},
    Named{"aquamarine1", {127, 255, 212}}, Named{"aquamarine2", {118, 238, 198}},
    Named{"aquamarine3", {102, 205, 170}}, Named{"aquamarine4", {69, 139, 116}},
    Named{"azure", {240, 255, 255}}, Named{"azure1", {240, 255, 255}},
    Named{"azure2", {224, 238, 238}}, Named{"azure3", {193, 205, 205}},
    Named{"azure4", {131, 139, 139}}, Named{"beige", {245, 245, 220}},
    Named{"bisque", {255, 228, 196}}, Named{"bisque1", {255, 228, 196}},
    Named{"bisque2", {238, 213, 183}}, Named{"bisque3", {205, 183, 158}},
    Named{"bisque4", {139, 125, 107}}, Named{"black", {0, 0, 0}},
    Named{"blanchedalmond", {255, 235, 205}}, Named{"blue", {0, 0, 255}},
    Named{"blue1", {0, 0, 255}}, Named{"blue2", {0, 0, 238}},
    Named{"blue3", {0, 0, 205}}, Named{"blue4", {0, 0, 139}},
    Named{"blueviolet", {138, 43, 226}}, Named{"brown", {165, 42, 42}},
    Named{"brown1", {255, 64, 64}}, Named{"brown2", {238, 59, 59}},
    Named{"brown3", {205, 51, 51}}, Named{"brown4", {139, 35, 35}},
    Named{"burlywood", {222, 184, 135}}, Named{"burlywood1", {255, 211, 155}},
    Named{"burlywood2", {238, 197, 145}}, Named{"burlywood3", {205, 170, 125}},
    Named{"burlywood4", {139, 115, 85}}, Named{"cadetblue", {95, 158, 160}},
    Named{"cadetblue1", {152, 245, 255}}, Named{"cadetblue2", {142, 229, 238}},
    Named{"cadetblue3", {122, 197, 205}}, Named{"cadetblue4", {83, 134, 139}},
    Named{"chartreuse", {127, 255, 0}}, Named{"chartreuse1", {127, 255, 0}},
    Named{"chartreuse2", {118, 238, 0}}, Named{"chartreuse3", {102, 205, 0}},
    Named{"chartreuse4", {69, 139, 0}}, Named{"chocolate", {210, 105, 30}},
    Named{"chocolate1", {255, 127, 36}}, Named{"chocolate2", {238, 118, 33}},
    Named{"chocolate3", {205, 102, 29}}, Named{"chocolate4", {139, 69, 19}},
    Named{"coral", {255, 127, 80}}, Named{"coral1", {255, 114, 86}},
    Named{"coral2", {238, 106, 80}}, Named{"coral3", {205, 91, 69}},
    Named{"coral4", {139, 62, 47}}, Named{"cornflowerblue", {100, 149, 237}},
    Named{"cornsilk", {255, 248, 220}}, Named{"cornsilk1", {255, 248, 220}},
    Named{"cornsilk2", {238, 232, 205}}, Named{"cornsilk3", {205, 200, 177}},
    Named{"cornsilk4", {139, 136, 120}}, Named{"crimson", {220, 20, 60}},
    Named{"cyan", {0, 255, 255}}, Named{"cyan1", {0, 255, 255}},
    Named{"cyan2", {0, 238, 238}}, Named{"cyan3", {0, 205, 205}},
    Named{"cyan4", {0, 139, 139}}, Named{"darkblue", {0, 0, 139}},
    Named{"darkcyan", {0, 139, 139}}, Named{"darkgoldenrod", {184, 134, 11}},
    Named{"darkgoldenrod1", {255, 185, 15}}, Named{"darkgoldenrod2", {238, 173, 14}},
    Named{"darkgoldenrod3", {205, 149, 12}}, Named{"darkgoldenrod4", {139, 101, 8}},
    Named{"darkgray", {169, 169, 169}}, Named{"darkgreen", {0, 100, 0}},
    Named{"darkgrey", {169, 169, 169}}, Named{"darkkhaki", {189, 183, 107}},
    Named{"darkmagenta", {139, 0, 139}}, Named{"darkolivegreen", {85, 107, 47}},
    Named{"darkolivegreen1", {202, 255, 112}}, Named{"darkolivegreen2", {188, 238, 104}},
    Named{"darkolivegreen3", {162, 205, 90}}, Named{"darkolivegreen4", {110, 139, 61}},
    Named{"darkorange", {255, 140, 0}}, Named{"darkorange1", {255, 127, 0}},
    Named{"darkorange2", {238, 118, 0}}, Named{"darkorange3", {205, 102, 0}},
    Named{"darkorange4", {139, 69, 0}}, Named{"darkorchid", {153, 50, 204}},
    Named{"darkorchid1", {191, 62, 255}}, Named{"darkorchid2", {178, 58, 238}},
    Named{"darkorchid3", {154, 50, 205}}, Named{"darkorchid4", {104, 34, 139}},
    Named{"darkred", {139, 0, 0}}, Named{"darksalmon", {233, 150, 122}},
    Named{"darkseagreen", {143, 188, 143}}, Named{"darkseagreen1", {193, 255, 193}},
    Named{"darkseagreen2", {180, 238, 180}}, Named{"darkseagreen3", {155, 205, 155}},
    Named{"darkseagreen4", {105, 139, 105}}, Named{"darkslateblue", {72, 61, 139}},
    Named{"darkslategray", {47, 79, 79}}, Named{"darkslategray1", {151, 255, 255}},
    Named{"darkslategray2", {141, 238, 238}}, Named{"darkslategray3", {121, 205, 205}},
    Named{"darkslategray4", {82, 139, 139}}, Named{"darkslategrey", {47, 79, 79}},
    Named{"darkturquoise", {0, 206, 209}}, Named{"darkviolet", {148, 0, 211}},
    Named{"deeppink", {255, 20, 147}}, Named{"deeppink1", {255, 20, 147}},
    Named{"deeppink2", {238, 18, 137}}, Named{"deeppink3", {205, 16, 118}},
    Named{"deeppink4", {139, 10, 80}}, Named{"deepskyblue", {0, 191, 255}},
    Named{"deepskyblue1", {0, 191, 255}}, Named{"deepskyblue2", {0, 178, 238}},
    Named{"deepskyblue3", {0, 154, 205}}, Named{"deepskyblue4", {0, 104, 139}},
    Named{"dimgray", {105, 105, 105}}, Named{"dimgrey", {105, 105, 105}},
    Named{"dodgerblue", {30, 144, 255}}, Named{"dodgerblue1", {30, 144, 255}},
    Named{"dodgerblue2", {28, 134, 238}}, Named{"dodgerblue3", {24, 116, 205}},
    Named{"dodgerblue4", {16, 78, 139}}, Named{"firebrick", {178, 34, 34}},
    Named{"firebrick1", {255, 48, 48}}, Named{"firebrick2", {238, 44, 44}},
    Named{"firebrick3", {205, 38, 38}}, Named{"firebrick4", {139, 26, 26}},
    Named{"floralwhite", {255, 250, 240}}, Named{"forestgreen", {34, 139, 34}},
    Named{"fuchsia", {255, 0, 255}}, Named{"gainsboro", {220, 220, 220}},
    Named{"ghostwhite", {248, 248, 255}}, Named{"gold", {255, 215, 0}},
    Named{"gold1", {255, 215, 0}}, Named{"gold2", {238, 201, 0}},
    Named{"gold3", {205, 173, 0}}, Named{"gold4", {139, 117, 0}},
    Named{"goldenrod", {218, 165, 32}}, Named{"goldenrod1", {255, 193, 37}},
    Named{"goldenrod2", {238, 180, 34}}, Named{"goldenrod3", {205, 155, 29}},
    Named{"goldenrod4", {139, 105, 20}}, Named{"gray", {190, 190, 190}},
    Named{"gray0", {0, 0, 0}}, Named{"gray1", {3, 3, 3}},
    Named{"gray10", {26, 26, 26}}, Named{"gray100", {255, 255, 255}},
    Named{"gray11", {28, 28, 28}}, Named{"gray12", {31, 31, 31}},
    Named{"gray13", {33, 33, 33}}, Named{"gray14", {36, 36, 36}},
    Named{"gray15", {38, 38, 38}}, Named{"gray16", {41, 41, 41}},
    Named{"gray17", {43, 43, 43}}, Named{"gray18", {46, 46, 46}},
    Named{"gray19", {48, 48, 48}}, Named{"gray2", {5, 5, 5}},
    Named{"gray20", {51, 51, 51}}, Named{"gray21", {54, 54, 54}},
    Named{"gray22", {56, 56, 56}}, Named{"gray23", {59, 59, 59}},
    Named{"gray24", {61, 61, 61}}, Named{"gray25", {64, 64, 64}},
    Named{"gray26", {66, 66, 66}}, Named{"gray27", {69, 69, 69}},
    Named{"gray28", {71, 71, 71}}, Named{"gray29", {74, 74, 74}},
    Named{"gray3", {8, 8, 8}}, Named{"gray30", {77, 77, 77}},
    Named{"gray31", {79, 79, 79}}, Named{"gray32", {82, 82, 82}},
    Named{"gray33", {84, 84, 84}}, Named{"gray34", {87, 87, 87}},
    Named{"gray35", {89, 89, 89}}, Named{"gray36", {92, 92, 92}},
    Named{"gray37", {94, 94, 94}}, Named{"gray38", {97, 97, 97}},
    Named{"gray39", {99, 99, 99}}, Named{"gray4", {10, 10, 10}},
    Named{"gray40", {102, 102, 102}}, Named{"gray41", {105, 105, 105}},
    Named{"gray42", {107, 107, 107}}, Named{"gray43", {110, 110, 110}},
    Named{"gray44", {112, 112, 112}}, Named{"gray45", {115, 115, 115}},
    Named{"gray46", {117, 117, 117}}, Named{"gray47", {120, 120, 120}},
    Named{"gray48", {122, 122, 122}}, Named{"gray49", {125, 125, 125}},
    Named{"gray5", {13, 13, 13}}, Named{"gray50", {127, 127, 127}},
    Named{"gray51", {130, 130, 130}}, Named{"gray52", {133, 133, 133}},
    Named{"gray53", {135, 135, 135}}, Named{"gray54", {138, 138, 138}},
    Named{"gray55", {140, 140, 140}}, Named{"gray56", {143, 143, 143}},
    Named{"gray57", {145, 145, 145}}, Named{"gray58", {148, 148, 148}},
    Named{"gray59", {150, 150, 150}}, Named{"gray6", {15, 15, 15}},
    Named{"gray60", {153, 153, 153}}, Named{"gray61", {156, 156, 156}},
    Named{"gray62", {158, 158, 158}}, Named{"gray63", {161, 161, 161}},
    Named{"gray64", {163, 163, 163}}, Named{"gray65", {166, 166, 166}},
    Named{"gray66", {168, 168, 168}}, Named{"gray67", {171, 171, 171}},
    Named{"gray68", {173, 173, 173}}, Named{"gray69", {176, 176, 176}},
    Named{"gray7", {18, 18, 18}}, Named{"gray70", {179, 179, 179}},
    Named{"gray71", {181, 181, 181}}, Named{"gray72", {184, 184, 184}},
    Named{"gray73", {186, 186, 186}}, Named{"gray74", {189, 189, 189}},
    Named{"gray75", {191, 191, 191}}, Named{"gray76", {194, 194, 194}},
    Named{"gray77", {196, 196, 196}}, Named{"gray78", {199, 199, 199}},
    Named{"gray79", {201, 201, 201}}, Named{"gray8", {20, 20, 20}},
    Named{"gray80", {204, 204, 204}}, Named{"gray81", {207, 207, 207}},
    Named{"gray82", {209, 209, 209}}, Named{"gray83", {212, 212, 212}},
    Named{"gray84", {214, 214, 214}}, Named{"gray85", {217, 217, 217}},
    Named{"gray86", {219, 219, 219}}, Named{"gray87", {222, 222, 222}},
    Named{"gray88", {224, 224, 224}}, Named{"gray89", {227, 227, 227}},
    Named{"gray9", {23, 23, 23}}, Named{"gray90", {229, 229, 229}},
    Named{"gray91", {232, 232, 232}}, Named{"gray92", {235, 235, 235}},
    Named{"gray93", {237, 237, 237}}, Named{"gray94", {240, 240, 240}},
    Named{"gray95", {242, 242, 242}}, Named{"gray96", {245, 245, 245}},
    Named{"gray97", {247, 247, 247}}, Named{"gray98", {250, 250, 250}},
    Named{"gray99", {252, 252, 252}}, Named{"green", {0, 255, 0}},
    Named{"green1", {0, 255, 0}}, Named{"green2", {0, 238, 0}},
    Named{"green3", {0, 205, 0}}, Named{"green4", {0, 139, 0}},
    Named{"greenyellow", {173, 255, 47}}, Named{"grey", {190, 190, 190}},
    Named{"grey0", {0, 0, 0}}, Named{"grey1", {3, 3, 3}},
    Named{"grey10", {26, 26, 26}}, Named{"grey100", {255, 255, 255}},
    Named{"grey11", {28, 28, 28}}, Named{"grey12", {31, 31, 31}},
    Named{"grey13", {33, 33, 33}}, Named{"grey14", {36, 36, 36}},
    Named{"grey15", {38, 38, 38}}, Named{"grey16", {41, 41, 41}},
    Named{"grey17", {43, 43, 43}}, Named{"grey18", {46, 46, 46}},
    Named{"grey19", {48, 48, 48}}, Named{"grey2", {5, 5, 5}},
    Named{"grey20", {51, 51, 51}}, Named{"grey21", {54, 54, 54}},
    Named{"grey22", {56, 56, 56}}, Named{"grey23", {59, 59, 59}},
    Named{"grey24", {61, 61, 61}}, Named{"grey25", {64, 64, 64}},
    Named{"grey26", {66, 66, 66}}, Named{"grey27", {69, 69, 69}},
    Named{"grey28", {71, 71, 71}}, Named{"grey29", {74, 74, 74}},
    Named{"grey3", {8, 8, 8}}, Named{"grey30", {77, 77, 77}},
    Named{"grey31", {79, 79, 79}}, Named{"grey32", {82, 82, 82}},
    Named{"grey33", {84, 84, 84}}, Named{"grey34", {87, 87, 87}},
    Named{"grey35", {89, 89, 89}}, Named{"grey36", {92, 92, 92}},
    Named{"grey37", {94, 94, 94}}, Named{"grey38", {97, 97, 97}},
    Named{"grey39", {99, 99, 99}}, Named{"grey4", {10, 10, 10}},
    Named{"grey40", {102, 102, 102}}, Named{"grey41", {105, 105, 105}},
    Named{"grey42", {107, 107, 107}}, Named{"grey43", {110, 110, 110}},
    Named{"grey44", {112, 112, 112}}, Named{"grey45", {115, 115, 115}},
    Named{"grey46", {117, 117, 117}}, Named{"grey47", {120, 120, 120}},
    Named{"grey48", {122, 122, 122}}, Named{"grey49", {125, 125, 125}},
    Named{"grey5", {13, 13, 13}}, Named{"grey50", {127, 127, 127}},
    Named{"grey51", {130, 130, 130}}, Named{"grey52", {133, 133, 133}},
    Named{"grey53", {135, 135, 135}}, Named{"grey54", {138, 138, 138}},
    Named{"grey55", {140, 140, 140}}, Named{"grey56", {143, 143, 143}},
    Named{"grey57", {145, 145, 145}}, Named{"grey58", {148, 148, 148}},
    Named{"grey59", {150, 150, 150}}, Named{"grey6", {15, 15, 15}},
    Named{"grey60", {153, 153, 153}}, Named{"grey61", {156, 156, 156}},
    Named{"grey62", {158, 158, 158}}, Named{"grey63", {161, 161, 161}},
    Named{"grey64", {163, 163, 163}}, Named{"grey65", {166, 166, 166}},
    Named{"grey66", {168, 168, 168}}, Named{"grey67", {171, 171, 171}},
    Named{"grey68", {173, 173, 173}}, Named{"grey69", {176, 176, 176}},
    Named{"grey7", {18, 18, 18}}, Named{"grey70", {179, 179, 179}},
    Named{"grey71", {181, 181, 181}}, Named{"grey72", {184, 184, 184}},
    Named{"grey73", {186, 186, 186}}, Named{"grey74", {189, 189, 189}},
    Named{"grey75", {191, 191, 191}}, Named{"grey76", {194, 194, 194}},
    Named{"grey77", {196, 196, 196}}, Named{"grey78", {199, 199, 199}},
    Named{"grey79", {201, 201, 201}}, Named{"grey8", {20, 20, 20}},
    Named{"grey80", {204, 204, 204}}, Named{"grey81", {207, 207, 207}},
    Named{"grey82", {209, 209, 209}}, Named{"grey83", {212, 212, 212}},
    Named{"grey84", {214, 214, 214}}, Named{"grey85", {217, 217, 217}},
    Named{"grey86", {219, 219, 219}}, Named{"grey87", {222, 222, 222}},
    Named{"grey88", {224, 224, 224}}, Named{"grey89", {227, 227, 227}},
    Named{"grey9", {23, 23, 23}}, Named{"grey90", {229, 229, 229}},
    Named{"grey91", {232, 232, 232}}, Named{"grey92", {235, 235, 235}},
    Named{"grey93", {237, 237, 237}}, Named{"grey94", {240, 240, 240}},
    Named{"grey95", {242, 242, 242}}, Named{"grey96", {245, 245, 245}},
    Named{"grey97", {247, 247, 247}}, Named{"grey98", {250, 250, 250}},
    Named{"grey99", {252, 252, 252}}, Named{"honeydew", {240, 255, 240}},
    Named{"honeydew1", {240, 255, 240}}, Named{"honeydew2", {224, 238, 224}},
    Named{"honeydew3", {193, 205, 193}}, Named{"honeydew4", {131, 139, 131}},
    Named{"hotpink", {255, 105, 180}}, Named{"hotpink1", {255, 110, 180}},
    Named{"hotpink2", {238, 106, 167}}, Named{"hotpink3", {205, 96, 144}},
    Named{"hotpink4", {139, 58, 98}}, Named{"indianred", {205, 92, 92}},
    Named{"indianred1", {255, 106, 106}}, Named{"indianred2", {238, 99, 99}},
    Named{"indianred3", {205, 85, 85}}, Named{"indianred4", {139, 58, 58}},
    Named{"indigo", {75, 0, 130}}, Named{"ivory", {255, 255, 240}},
    Named{"ivory1", {255, 255, 240}}, Named{"ivory2", {238, 238, 224}},
    Named{"ivory3", {205, 205, 193}}, Named{"ivory4", {139, 139, 131}},
    Named{"khaki", {240, 230, 140}}, Named{"khaki1", {255, 246, 143}},
    Named{"khaki2", {238, 230, 133}}, Named{"khaki3", {205, 198, 115}},
    Named{"khaki4", {139, 134, 78}}, Named{"lavender", {230, 230, 250}},
    Named{"lavenderblush", {255, 240, 245}}, Named{"lavenderblush1", {255, 240, 245}},
    Named{"lavenderblush2", {238, 224, 229}}, Named{"lavenderblush3", {205, 193, 197}},
    Named{"lavenderblush4", {139, 131, 134}}, Named{"lawngreen", {124, 252, 0}},
    Named{"lemonchiffon", {255, 250, 205}}, Named{"lemonchiffon1", {255, 250, 205}},
    Named{"lemonchiffon2", {238, 233, 191}}, Named{"lemonchiffon3", {205, 201, 165}},
    Named{"lemonchiffon4", {139, 137, 112}}, Named{"lightblue", {173, 216, 230}},
    Named{"lightblue1", {191, 239, 255}}, Named{"lightblue2", {178, 223, 238}},
    Named{"lightblue3", {154, 192, 205}}, Named{"lightblue4", {104, 131, 139}},
    Named{"lightcoral", {240, 128, 128}}, Named{"lightcyan", {224, 255, 255}},
    Named{"lightcyan1", {224, 255, 255}}, Named{"lightcyan2", {209, 238, 238}},
    Named{"lightcyan3", {180, 205, 205}}, Named{"lightcyan4", {122, 139, 139}},
    Named{"lightgoldenrod", {238, 221, 130}}, Named{"lightgoldenrod1", {255, 236, 139}},
    Named{"lightgoldenrod2", {238, 220, 130}}, Named{"lightgoldenrod3", {205, 190, 112}},
    Named{"lightgoldenrod4", {139, 129, 76}}, Named{"lightgoldenrodyellow", {250, 250, 210}},
    Named{"lightgray", {211, 211, 211}}, Named{"lightgreen", {144, 238, 144}},
    Named{"lightgrey", {211, 211, 211}}, Named{"lightpink", {255, 182, 193}},
    Named{"lightpink1", {255, 174, 185}}, Named{"lightpink2", {238, 162, 173}},
    Named{"lightpink3", {205, 140, 149}}, Named{"lightpink4", {139, 95, 101}},
    Named{"lightsalmon", {255, 160, 122}}, Named{"lightsalmon1", {255, 160, 122}},
    Named{"lightsalmon2", {238, 149, 114}}, Named{"lightsalmon3", {205, 129, 98}},
    Named{"lightsalmon4", {139, 87, 66}}, Named{"lightseagreen", {32, 178, 170}},
    Named{"lightskyblue", {135, 206, 250}}, Named{"lightskyblue1", {176, 226, 255}},
    Named{"lightskyblue2", {164, 211, 238}}, Named{"lightskyblue3", {141, 182, 205}},
    Named{"lightskyblue4", {96, 123, 139}}, Named{"lightslateblue", {132, 112, 255}},
    Named{"lightslategray", {119, 136, 153}}, Named{"lightslategrey", {119, 136, 153}},
    Named{"lightsteelblue", {176, 196, 222}}, Named{"lightsteelblue1", {202, 225, 255}},
    Named{"lightsteelblue2", {188, 210, 238}}, Named{"lightsteelblue3", {162, 181, 205}},
    Named{"lightsteelblue4", {110, 123, 139}}, Named{"lightyellow", {255, 255, 224}},
    Named{"lightyellow1", {255, 255, 224}}, Named{"lightyellow2", {238, 238, 209}},
    Named{"lightyellow3", {205, 205, 180}}, Named{"lightyellow4", {139, 139, 122}},
    Named{"lime", {0, 255, 0}}, Named{"limegreen", {50, 205, 50}},
    Named{"linen", {250, 240, 230}}, Named{"magenta", {255, 0, 255}},
    Named{"magenta1", {255, 0, 255}}, Named{"magenta2", {238, 0, 238}},
    Named{"magenta3", {205, 0, 205}}, Named{"magenta4", {139, 0, 139}},
    Named{"maroon", {176, 48, 96}}, Named{"maroon1", {255, 52, 179}},
    Named{"maroon2", {238, 48, 167}}, Named{"maroon3", {205, 41, 144}},
    Named{"maroon4", {139, 28, 98}}, Named{"mediumaquamarine", {102, 205, 170}},
    Named{"mediumblue", {0, 0, 205}}, Named{"mediumorchid", {186, 85, 211}},
    Named{"mediumorchid1", {224, 102, 255}}, Named{"mediumorchid2", {209, 95, 238}},
    Named{"mediumorchid3", {180, 82, 205}}, Named{"mediumorchid4", {122, 55, 139}},
    Named{"mediumpurple", {147, 112, 219}}, Named{"mediumpurple1", {171, 130, 255}},
    Named{"mediumpurple2", {159, 121, 238}}, Named{"mediumpurple3", {137, 104, 205}},
    Named{"mediumpurple4", {93, 71, 139}}, Named{"mediumseagreen", {60, 179, 113}},
    Named{"mediumslateblue", {123, 104, 238}}, Named{"mediumspringgreen", {0, 250, 154}},
    Named{"mediumturquoise", {72, 209, 204}}, Named{"mediumvioletred", {199, 21, 133}},
    Named{"midnightblue", {25, 25, 112}}, Named{"mintcream", {245, 255, 250}},
    Named{"mistyrose", {255, 228, 225}}, Named{"mistyrose1", {255, 228, 225}},
    Named{"mistyrose2", {238, 213, 210}}, Named{"mistyrose3", {205, 183, 181}},
    Named{"mistyrose4", {139, 125, 123}}, Named{"moccasin", {255, 228, 181}},
    Named{"navajowhite", {255, 222, 173}}, Named{"navajowhite1", {255, 222, 173}},
    Named{"navajowhite2", {238, 207, 161}}, Named{"navajowhite3", {205, 179, 139}},
    Named{"navajowhite4", {139, 121, 94}}, Named{"navy", {0, 0, 128}},
    Named{"navyblue", {0, 0, 128}}, Named{"oldlace", {253, 245, 230}},
    Named{"olive", {128, 128, 0}}, Named{"olivedrab", {107, 142, 35}},
    Named{"olivedrab1", {192, 255, 62}}, Named{"olivedrab2", {179, 238, 58}},
    Named{"olivedrab3", {154, 205, 50}}, Named{"olivedrab4", {105, 139, 34}},
    Named{"orange", {255, 165, 0}}, Named{"orange1", {255, 165, 0}},
    Named{"orange2", {238, 154, 0}}, Named{"orange3", {205, 133, 0}},
    Named{"orange4", {139, 90, 0}}, Named{"orangered", {255, 69, 0}},
    Named{"orangered1", {255, 69, 0}}, Named{"orangered2", {238, 64, 0}},
    Named{"orangered3", {205, 55, 0}}, Named{"orangered4", {139, 37, 0}},
    Named{"orchid", {218, 112, 214}}, Named{"orchid1", {255, 131, 250}},
    Named{"orchid2", {238, 122, 233}}, Named{"orchid3", {205, 105, 201}},
    Named{"orchid4", {139, 71, 137}}, Named{"palegoldenrod", {238, 232, 170}},
    Named{"palegreen", {152, 251, 152}}, Named{"palegreen1", {154, 255, 154}},
    Named{"palegreen2", {144, 238, 144}}, Named{"palegreen3", {124, 205, 124}},
    Named{"palegreen4", {84, 139, 84}}, Named{"paleturquoise", {175, 238, 238}},
    Named{"paleturquoise1", {187, 255, 255}}, Named{"paleturquoise2", {174, 238, 238}},
    Named{"paleturquoise3", {150, 205, 205}}, Named{"paleturquoise4", {102, 139, 139}},
    Named{"palevioletred", {219, 112, 147}}, Named{"palevioletred1", {255, 130, 171}},
    Named{"palevioletred2", {238, 121, 159}}, Named{"palevioletred3", {205, 104, 137}},
    Named{"palevioletred4", {139, 71, 93}}, Named{"papayawhip", {255, 239, 213}},
    Named{"peachpuff", {255, 218, 185}}, Named{"peachpuff1", {255, 218, 185}},
    Named{"peachpuff2", {238, 203, 173}}, Named{"peachpuff3", {205, 175, 149}},
    Named{"peachpuff4", {139, 119, 101}}, Named{"peru", {205, 133, 63}},
    Named{"pink", {255, 192, 203}}, Named{"pink1", {255, 181, 197}},
    Named{"pink2", {238, 169, 184}}, Named{"pink3", {205, 145, 158}},
    Named{"pink4", {139, 99, 108}}, Named{"plum", {221, 160, 221}},
    Named{"plum1", {255, 187, 255}}, Named{"plum2", {238, 174, 238}},
    Named{"plum3", {205, 150, 205}}, Named{"plum4", {139, 102, 139}},
    Named{"powderblue", {176, 224, 230}}, Named{"purple", {160, 32, 240}},
    Named{"purple1", {155, 48, 255}}, Named{"purple2", {145, 44, 238}},
    Named{"purple3", {125, 38, 205}}, Named{"purple4", {85, 26, 139}},
    Named{"rebeccapurple", {102, 51, 153}}, Named{"red", {255, 0, 0}},
    Named{"red1", {255, 0, 0}}, Named{"red2", {238, 0, 0}},
    Named{"red3", {205, 0, 0}}, Named{"red4", {139, 0, 0}},
    Named{"rosybrown", {188, 143, 143}}, Named{"rosybrown1", {255, 193, 193}},
    Named{"rosybrown2", {238, 180, 180}}, Named{"rosybrown3", {205, 155, 155}},
    Named{"rosybrown4", {139, 105, 105}}, Named{"royalblue", {65, 105, 225}},
    Named{"royalblue1", {72, 118, 255}}, Named{"royalblue2", {67, 110, 238}},
    Named{"royalblue3", {58, 95, 205}}, Named{"royalblue4", {39, 64, 139}},
    Named{"saddlebrown", {139, 69, 19}}, Named{"salmon", {250, 128, 114}},
    Named{"salmon1", {255, 140, 105}}, Named{"salmon2", {238, 130, 98}},
    Named{"salmon3", {205, 112, 84}}, Named{"salmon4", {139, 76, 57}},
    Named{"sandybrown", {244, 164, 96}}, Named{"seagreen", {46, 139, 87}},
    Named{"seagreen1", {84, 255, 159}}, Named{"seagreen2", {78, 238, 148}},
    Named{"seagreen3", {67, 205, 128}}, Named{"seagreen4", {46, 139, 87}},
    Named{"seashell", {255, 245, 238}}, Named{"seashell1", {255, 245, 238}},
    Named{"seashell2", {238, 229, 222}}, Named{"seashell3", {205, 197, 191}},
    Named{"seashell4", {139, 134, 130}}, Named{"sienna", {160, 82, 45}},
    Named{"sienna1", {255, 130, 71}}, Named{"sienna2", {238, 121, 66}},
    Named{"sienna3", {205, 104, 57}}, Named{"sienna4", {139, 71, 38}},
    Named{"silver", {192, 192, 192}}, Named{"skyblue", {135, 206, 235}},
    Named{"skyblue1", {135, 206, 255}}, Named{"skyblue2", {126, 192, 238}},
    Named{"skyblue3", {108, 166, 205}}, Named{"skyblue4", {74, 112, 139}},
    Named{"slateblue", {106, 90, 205}}, Named{"slateblue1", {131, 111, 255}},
    Named{"slateblue2", {122, 103, 238}}, Named{"slateblue3", {105, 89, 205}},
    Named{"slateblue4", {71, 60, 139}}, Named{"slategray", {112, 128, 144}},
    Named{"slategray1", {198, 226, 255}}, Named{"slategray2", {185, 211, 238}},
    Named{"slategray3", {159, 182, 205}}, Named{"slategray4", {108, 123, 139}},
    Named{"slategrey", {112, 128, 144}}, Named{"snow", {255, 250, 250}},
    Named{"snow1", {255, 250, 250}}, Named{"snow2", {238, 233, 233}},
    Named{"snow3", {205, 201, 201}}, Named{"snow4", {139, 137, 137}},
    Named{"springgreen", {0, 255, 127}}, Named{"springgreen1", {0, 255, 127}},
    Named{"springgreen2", {0, 238, 118}}, Named{"springgreen3", {0, 205, 102}},
    Named{"springgreen4", {0, 139, 69}}, Named{"steelblue", {70, 130, 180}},
    Named{"steelblue1", {99, 184, 255}}, Named{"steelblue2", {92, 172, 238}},
    Named{"steelblue3", {79, 148, 205}}, Named{"steelblue4", {54, 100, 139}},
    Named{"tan", {210, 180, 140}}, Named{"tan1", {255, 165, 79}},
    Named{"tan2", {238, 154, 73}}, Named{"tan3", {205, 133, 63}},
    Named{"tan4", {139, 90, 43}}, Named{"teal", {0, 128, 128}},
    Named{"thistle", {216, 191, 216}}, Named{"thistle1", {255, 225, 255}},
    Named{"thistle2", {238, 210, 238}}, Named{"thistle3", {205, 181, 205}},
    Named{"thistle4", {139, 123, 139}}, Named{"tomato", {255, 99, 71}},
    Named{"tomato1", {255, 99, 71}}, Named{"tomato2", {238, 92, 66}},
    Named{"tomato3", {205, 79, 57}}, Named{"tomato4", {139, 54, 38}},
    Named{"turquoise", {64, 224, 208}}, Named{"turquoise1", {0, 245, 255}},
    Named{"turquoise2", {0, 229, 238}}, Named{"turquoise3", {0, 197, 205}},
    Named{"turquoise4", {0, 134, 139}}, Named{"violet", {238, 130, 238}},
    Named{"violetred", {208, 32, 144}}, Named{"violetred1", {255, 62, 150}},
    Named{"violetred2", {238, 58, 140}}, Named{"violetred3", {205, 50, 120}},
    Named{"violetred4", {139, 34, 82}}, Named{"webgray", {128, 128, 128}},
    Named{"webgreen", {0, 128, 0}}, Named{"webgrey", {128, 128, 128}},
    Named{"webmaroon", {128, 0, 0}}, Named{"webpurple", {128, 0, 128}},
    Named{"wheat", {245, 222, 179}}, Named{"wheat1", {255, 231, 186}},
    Named{"wheat2", {238, 216, 174}}, Named{"wheat3", {205, 186, 150}},
    Named{"wheat4", {139, 126, 102}}, Named{"white", {255, 255, 255}},
    Named{"whitesmoke", {245, 245, 245}}, Named{"x11gray", {190, 190, 190}},
    Named{"x11green", {0, 255, 0}}, Named{"x11grey", {190, 190, 190}},
    Named{"x11maroon", {176, 48, 96}}, Named{"x11purple", {160, 32, 240}},
    Named{"yellow", {255, 255, 0}}, Named{"yellow1", {255, 255, 0}},
    Named{"yellow2", {238, 238, 0}}, Named{"yellow3", {205, 205, 0}},
    Named{"yellow4", {139, 139, 0}}, Named{"yellowgreen", {154, 205, 50}},
}};

[[nodiscard]] inline auto linear_component(std::uint8_t value) noexcept -> double {
    const auto srgb = static_cast<double>(value) / 255.0;
    return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}

}  // namespace detail

/** Get an RGB value from the default 256-color palette. */
[[nodiscard]] constexpr auto from_256(std::uint8_t index) noexcept -> Rgb {
    return detail::palette_256(index);
}

/** Get an RGB value from the default ANSI palette using the low four bits. */
[[nodiscard]] constexpr auto from_16(std::uint8_t index) noexcept -> Rgb {
    return detail::ansi_16[index & 0x0FU];
}

/** Approximate perceptual distance between two sRGB colors. */
[[nodiscard]] inline auto distance(const Rgb lhs, const Rgb rhs) noexcept -> double {
    const auto red = static_cast<double>(lhs.red) - static_cast<double>(rhs.red);
    const auto green = static_cast<double>(lhs.green) - static_cast<double>(rhs.green);
    const auto blue = static_cast<double>(lhs.blue) - static_cast<double>(rhs.blue);
    // Weighted Euclidean distance approximates relative luminance sensitivity.
    return std::sqrt(0.299 * red * red + 0.587 * green * green + 0.114 * blue * blue);
}

/** Quantize to the nearest entry of the default 256-color palette. */
[[nodiscard]] inline auto to_256(const Rgb rgb) noexcept -> std::uint8_t {
    std::uint8_t nearest = 0;
    auto nearest_distance = distance(rgb, detail::palette_256(0));
    for (unsigned int index = 1; index < 256; ++index) {
        const auto candidate = distance(rgb, detail::palette_256(static_cast<std::uint8_t>(index)));
        if (candidate < nearest_distance) {
            nearest = static_cast<std::uint8_t>(index);
            nearest_distance = candidate;
        }
    }
    return nearest;
}

/** Quantize to the nearest entry of the default ANSI palette. */
[[nodiscard]] inline auto to_16(const Rgb rgb) noexcept -> std::uint8_t {
    std::uint8_t nearest = 0;
    auto nearest_distance = distance(rgb, detail::ansi_16[0]);
    for (std::size_t index = 1; index < detail::ansi_16.size(); ++index) {
        const auto candidate = distance(rgb, detail::ansi_16[index]);
        if (candidate < nearest_distance) {
            nearest = static_cast<std::uint8_t>(index);
            nearest_distance = candidate;
        }
    }
    return nearest;
}

/** Compute linear-light relative luminance on [0, 1] for an sRGB color. */
[[nodiscard]] inline auto luminance(const Rgb rgb) noexcept -> double {
    return 0.2126 * detail::linear_component(rgb.red) +
           0.7152 * detail::linear_component(rgb.green) +
           0.0722 * detail::linear_component(rgb.blue);
}

/** Parse an X11 color name, a `#` hexadecimal color, or `rgb:R/G/B`. */
[[nodiscard]] inline auto parse(const std::string_view value) noexcept -> std::optional<Rgb> {
    const auto rgb = [](int r, int g, int b) -> std::optional<Rgb> {
        if (r < 0 || g < 0 || b < 0) return std::nullopt;
        return Rgb{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                   static_cast<std::uint8_t>(b)};
    };
    if (value.starts_with('#')) {
        const auto digits = value.substr(1);
        if (digits.size() % 3 != 0 || digits.empty() || digits.size() > 12) return std::nullopt;
        const auto size = digits.size() / 3;
        return rgb(detail::hex_component(digits.substr(0, size), false),
                   detail::hex_component(digits.substr(size, size), false),
                   detail::hex_component(digits.substr(2 * size), false));
    }
    if (value.starts_with("rgb:")) {
        const auto digits = value.substr(4);
        const auto first = digits.find('/');
        if (first == std::string_view::npos) return std::nullopt;
        const auto second = digits.find('/', first + 1);
        if (second == std::string_view::npos) return std::nullopt;
        return rgb(detail::hex_component(digits.substr(0, first), true),
                   detail::hex_component(digits.substr(first + 1, second - first - 1), true),
                   detail::hex_component(digits.substr(second + 1), true));
    }
    std::array<char, 32> normalized{};
    std::size_t size = 0;
    for (auto ch : value) {
        if (ch == ' ' || ch == '\t') continue;
        if (size == normalized.size()) return std::nullopt;
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        normalized[size++] = ch;
    }
    const std::string_view key{normalized.data(), size};
    const auto found = std::lower_bound(detail::names.begin(), detail::names.end(), key,
        [](const detail::Named& entry, std::string_view name) { return entry.name < name; });
    if (found != detail::names.end() && found->name == key) return found->rgb;
    return std::nullopt;
}

}  // namespace ttytk::color

/*
Copyright 1985, 1989, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from The Open Group.

Copyright (c) 1994, 2008, Oracle and/or its affiliates. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/
