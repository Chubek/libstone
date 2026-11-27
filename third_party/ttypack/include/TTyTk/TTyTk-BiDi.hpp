#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Encoding.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ttytk::bidi {

/** A Unicode bidirectional embedding level; odd is right-to-left. */
using Level = std::uint8_t;

namespace detail {

enum class Type : std::uint8_t {
    L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON, LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI,
};

struct Range { char32_t first, last; Type type; };
struct Bracket { char32_t scalar, mate; bool opening; };
struct Mirror { char32_t scalar, mate; };

// Unicode 16.0.0 DerivedBidiClass.txt, BidiBrackets.txt and BidiMirroring.txt.
// See the Unicode data license notice at the end of this file.
inline constexpr std::array<Range, 745> types = {{
    Range{0x0000, 0x0008, Type::BN}, Range{0x0009, 0x0009, Type::S},
    Range{0x000A, 0x000A, Type::B}, Range{0x000B, 0x000B, Type::S},
    Range{0x000C, 0x000C, Type::WS}, Range{0x000D, 0x000D, Type::B},
    Range{0x000E, 0x001B, Type::BN}, Range{0x001C, 0x001E, Type::B},
    Range{0x001F, 0x001F, Type::S}, Range{0x0020, 0x0020, Type::WS},
    Range{0x0021, 0x0022, Type::ON}, Range{0x0023, 0x0025, Type::ET},
    Range{0x0026, 0x002A, Type::ON}, Range{0x002B, 0x002B, Type::ES},
    Range{0x002C, 0x002C, Type::CS}, Range{0x002D, 0x002D, Type::ES},
    Range{0x002E, 0x002F, Type::CS}, Range{0x0030, 0x0039, Type::EN},
    Range{0x003A, 0x003A, Type::CS}, Range{0x003B, 0x0040, Type::ON},
    Range{0x005B, 0x0060, Type::ON}, Range{0x007B, 0x007E, Type::ON},
    Range{0x007F, 0x0084, Type::BN}, Range{0x0085, 0x0085, Type::B},
    Range{0x0086, 0x009F, Type::BN}, Range{0x00A0, 0x00A0, Type::CS},
    Range{0x00A1, 0x00A1, Type::ON}, Range{0x00A2, 0x00A5, Type::ET},
    Range{0x00A6, 0x00A9, Type::ON}, Range{0x00AB, 0x00AC, Type::ON},
    Range{0x00AD, 0x00AD, Type::BN}, Range{0x00AE, 0x00AF, Type::ON},
    Range{0x00B0, 0x00B1, Type::ET}, Range{0x00B2, 0x00B3, Type::EN},
    Range{0x00B4, 0x00B4, Type::ON}, Range{0x00B6, 0x00B8, Type::ON},
    Range{0x00B9, 0x00B9, Type::EN}, Range{0x00BB, 0x00BF, Type::ON},
    Range{0x00D7, 0x00D7, Type::ON}, Range{0x00F7, 0x00F7, Type::ON},
    Range{0x02B9, 0x02BA, Type::ON}, Range{0x02C2, 0x02CF, Type::ON},
    Range{0x02D2, 0x02DF, Type::ON}, Range{0x02E5, 0x02ED, Type::ON},
    Range{0x02EF, 0x02FF, Type::ON}, Range{0x0300, 0x036F, Type::NSM},
    Range{0x0374, 0x0375, Type::ON}, Range{0x037E, 0x037E, Type::ON},
    Range{0x0384, 0x0385, Type::ON}, Range{0x0387, 0x0387, Type::ON},
    Range{0x03F6, 0x03F6, Type::ON}, Range{0x0483, 0x0489, Type::NSM},
    Range{0x058A, 0x058A, Type::ON}, Range{0x058D, 0x058E, Type::ON},
    Range{0x058F, 0x058F, Type::ET}, Range{0x0590, 0x0590, Type::R},
    Range{0x0591, 0x05BD, Type::NSM}, Range{0x05BE, 0x05BE, Type::R},
    Range{0x05BF, 0x05BF, Type::NSM}, Range{0x05C0, 0x05C0, Type::R},
    Range{0x05C1, 0x05C2, Type::NSM}, Range{0x05C3, 0x05C3, Type::R},
    Range{0x05C4, 0x05C5, Type::NSM}, Range{0x05C6, 0x05C6, Type::R},
    Range{0x05C7, 0x05C7, Type::NSM}, Range{0x05C8, 0x05FF, Type::R},
    Range{0x0600, 0x0605, Type::AN}, Range{0x0606, 0x0607, Type::ON},
    Range{0x0608, 0x0608, Type::AL}, Range{0x0609, 0x060A, Type::ET},
    Range{0x060B, 0x060B, Type::AL}, Range{0x060C, 0x060C, Type::CS},
    Range{0x060D, 0x060D, Type::AL}, Range{0x060E, 0x060F, Type::ON},
    Range{0x0610, 0x061A, Type::NSM}, Range{0x061B, 0x064A, Type::AL},
    Range{0x064B, 0x065F, Type::NSM}, Range{0x0660, 0x0669, Type::AN},
    Range{0x066A, 0x066A, Type::ET}, Range{0x066B, 0x066C, Type::AN},
    Range{0x066D, 0x066F, Type::AL}, Range{0x0670, 0x0670, Type::NSM},
    Range{0x0671, 0x06D5, Type::AL}, Range{0x06D6, 0x06DC, Type::NSM},
    Range{0x06DD, 0x06DD, Type::AN}, Range{0x06DE, 0x06DE, Type::ON},
    Range{0x06DF, 0x06E4, Type::NSM}, Range{0x06E5, 0x06E6, Type::AL},
    Range{0x06E7, 0x06E8, Type::NSM}, Range{0x06E9, 0x06E9, Type::ON},
    Range{0x06EA, 0x06ED, Type::NSM}, Range{0x06EE, 0x06EF, Type::AL},
    Range{0x06F0, 0x06F9, Type::EN}, Range{0x06FA, 0x0710, Type::AL},
    Range{0x0711, 0x0711, Type::NSM}, Range{0x0712, 0x072F, Type::AL},
    Range{0x0730, 0x074A, Type::NSM}, Range{0x074B, 0x07A5, Type::AL},
    Range{0x07A6, 0x07B0, Type::NSM}, Range{0x07B1, 0x07BF, Type::AL},
    Range{0x07C0, 0x07EA, Type::R}, Range{0x07EB, 0x07F3, Type::NSM},
    Range{0x07F4, 0x07F5, Type::R}, Range{0x07F6, 0x07F9, Type::ON},
    Range{0x07FA, 0x07FC, Type::R}, Range{0x07FD, 0x07FD, Type::NSM},
    Range{0x07FE, 0x0815, Type::R}, Range{0x0816, 0x0819, Type::NSM},
    Range{0x081A, 0x081A, Type::R}, Range{0x081B, 0x0823, Type::NSM},
    Range{0x0824, 0x0824, Type::R}, Range{0x0825, 0x0827, Type::NSM},
    Range{0x0828, 0x0828, Type::R}, Range{0x0829, 0x082D, Type::NSM},
    Range{0x082E, 0x0858, Type::R}, Range{0x0859, 0x085B, Type::NSM},
    Range{0x085C, 0x085F, Type::R}, Range{0x0860, 0x088F, Type::AL},
    Range{0x0890, 0x0891, Type::AN}, Range{0x0892, 0x0896, Type::AL},
    Range{0x0897, 0x089F, Type::NSM}, Range{0x08A0, 0x08C9, Type::AL},
    Range{0x08CA, 0x08E1, Type::NSM}, Range{0x08E2, 0x08E2, Type::AN},
    Range{0x08E3, 0x0902, Type::NSM}, Range{0x093A, 0x093A, Type::NSM},
    Range{0x093C, 0x093C, Type::NSM}, Range{0x0941, 0x0948, Type::NSM},
    Range{0x094D, 0x094D, Type::NSM}, Range{0x0951, 0x0957, Type::NSM},
    Range{0x0962, 0x0963, Type::NSM}, Range{0x0981, 0x0981, Type::NSM},
    Range{0x09BC, 0x09BC, Type::NSM}, Range{0x09C1, 0x09C4, Type::NSM},
    Range{0x09CD, 0x09CD, Type::NSM}, Range{0x09E2, 0x09E3, Type::NSM},
    Range{0x09F2, 0x09F3, Type::ET}, Range{0x09FB, 0x09FB, Type::ET},
    Range{0x09FE, 0x09FE, Type::NSM}, Range{0x0A01, 0x0A02, Type::NSM},
    Range{0x0A3C, 0x0A3C, Type::NSM}, Range{0x0A41, 0x0A42, Type::NSM},
    Range{0x0A47, 0x0A48, Type::NSM}, Range{0x0A4B, 0x0A4D, Type::NSM},
    Range{0x0A51, 0x0A51, Type::NSM}, Range{0x0A70, 0x0A71, Type::NSM},
    Range{0x0A75, 0x0A75, Type::NSM}, Range{0x0A81, 0x0A82, Type::NSM},
    Range{0x0ABC, 0x0ABC, Type::NSM}, Range{0x0AC1, 0x0AC5, Type::NSM},
    Range{0x0AC7, 0x0AC8, Type::NSM}, Range{0x0ACD, 0x0ACD, Type::NSM},
    Range{0x0AE2, 0x0AE3, Type::NSM}, Range{0x0AF1, 0x0AF1, Type::ET},
    Range{0x0AFA, 0x0AFF, Type::NSM}, Range{0x0B01, 0x0B01, Type::NSM},
    Range{0x0B3C, 0x0B3C, Type::NSM}, Range{0x0B3F, 0x0B3F, Type::NSM},
    Range{0x0B41, 0x0B44, Type::NSM}, Range{0x0B4D, 0x0B4D, Type::NSM},
    Range{0x0B55, 0x0B56, Type::NSM}, Range{0x0B62, 0x0B63, Type::NSM},
    Range{0x0B82, 0x0B82, Type::NSM}, Range{0x0BC0, 0x0BC0, Type::NSM},
    Range{0x0BCD, 0x0BCD, Type::NSM}, Range{0x0BF3, 0x0BF8, Type::ON},
    Range{0x0BF9, 0x0BF9, Type::ET}, Range{0x0BFA, 0x0BFA, Type::ON},
    Range{0x0C00, 0x0C00, Type::NSM}, Range{0x0C04, 0x0C04, Type::NSM},
    Range{0x0C3C, 0x0C3C, Type::NSM}, Range{0x0C3E, 0x0C40, Type::NSM},
    Range{0x0C46, 0x0C48, Type::NSM}, Range{0x0C4A, 0x0C4D, Type::NSM},
    Range{0x0C55, 0x0C56, Type::NSM}, Range{0x0C62, 0x0C63, Type::NSM},
    Range{0x0C78, 0x0C7E, Type::ON}, Range{0x0C81, 0x0C81, Type::NSM},
    Range{0x0CBC, 0x0CBC, Type::NSM}, Range{0x0CCC, 0x0CCD, Type::NSM},
    Range{0x0CE2, 0x0CE3, Type::NSM}, Range{0x0D00, 0x0D01, Type::NSM},
    Range{0x0D3B, 0x0D3C, Type::NSM}, Range{0x0D41, 0x0D44, Type::NSM},
    Range{0x0D4D, 0x0D4D, Type::NSM}, Range{0x0D62, 0x0D63, Type::NSM},
    Range{0x0D81, 0x0D81, Type::NSM}, Range{0x0DCA, 0x0DCA, Type::NSM},
    Range{0x0DD2, 0x0DD4, Type::NSM}, Range{0x0DD6, 0x0DD6, Type::NSM},
    Range{0x0E31, 0x0E31, Type::NSM}, Range{0x0E34, 0x0E3A, Type::NSM},
    Range{0x0E3F, 0x0E3F, Type::ET}, Range{0x0E47, 0x0E4E, Type::NSM},
    Range{0x0EB1, 0x0EB1, Type::NSM}, Range{0x0EB4, 0x0EBC, Type::NSM},
    Range{0x0EC8, 0x0ECE, Type::NSM}, Range{0x0F18, 0x0F19, Type::NSM},
    Range{0x0F35, 0x0F35, Type::NSM}, Range{0x0F37, 0x0F37, Type::NSM},
    Range{0x0F39, 0x0F39, Type::NSM}, Range{0x0F3A, 0x0F3D, Type::ON},
    Range{0x0F71, 0x0F7E, Type::NSM}, Range{0x0F80, 0x0F84, Type::NSM},
    Range{0x0F86, 0x0F87, Type::NSM}, Range{0x0F8D, 0x0F97, Type::NSM},
    Range{0x0F99, 0x0FBC, Type::NSM}, Range{0x0FC6, 0x0FC6, Type::NSM},
    Range{0x102D, 0x1030, Type::NSM}, Range{0x1032, 0x1037, Type::NSM},
    Range{0x1039, 0x103A, Type::NSM}, Range{0x103D, 0x103E, Type::NSM},
    Range{0x1058, 0x1059, Type::NSM}, Range{0x105E, 0x1060, Type::NSM},
    Range{0x1071, 0x1074, Type::NSM}, Range{0x1082, 0x1082, Type::NSM},
    Range{0x1085, 0x1086, Type::NSM}, Range{0x108D, 0x108D, Type::NSM},
    Range{0x109D, 0x109D, Type::NSM}, Range{0x135D, 0x135F, Type::NSM},
    Range{0x1390, 0x1399, Type::ON}, Range{0x1400, 0x1400, Type::ON},
    Range{0x1680, 0x1680, Type::WS}, Range{0x169B, 0x169C, Type::ON},
    Range{0x1712, 0x1714, Type::NSM}, Range{0x1732, 0x1733, Type::NSM},
    Range{0x1752, 0x1753, Type::NSM}, Range{0x1772, 0x1773, Type::NSM},
    Range{0x17B4, 0x17B5, Type::NSM}, Range{0x17B7, 0x17BD, Type::NSM},
    Range{0x17C6, 0x17C6, Type::NSM}, Range{0x17C9, 0x17D3, Type::NSM},
    Range{0x17DB, 0x17DB, Type::ET}, Range{0x17DD, 0x17DD, Type::NSM},
    Range{0x17F0, 0x17F9, Type::ON}, Range{0x1800, 0x180A, Type::ON},
    Range{0x180B, 0x180D, Type::NSM}, Range{0x180E, 0x180E, Type::BN},
    Range{0x180F, 0x180F, Type::NSM}, Range{0x1885, 0x1886, Type::NSM},
    Range{0x18A9, 0x18A9, Type::NSM}, Range{0x1920, 0x1922, Type::NSM},
    Range{0x1927, 0x1928, Type::NSM}, Range{0x1932, 0x1932, Type::NSM},
    Range{0x1939, 0x193B, Type::NSM}, Range{0x1940, 0x1940, Type::ON},
    Range{0x1944, 0x1945, Type::ON}, Range{0x19DE, 0x19FF, Type::ON},
    Range{0x1A17, 0x1A18, Type::NSM}, Range{0x1A1B, 0x1A1B, Type::NSM},
    Range{0x1A56, 0x1A56, Type::NSM}, Range{0x1A58, 0x1A5E, Type::NSM},
    Range{0x1A60, 0x1A60, Type::NSM}, Range{0x1A62, 0x1A62, Type::NSM},
    Range{0x1A65, 0x1A6C, Type::NSM}, Range{0x1A73, 0x1A7C, Type::NSM},
    Range{0x1A7F, 0x1A7F, Type::NSM}, Range{0x1AB0, 0x1ACE, Type::NSM},
    Range{0x1B00, 0x1B03, Type::NSM}, Range{0x1B34, 0x1B34, Type::NSM},
    Range{0x1B36, 0x1B3A, Type::NSM}, Range{0x1B3C, 0x1B3C, Type::NSM},
    Range{0x1B42, 0x1B42, Type::NSM}, Range{0x1B6B, 0x1B73, Type::NSM},
    Range{0x1B80, 0x1B81, Type::NSM}, Range{0x1BA2, 0x1BA5, Type::NSM},
    Range{0x1BA8, 0x1BA9, Type::NSM}, Range{0x1BAB, 0x1BAD, Type::NSM},
    Range{0x1BE6, 0x1BE6, Type::NSM}, Range{0x1BE8, 0x1BE9, Type::NSM},
    Range{0x1BED, 0x1BED, Type::NSM}, Range{0x1BEF, 0x1BF1, Type::NSM},
    Range{0x1C2C, 0x1C33, Type::NSM}, Range{0x1C36, 0x1C37, Type::NSM},
    Range{0x1CD0, 0x1CD2, Type::NSM}, Range{0x1CD4, 0x1CE0, Type::NSM},
    Range{0x1CE2, 0x1CE8, Type::NSM}, Range{0x1CED, 0x1CED, Type::NSM},
    Range{0x1CF4, 0x1CF4, Type::NSM}, Range{0x1CF8, 0x1CF9, Type::NSM},
    Range{0x1DC0, 0x1DFF, Type::NSM}, Range{0x1FBD, 0x1FBD, Type::ON},
    Range{0x1FBF, 0x1FC1, Type::ON}, Range{0x1FCD, 0x1FCF, Type::ON},
    Range{0x1FDD, 0x1FDF, Type::ON}, Range{0x1FED, 0x1FEF, Type::ON},
    Range{0x1FFD, 0x1FFE, Type::ON}, Range{0x2000, 0x200A, Type::WS},
    Range{0x200B, 0x200D, Type::BN}, Range{0x200F, 0x200F, Type::R},
    Range{0x2010, 0x2027, Type::ON}, Range{0x2028, 0x2028, Type::WS},
    Range{0x2029, 0x2029, Type::B}, Range{0x202A, 0x202A, Type::LRE},
    Range{0x202B, 0x202B, Type::RLE}, Range{0x202C, 0x202C, Type::PDF},
    Range{0x202D, 0x202D, Type::LRO}, Range{0x202E, 0x202E, Type::RLO},
    Range{0x202F, 0x202F, Type::CS}, Range{0x2030, 0x2034, Type::ET},
    Range{0x2035, 0x2043, Type::ON}, Range{0x2044, 0x2044, Type::CS},
    Range{0x2045, 0x205E, Type::ON}, Range{0x205F, 0x205F, Type::WS},
    Range{0x2060, 0x2065, Type::BN}, Range{0x2066, 0x2066, Type::LRI},
    Range{0x2067, 0x2067, Type::RLI}, Range{0x2068, 0x2068, Type::FSI},
    Range{0x2069, 0x2069, Type::PDI}, Range{0x206A, 0x206F, Type::BN},
    Range{0x2070, 0x2070, Type::EN}, Range{0x2074, 0x2079, Type::EN},
    Range{0x207A, 0x207B, Type::ES}, Range{0x207C, 0x207E, Type::ON},
    Range{0x2080, 0x2089, Type::EN}, Range{0x208A, 0x208B, Type::ES},
    Range{0x208C, 0x208E, Type::ON}, Range{0x20A0, 0x20CF, Type::ET},
    Range{0x20D0, 0x20F0, Type::NSM}, Range{0x2100, 0x2101, Type::ON},
    Range{0x2103, 0x2106, Type::ON}, Range{0x2108, 0x2109, Type::ON},
    Range{0x2114, 0x2114, Type::ON}, Range{0x2116, 0x2118, Type::ON},
    Range{0x211E, 0x2123, Type::ON}, Range{0x2125, 0x2125, Type::ON},
    Range{0x2127, 0x2127, Type::ON}, Range{0x2129, 0x2129, Type::ON},
    Range{0x212E, 0x212E, Type::ET}, Range{0x213A, 0x213B, Type::ON},
    Range{0x2140, 0x2144, Type::ON}, Range{0x214A, 0x214D, Type::ON},
    Range{0x2150, 0x215F, Type::ON}, Range{0x2189, 0x218B, Type::ON},
    Range{0x2190, 0x2211, Type::ON}, Range{0x2212, 0x2212, Type::ES},
    Range{0x2213, 0x2213, Type::ET}, Range{0x2214, 0x2335, Type::ON},
    Range{0x237B, 0x2394, Type::ON}, Range{0x2396, 0x2429, Type::ON},
    Range{0x2440, 0x244A, Type::ON}, Range{0x2460, 0x2487, Type::ON},
    Range{0x2488, 0x249B, Type::EN}, Range{0x24EA, 0x26AB, Type::ON},
    Range{0x26AD, 0x27FF, Type::ON}, Range{0x2900, 0x2B73, Type::ON},
    Range{0x2B76, 0x2B95, Type::ON}, Range{0x2B97, 0x2BFF, Type::ON},
    Range{0x2CE5, 0x2CEA, Type::ON}, Range{0x2CEF, 0x2CF1, Type::NSM},
    Range{0x2CF9, 0x2CFF, Type::ON}, Range{0x2D7F, 0x2D7F, Type::NSM},
    Range{0x2DE0, 0x2DFF, Type::NSM}, Range{0x2E00, 0x2E5D, Type::ON},
    Range{0x2E80, 0x2E99, Type::ON}, Range{0x2E9B, 0x2EF3, Type::ON},
    Range{0x2F00, 0x2FD5, Type::ON}, Range{0x2FF0, 0x2FFF, Type::ON},
    Range{0x3000, 0x3000, Type::WS}, Range{0x3001, 0x3004, Type::ON},
    Range{0x3008, 0x3020, Type::ON}, Range{0x302A, 0x302D, Type::NSM},
    Range{0x3030, 0x3030, Type::ON}, Range{0x3036, 0x3037, Type::ON},
    Range{0x303D, 0x303F, Type::ON}, Range{0x3099, 0x309A, Type::NSM},
    Range{0x309B, 0x309C, Type::ON}, Range{0x30A0, 0x30A0, Type::ON},
    Range{0x30FB, 0x30FB, Type::ON}, Range{0x31C0, 0x31E5, Type::ON},
    Range{0x31EF, 0x31EF, Type::ON}, Range{0x321D, 0x321E, Type::ON},
    Range{0x3250, 0x325F, Type::ON}, Range{0x327C, 0x327E, Type::ON},
    Range{0x32B1, 0x32BF, Type::ON}, Range{0x32CC, 0x32CF, Type::ON},
    Range{0x3377, 0x337A, Type::ON}, Range{0x33DE, 0x33DF, Type::ON},
    Range{0x33FF, 0x33FF, Type::ON}, Range{0x4DC0, 0x4DFF, Type::ON},
    Range{0xA490, 0xA4C6, Type::ON}, Range{0xA60D, 0xA60F, Type::ON},
    Range{0xA66F, 0xA672, Type::NSM}, Range{0xA673, 0xA673, Type::ON},
    Range{0xA674, 0xA67D, Type::NSM}, Range{0xA67E, 0xA67F, Type::ON},
    Range{0xA69E, 0xA69F, Type::NSM}, Range{0xA6F0, 0xA6F1, Type::NSM},
    Range{0xA700, 0xA721, Type::ON}, Range{0xA788, 0xA788, Type::ON},
    Range{0xA802, 0xA802, Type::NSM}, Range{0xA806, 0xA806, Type::NSM},
    Range{0xA80B, 0xA80B, Type::NSM}, Range{0xA825, 0xA826, Type::NSM},
    Range{0xA828, 0xA82B, Type::ON}, Range{0xA82C, 0xA82C, Type::NSM},
    Range{0xA838, 0xA839, Type::ET}, Range{0xA874, 0xA877, Type::ON},
    Range{0xA8C4, 0xA8C5, Type::NSM}, Range{0xA8E0, 0xA8F1, Type::NSM},
    Range{0xA8FF, 0xA8FF, Type::NSM}, Range{0xA926, 0xA92D, Type::NSM},
    Range{0xA947, 0xA951, Type::NSM}, Range{0xA980, 0xA982, Type::NSM},
    Range{0xA9B3, 0xA9B3, Type::NSM}, Range{0xA9B6, 0xA9B9, Type::NSM},
    Range{0xA9BC, 0xA9BD, Type::NSM}, Range{0xA9E5, 0xA9E5, Type::NSM},
    Range{0xAA29, 0xAA2E, Type::NSM}, Range{0xAA31, 0xAA32, Type::NSM},
    Range{0xAA35, 0xAA36, Type::NSM}, Range{0xAA43, 0xAA43, Type::NSM},
    Range{0xAA4C, 0xAA4C, Type::NSM}, Range{0xAA7C, 0xAA7C, Type::NSM},
    Range{0xAAB0, 0xAAB0, Type::NSM}, Range{0xAAB2, 0xAAB4, Type::NSM},
    Range{0xAAB7, 0xAAB8, Type::NSM}, Range{0xAABE, 0xAABF, Type::NSM},
    Range{0xAAC1, 0xAAC1, Type::NSM}, Range{0xAAEC, 0xAAED, Type::NSM},
    Range{0xAAF6, 0xAAF6, Type::NSM}, Range{0xAB6A, 0xAB6B, Type::ON},
    Range{0xABE5, 0xABE5, Type::NSM}, Range{0xABE8, 0xABE8, Type::NSM},
    Range{0xABED, 0xABED, Type::NSM}, Range{0xFB1D, 0xFB1D, Type::R},
    Range{0xFB1E, 0xFB1E, Type::NSM}, Range{0xFB1F, 0xFB28, Type::R},
    Range{0xFB29, 0xFB29, Type::ES}, Range{0xFB2A, 0xFB4F, Type::R},
    Range{0xFB50, 0xFD3D, Type::AL}, Range{0xFD3E, 0xFD4F, Type::ON},
    Range{0xFD50, 0xFDCE, Type::AL}, Range{0xFDCF, 0xFDCF, Type::ON},
    Range{0xFDD0, 0xFDEF, Type::BN}, Range{0xFDF0, 0xFDFC, Type::AL},
    Range{0xFDFD, 0xFDFF, Type::ON}, Range{0xFE00, 0xFE0F, Type::NSM},
    Range{0xFE10, 0xFE19, Type::ON}, Range{0xFE20, 0xFE2F, Type::NSM},
    Range{0xFE30, 0xFE4F, Type::ON}, Range{0xFE50, 0xFE50, Type::CS},
    Range{0xFE51, 0xFE51, Type::ON}, Range{0xFE52, 0xFE52, Type::CS},
    Range{0xFE54, 0xFE54, Type::ON}, Range{0xFE55, 0xFE55, Type::CS},
    Range{0xFE56, 0xFE5E, Type::ON}, Range{0xFE5F, 0xFE5F, Type::ET},
    Range{0xFE60, 0xFE61, Type::ON}, Range{0xFE62, 0xFE63, Type::ES},
    Range{0xFE64, 0xFE66, Type::ON}, Range{0xFE68, 0xFE68, Type::ON},
    Range{0xFE69, 0xFE6A, Type::ET}, Range{0xFE6B, 0xFE6B, Type::ON},
    Range{0xFE70, 0xFEFE, Type::AL}, Range{0xFEFF, 0xFEFF, Type::BN},
    Range{0xFF01, 0xFF02, Type::ON}, Range{0xFF03, 0xFF05, Type::ET},
    Range{0xFF06, 0xFF0A, Type::ON}, Range{0xFF0B, 0xFF0B, Type::ES},
    Range{0xFF0C, 0xFF0C, Type::CS}, Range{0xFF0D, 0xFF0D, Type::ES},
    Range{0xFF0E, 0xFF0F, Type::CS}, Range{0xFF10, 0xFF19, Type::EN},
    Range{0xFF1A, 0xFF1A, Type::CS}, Range{0xFF1B, 0xFF20, Type::ON},
    Range{0xFF3B, 0xFF40, Type::ON}, Range{0xFF5B, 0xFF65, Type::ON},
    Range{0xFFE0, 0xFFE1, Type::ET}, Range{0xFFE2, 0xFFE4, Type::ON},
    Range{0xFFE5, 0xFFE6, Type::ET}, Range{0xFFE8, 0xFFEE, Type::ON},
    Range{0xFFF0, 0xFFF8, Type::BN}, Range{0xFFF9, 0xFFFD, Type::ON},
    Range{0xFFFE, 0xFFFF, Type::BN}, Range{0x10101, 0x10101, Type::ON},
    Range{0x10140, 0x1018C, Type::ON}, Range{0x10190, 0x1019C, Type::ON},
    Range{0x101A0, 0x101A0, Type::ON}, Range{0x101FD, 0x101FD, Type::NSM},
    Range{0x102E0, 0x102E0, Type::NSM}, Range{0x102E1, 0x102FB, Type::EN},
    Range{0x10376, 0x1037A, Type::NSM}, Range{0x10800, 0x1091E, Type::R},
    Range{0x1091F, 0x1091F, Type::ON}, Range{0x10920, 0x10A00, Type::R},
    Range{0x10A01, 0x10A03, Type::NSM}, Range{0x10A04, 0x10A04, Type::R},
    Range{0x10A05, 0x10A06, Type::NSM}, Range{0x10A07, 0x10A0B, Type::R},
    Range{0x10A0C, 0x10A0F, Type::NSM}, Range{0x10A10, 0x10A37, Type::R},
    Range{0x10A38, 0x10A3A, Type::NSM}, Range{0x10A3B, 0x10A3E, Type::R},
    Range{0x10A3F, 0x10A3F, Type::NSM}, Range{0x10A40, 0x10AE4, Type::R},
    Range{0x10AE5, 0x10AE6, Type::NSM}, Range{0x10AE7, 0x10B38, Type::R},
    Range{0x10B39, 0x10B3F, Type::ON}, Range{0x10B40, 0x10CFF, Type::R},
    Range{0x10D00, 0x10D23, Type::AL}, Range{0x10D24, 0x10D27, Type::NSM},
    Range{0x10D28, 0x10D2F, Type::AL}, Range{0x10D30, 0x10D39, Type::AN},
    Range{0x10D3A, 0x10D3F, Type::AL}, Range{0x10D40, 0x10D49, Type::AN},
    Range{0x10D4A, 0x10D68, Type::R}, Range{0x10D69, 0x10D6D, Type::NSM},
    Range{0x10D6E, 0x10D6E, Type::ON}, Range{0x10D6F, 0x10E5F, Type::R},
    Range{0x10E60, 0x10E7E, Type::AN}, Range{0x10E7F, 0x10EAA, Type::R},
    Range{0x10EAB, 0x10EAC, Type::NSM}, Range{0x10EAD, 0x10EBF, Type::R},
    Range{0x10EC0, 0x10EFB, Type::AL}, Range{0x10EFC, 0x10EFF, Type::NSM},
    Range{0x10F00, 0x10F2F, Type::R}, Range{0x10F30, 0x10F45, Type::AL},
    Range{0x10F46, 0x10F50, Type::NSM}, Range{0x10F51, 0x10F6F, Type::AL},
    Range{0x10F70, 0x10F81, Type::R}, Range{0x10F82, 0x10F85, Type::NSM},
    Range{0x10F86, 0x10FFF, Type::R}, Range{0x11001, 0x11001, Type::NSM},
    Range{0x11038, 0x11046, Type::NSM}, Range{0x11052, 0x11065, Type::ON},
    Range{0x11070, 0x11070, Type::NSM}, Range{0x11073, 0x11074, Type::NSM},
    Range{0x1107F, 0x11081, Type::NSM}, Range{0x110B3, 0x110B6, Type::NSM},
    Range{0x110B9, 0x110BA, Type::NSM}, Range{0x110C2, 0x110C2, Type::NSM},
    Range{0x11100, 0x11102, Type::NSM}, Range{0x11127, 0x1112B, Type::NSM},
    Range{0x1112D, 0x11134, Type::NSM}, Range{0x11173, 0x11173, Type::NSM},
    Range{0x11180, 0x11181, Type::NSM}, Range{0x111B6, 0x111BE, Type::NSM},
    Range{0x111C9, 0x111CC, Type::NSM}, Range{0x111CF, 0x111CF, Type::NSM},
    Range{0x1122F, 0x11231, Type::NSM}, Range{0x11234, 0x11234, Type::NSM},
    Range{0x11236, 0x11237, Type::NSM}, Range{0x1123E, 0x1123E, Type::NSM},
    Range{0x11241, 0x11241, Type::NSM}, Range{0x112DF, 0x112DF, Type::NSM},
    Range{0x112E3, 0x112EA, Type::NSM}, Range{0x11300, 0x11301, Type::NSM},
    Range{0x1133B, 0x1133C, Type::NSM}, Range{0x11340, 0x11340, Type::NSM},
    Range{0x11366, 0x1136C, Type::NSM}, Range{0x11370, 0x11374, Type::NSM},
    Range{0x113BB, 0x113C0, Type::NSM}, Range{0x113CE, 0x113CE, Type::NSM},
    Range{0x113D0, 0x113D0, Type::NSM}, Range{0x113D2, 0x113D2, Type::NSM},
    Range{0x113E1, 0x113E2, Type::NSM}, Range{0x11438, 0x1143F, Type::NSM},
    Range{0x11442, 0x11444, Type::NSM}, Range{0x11446, 0x11446, Type::NSM},
    Range{0x1145E, 0x1145E, Type::NSM}, Range{0x114B3, 0x114B8, Type::NSM},
    Range{0x114BA, 0x114BA, Type::NSM}, Range{0x114BF, 0x114C0, Type::NSM},
    Range{0x114C2, 0x114C3, Type::NSM}, Range{0x115B2, 0x115B5, Type::NSM},
    Range{0x115BC, 0x115BD, Type::NSM}, Range{0x115BF, 0x115C0, Type::NSM},
    Range{0x115DC, 0x115DD, Type::NSM}, Range{0x11633, 0x1163A, Type::NSM},
    Range{0x1163D, 0x1163D, Type::NSM}, Range{0x1163F, 0x11640, Type::NSM},
    Range{0x11660, 0x1166C, Type::ON}, Range{0x116AB, 0x116AB, Type::NSM},
    Range{0x116AD, 0x116AD, Type::NSM}, Range{0x116B0, 0x116B5, Type::NSM},
    Range{0x116B7, 0x116B7, Type::NSM}, Range{0x1171D, 0x1171D, Type::NSM},
    Range{0x1171F, 0x1171F, Type::NSM}, Range{0x11722, 0x11725, Type::NSM},
    Range{0x11727, 0x1172B, Type::NSM}, Range{0x1182F, 0x11837, Type::NSM},
    Range{0x11839, 0x1183A, Type::NSM}, Range{0x1193B, 0x1193C, Type::NSM},
    Range{0x1193E, 0x1193E, Type::NSM}, Range{0x11943, 0x11943, Type::NSM},
    Range{0x119D4, 0x119D7, Type::NSM}, Range{0x119DA, 0x119DB, Type::NSM},
    Range{0x119E0, 0x119E0, Type::NSM}, Range{0x11A01, 0x11A06, Type::NSM},
    Range{0x11A09, 0x11A0A, Type::NSM}, Range{0x11A33, 0x11A38, Type::NSM},
    Range{0x11A3B, 0x11A3E, Type::NSM}, Range{0x11A47, 0x11A47, Type::NSM},
    Range{0x11A51, 0x11A56, Type::NSM}, Range{0x11A59, 0x11A5B, Type::NSM},
    Range{0x11A8A, 0x11A96, Type::NSM}, Range{0x11A98, 0x11A99, Type::NSM},
    Range{0x11C30, 0x11C36, Type::NSM}, Range{0x11C38, 0x11C3D, Type::NSM},
    Range{0x11C92, 0x11CA7, Type::NSM}, Range{0x11CAA, 0x11CB0, Type::NSM},
    Range{0x11CB2, 0x11CB3, Type::NSM}, Range{0x11CB5, 0x11CB6, Type::NSM},
    Range{0x11D31, 0x11D36, Type::NSM}, Range{0x11D3A, 0x11D3A, Type::NSM},
    Range{0x11D3C, 0x11D3D, Type::NSM}, Range{0x11D3F, 0x11D45, Type::NSM},
    Range{0x11D47, 0x11D47, Type::NSM}, Range{0x11D90, 0x11D91, Type::NSM},
    Range{0x11D95, 0x11D95, Type::NSM}, Range{0x11D97, 0x11D97, Type::NSM},
    Range{0x11EF3, 0x11EF4, Type::NSM}, Range{0x11F00, 0x11F01, Type::NSM},
    Range{0x11F36, 0x11F3A, Type::NSM}, Range{0x11F40, 0x11F40, Type::NSM},
    Range{0x11F42, 0x11F42, Type::NSM}, Range{0x11F5A, 0x11F5A, Type::NSM},
    Range{0x11FD5, 0x11FDC, Type::ON}, Range{0x11FDD, 0x11FE0, Type::ET},
    Range{0x11FE1, 0x11FF1, Type::ON}, Range{0x13440, 0x13440, Type::NSM},
    Range{0x13447, 0x13455, Type::NSM}, Range{0x1611E, 0x16129, Type::NSM},
    Range{0x1612D, 0x1612F, Type::NSM}, Range{0x16AF0, 0x16AF4, Type::NSM},
    Range{0x16B30, 0x16B36, Type::NSM}, Range{0x16F4F, 0x16F4F, Type::NSM},
    Range{0x16F8F, 0x16F92, Type::NSM}, Range{0x16FE2, 0x16FE2, Type::ON},
    Range{0x16FE4, 0x16FE4, Type::NSM}, Range{0x1BC9D, 0x1BC9E, Type::NSM},
    Range{0x1BCA0, 0x1BCA3, Type::BN}, Range{0x1CC00, 0x1CCD5, Type::ON},
    Range{0x1CCF0, 0x1CCF9, Type::EN}, Range{0x1CD00, 0x1CEB3, Type::ON},
    Range{0x1CF00, 0x1CF2D, Type::NSM}, Range{0x1CF30, 0x1CF46, Type::NSM},
    Range{0x1D167, 0x1D169, Type::NSM}, Range{0x1D173, 0x1D17A, Type::BN},
    Range{0x1D17B, 0x1D182, Type::NSM}, Range{0x1D185, 0x1D18B, Type::NSM},
    Range{0x1D1AA, 0x1D1AD, Type::NSM}, Range{0x1D1E9, 0x1D1EA, Type::ON},
    Range{0x1D200, 0x1D241, Type::ON}, Range{0x1D242, 0x1D244, Type::NSM},
    Range{0x1D245, 0x1D245, Type::ON}, Range{0x1D300, 0x1D356, Type::ON},
    Range{0x1D6C1, 0x1D6C1, Type::ON}, Range{0x1D6DB, 0x1D6DB, Type::ON},
    Range{0x1D6FB, 0x1D6FB, Type::ON}, Range{0x1D715, 0x1D715, Type::ON},
    Range{0x1D735, 0x1D735, Type::ON}, Range{0x1D74F, 0x1D74F, Type::ON},
    Range{0x1D76F, 0x1D76F, Type::ON}, Range{0x1D789, 0x1D789, Type::ON},
    Range{0x1D7A9, 0x1D7A9, Type::ON}, Range{0x1D7C3, 0x1D7C3, Type::ON},
    Range{0x1D7CE, 0x1D7FF, Type::EN}, Range{0x1DA00, 0x1DA36, Type::NSM},
    Range{0x1DA3B, 0x1DA6C, Type::NSM}, Range{0x1DA75, 0x1DA75, Type::NSM},
    Range{0x1DA84, 0x1DA84, Type::NSM}, Range{0x1DA9B, 0x1DA9F, Type::NSM},
    Range{0x1DAA1, 0x1DAAF, Type::NSM}, Range{0x1E000, 0x1E006, Type::NSM},
    Range{0x1E008, 0x1E018, Type::NSM}, Range{0x1E01B, 0x1E021, Type::NSM},
    Range{0x1E023, 0x1E024, Type::NSM}, Range{0x1E026, 0x1E02A, Type::NSM},
    Range{0x1E08F, 0x1E08F, Type::NSM}, Range{0x1E130, 0x1E136, Type::NSM},
    Range{0x1E2AE, 0x1E2AE, Type::NSM}, Range{0x1E2EC, 0x1E2EF, Type::NSM},
    Range{0x1E2FF, 0x1E2FF, Type::ET}, Range{0x1E4EC, 0x1E4EF, Type::NSM},
    Range{0x1E5EE, 0x1E5EF, Type::NSM}, Range{0x1E800, 0x1E8CF, Type::R},
    Range{0x1E8D0, 0x1E8D6, Type::NSM}, Range{0x1E8D7, 0x1E943, Type::R},
    Range{0x1E944, 0x1E94A, Type::NSM}, Range{0x1E94B, 0x1EC6F, Type::R},
    Range{0x1EC70, 0x1ECBF, Type::AL}, Range{0x1ECC0, 0x1ECFF, Type::R},
    Range{0x1ED00, 0x1ED4F, Type::AL}, Range{0x1ED50, 0x1EDFF, Type::R},
    Range{0x1EE00, 0x1EEEF, Type::AL}, Range{0x1EEF0, 0x1EEF1, Type::ON},
    Range{0x1EEF2, 0x1EEFF, Type::AL}, Range{0x1EF00, 0x1EFFF, Type::R},
    Range{0x1F000, 0x1F02B, Type::ON}, Range{0x1F030, 0x1F093, Type::ON},
    Range{0x1F0A0, 0x1F0AE, Type::ON}, Range{0x1F0B1, 0x1F0BF, Type::ON},
    Range{0x1F0C1, 0x1F0CF, Type::ON}, Range{0x1F0D1, 0x1F0F5, Type::ON},
    Range{0x1F100, 0x1F10A, Type::EN}, Range{0x1F10B, 0x1F10F, Type::ON},
    Range{0x1F12F, 0x1F12F, Type::ON}, Range{0x1F16A, 0x1F16F, Type::ON},
    Range{0x1F1AD, 0x1F1AD, Type::ON}, Range{0x1F260, 0x1F265, Type::ON},
    Range{0x1F300, 0x1F6D7, Type::ON}, Range{0x1F6DC, 0x1F6EC, Type::ON},
    Range{0x1F6F0, 0x1F6FC, Type::ON}, Range{0x1F700, 0x1F776, Type::ON},
    Range{0x1F77B, 0x1F7D9, Type::ON}, Range{0x1F7E0, 0x1F7EB, Type::ON},
    Range{0x1F7F0, 0x1F7F0, Type::ON}, Range{0x1F800, 0x1F80B, Type::ON},
    Range{0x1F810, 0x1F847, Type::ON}, Range{0x1F850, 0x1F859, Type::ON},
    Range{0x1F860, 0x1F887, Type::ON}, Range{0x1F890, 0x1F8AD, Type::ON},
    Range{0x1F8B0, 0x1F8BB, Type::ON}, Range{0x1F8C0, 0x1F8C1, Type::ON},
    Range{0x1F900, 0x1FA53, Type::ON}, Range{0x1FA60, 0x1FA6D, Type::ON},
    Range{0x1FA70, 0x1FA7C, Type::ON}, Range{0x1FA80, 0x1FA89, Type::ON},
    Range{0x1FA8F, 0x1FAC6, Type::ON}, Range{0x1FACE, 0x1FADC, Type::ON},
    Range{0x1FADF, 0x1FAE9, Type::ON}, Range{0x1FAF0, 0x1FAF8, Type::ON},
    Range{0x1FB00, 0x1FB92, Type::ON}, Range{0x1FB94, 0x1FBEF, Type::ON},
    Range{0x1FBF0, 0x1FBF9, Type::EN}, Range{0x1FFFE, 0x1FFFF, Type::BN},
    Range{0x2FFFE, 0x2FFFF, Type::BN}, Range{0x3FFFE, 0x3FFFF, Type::BN},
    Range{0x4FFFE, 0x4FFFF, Type::BN}, Range{0x5FFFE, 0x5FFFF, Type::BN},
    Range{0x6FFFE, 0x6FFFF, Type::BN}, Range{0x7FFFE, 0x7FFFF, Type::BN},
    Range{0x8FFFE, 0x8FFFF, Type::BN}, Range{0x9FFFE, 0x9FFFF, Type::BN},
    Range{0xAFFFE, 0xAFFFF, Type::BN}, Range{0xBFFFE, 0xBFFFF, Type::BN},
    Range{0xCFFFE, 0xCFFFF, Type::BN}, Range{0xDFFFE, 0xE00FF, Type::BN},
    Range{0xE0100, 0xE01EF, Type::NSM}, Range{0xE01F0, 0xE0FFF, Type::BN},
    Range{0xEFFFE, 0xEFFFF, Type::BN}, Range{0xFFFFE, 0xFFFFF, Type::BN},
    Range{0x10FFFE, 0x10FFFF, Type::BN},
}};

inline constexpr std::array<Bracket, 128> brackets = {{
    Bracket{0x0028, 0x0029, true}, Bracket{0x0029, 0x0028, false},
    Bracket{0x005B, 0x005D, true}, Bracket{0x005D, 0x005B, false},
    Bracket{0x007B, 0x007D, true}, Bracket{0x007D, 0x007B, false},
    Bracket{0x0F3A, 0x0F3B, true}, Bracket{0x0F3B, 0x0F3A, false},
    Bracket{0x0F3C, 0x0F3D, true}, Bracket{0x0F3D, 0x0F3C, false},
    Bracket{0x169B, 0x169C, true}, Bracket{0x169C, 0x169B, false},
    Bracket{0x2045, 0x2046, true}, Bracket{0x2046, 0x2045, false},
    Bracket{0x207D, 0x207E, true}, Bracket{0x207E, 0x207D, false},
    Bracket{0x208D, 0x208E, true}, Bracket{0x208E, 0x208D, false},
    Bracket{0x2308, 0x2309, true}, Bracket{0x2309, 0x2308, false},
    Bracket{0x230A, 0x230B, true}, Bracket{0x230B, 0x230A, false},
    Bracket{0x2329, 0x232A, true}, Bracket{0x232A, 0x2329, false},
    Bracket{0x2768, 0x2769, true}, Bracket{0x2769, 0x2768, false},
    Bracket{0x276A, 0x276B, true}, Bracket{0x276B, 0x276A, false},
    Bracket{0x276C, 0x276D, true}, Bracket{0x276D, 0x276C, false},
    Bracket{0x276E, 0x276F, true}, Bracket{0x276F, 0x276E, false},
    Bracket{0x2770, 0x2771, true}, Bracket{0x2771, 0x2770, false},
    Bracket{0x2772, 0x2773, true}, Bracket{0x2773, 0x2772, false},
    Bracket{0x2774, 0x2775, true}, Bracket{0x2775, 0x2774, false},
    Bracket{0x27C5, 0x27C6, true}, Bracket{0x27C6, 0x27C5, false},
    Bracket{0x27E6, 0x27E7, true}, Bracket{0x27E7, 0x27E6, false},
    Bracket{0x27E8, 0x27E9, true}, Bracket{0x27E9, 0x27E8, false},
    Bracket{0x27EA, 0x27EB, true}, Bracket{0x27EB, 0x27EA, false},
    Bracket{0x27EC, 0x27ED, true}, Bracket{0x27ED, 0x27EC, false},
    Bracket{0x27EE, 0x27EF, true}, Bracket{0x27EF, 0x27EE, false},
    Bracket{0x2983, 0x2984, true}, Bracket{0x2984, 0x2983, false},
    Bracket{0x2985, 0x2986, true}, Bracket{0x2986, 0x2985, false},
    Bracket{0x2987, 0x2988, true}, Bracket{0x2988, 0x2987, false},
    Bracket{0x2989, 0x298A, true}, Bracket{0x298A, 0x2989, false},
    Bracket{0x298B, 0x298C, true}, Bracket{0x298C, 0x298B, false},
    Bracket{0x298D, 0x2990, true}, Bracket{0x298E, 0x298F, false},
    Bracket{0x298F, 0x298E, true}, Bracket{0x2990, 0x298D, false},
    Bracket{0x2991, 0x2992, true}, Bracket{0x2992, 0x2991, false},
    Bracket{0x2993, 0x2994, true}, Bracket{0x2994, 0x2993, false},
    Bracket{0x2995, 0x2996, true}, Bracket{0x2996, 0x2995, false},
    Bracket{0x2997, 0x2998, true}, Bracket{0x2998, 0x2997, false},
    Bracket{0x29D8, 0x29D9, true}, Bracket{0x29D9, 0x29D8, false},
    Bracket{0x29DA, 0x29DB, true}, Bracket{0x29DB, 0x29DA, false},
    Bracket{0x29FC, 0x29FD, true}, Bracket{0x29FD, 0x29FC, false},
    Bracket{0x2E22, 0x2E23, true}, Bracket{0x2E23, 0x2E22, false},
    Bracket{0x2E24, 0x2E25, true}, Bracket{0x2E25, 0x2E24, false},
    Bracket{0x2E26, 0x2E27, true}, Bracket{0x2E27, 0x2E26, false},
    Bracket{0x2E28, 0x2E29, true}, Bracket{0x2E29, 0x2E28, false},
    Bracket{0x2E55, 0x2E56, true}, Bracket{0x2E56, 0x2E55, false},
    Bracket{0x2E57, 0x2E58, true}, Bracket{0x2E58, 0x2E57, false},
    Bracket{0x2E59, 0x2E5A, true}, Bracket{0x2E5A, 0x2E59, false},
    Bracket{0x2E5B, 0x2E5C, true}, Bracket{0x2E5C, 0x2E5B, false},
    Bracket{0x3008, 0x3009, true}, Bracket{0x3009, 0x3008, false},
    Bracket{0x300A, 0x300B, true}, Bracket{0x300B, 0x300A, false},
    Bracket{0x300C, 0x300D, true}, Bracket{0x300D, 0x300C, false},
    Bracket{0x300E, 0x300F, true}, Bracket{0x300F, 0x300E, false},
    Bracket{0x3010, 0x3011, true}, Bracket{0x3011, 0x3010, false},
    Bracket{0x3014, 0x3015, true}, Bracket{0x3015, 0x3014, false},
    Bracket{0x3016, 0x3017, true}, Bracket{0x3017, 0x3016, false},
    Bracket{0x3018, 0x3019, true}, Bracket{0x3019, 0x3018, false},
    Bracket{0x301A, 0x301B, true}, Bracket{0x301B, 0x301A, false},
    Bracket{0xFE59, 0xFE5A, true}, Bracket{0xFE5A, 0xFE59, false},
    Bracket{0xFE5B, 0xFE5C, true}, Bracket{0xFE5C, 0xFE5B, false},
    Bracket{0xFE5D, 0xFE5E, true}, Bracket{0xFE5E, 0xFE5D, false},
    Bracket{0xFF08, 0xFF09, true}, Bracket{0xFF09, 0xFF08, false},
    Bracket{0xFF3B, 0xFF3D, true}, Bracket{0xFF3D, 0xFF3B, false},
    Bracket{0xFF5B, 0xFF5D, true}, Bracket{0xFF5D, 0xFF5B, false},
    Bracket{0xFF5F, 0xFF60, true}, Bracket{0xFF60, 0xFF5F, false},
    Bracket{0xFF62, 0xFF63, true}, Bracket{0xFF63, 0xFF62, false},
}};

inline constexpr std::array<Mirror, 428> mirrors = {{
    Mirror{0x0028, 0x0029}, Mirror{0x0029, 0x0028}, Mirror{0x003C, 0x003E},
    Mirror{0x003E, 0x003C}, Mirror{0x005B, 0x005D}, Mirror{0x005D, 0x005B},
    Mirror{0x007B, 0x007D}, Mirror{0x007D, 0x007B}, Mirror{0x00AB, 0x00BB},
    Mirror{0x00BB, 0x00AB}, Mirror{0x0F3A, 0x0F3B}, Mirror{0x0F3B, 0x0F3A},
    Mirror{0x0F3C, 0x0F3D}, Mirror{0x0F3D, 0x0F3C}, Mirror{0x169B, 0x169C},
    Mirror{0x169C, 0x169B}, Mirror{0x2039, 0x203A}, Mirror{0x203A, 0x2039},
    Mirror{0x2045, 0x2046}, Mirror{0x2046, 0x2045}, Mirror{0x207D, 0x207E},
    Mirror{0x207E, 0x207D}, Mirror{0x208D, 0x208E}, Mirror{0x208E, 0x208D},
    Mirror{0x2208, 0x220B}, Mirror{0x2209, 0x220C}, Mirror{0x220A, 0x220D},
    Mirror{0x220B, 0x2208}, Mirror{0x220C, 0x2209}, Mirror{0x220D, 0x220A},
    Mirror{0x2215, 0x29F5}, Mirror{0x221F, 0x2BFE}, Mirror{0x2220, 0x29A3},
    Mirror{0x2221, 0x299B}, Mirror{0x2222, 0x29A0}, Mirror{0x2224, 0x2AEE},
    Mirror{0x223C, 0x223D}, Mirror{0x223D, 0x223C}, Mirror{0x2243, 0x22CD},
    Mirror{0x2245, 0x224C}, Mirror{0x224C, 0x2245}, Mirror{0x2252, 0x2253},
    Mirror{0x2253, 0x2252}, Mirror{0x2254, 0x2255}, Mirror{0x2255, 0x2254},
    Mirror{0x2264, 0x2265}, Mirror{0x2265, 0x2264}, Mirror{0x2266, 0x2267},
    Mirror{0x2267, 0x2266}, Mirror{0x2268, 0x2269}, Mirror{0x2269, 0x2268},
    Mirror{0x226A, 0x226B}, Mirror{0x226B, 0x226A}, Mirror{0x226E, 0x226F},
    Mirror{0x226F, 0x226E}, Mirror{0x2270, 0x2271}, Mirror{0x2271, 0x2270},
    Mirror{0x2272, 0x2273}, Mirror{0x2273, 0x2272}, Mirror{0x2274, 0x2275},
    Mirror{0x2275, 0x2274}, Mirror{0x2276, 0x2277}, Mirror{0x2277, 0x2276},
    Mirror{0x2278, 0x2279}, Mirror{0x2279, 0x2278}, Mirror{0x227A, 0x227B},
    Mirror{0x227B, 0x227A}, Mirror{0x227C, 0x227D}, Mirror{0x227D, 0x227C},
    Mirror{0x227E, 0x227F}, Mirror{0x227F, 0x227E}, Mirror{0x2280, 0x2281},
    Mirror{0x2281, 0x2280}, Mirror{0x2282, 0x2283}, Mirror{0x2283, 0x2282},
    Mirror{0x2284, 0x2285}, Mirror{0x2285, 0x2284}, Mirror{0x2286, 0x2287},
    Mirror{0x2287, 0x2286}, Mirror{0x2288, 0x2289}, Mirror{0x2289, 0x2288},
    Mirror{0x228A, 0x228B}, Mirror{0x228B, 0x228A}, Mirror{0x228F, 0x2290},
    Mirror{0x2290, 0x228F}, Mirror{0x2291, 0x2292}, Mirror{0x2292, 0x2291},
    Mirror{0x2298, 0x29B8}, Mirror{0x22A2, 0x22A3}, Mirror{0x22A3, 0x22A2},
    Mirror{0x22A6, 0x2ADE}, Mirror{0x22A8, 0x2AE4}, Mirror{0x22A9, 0x2AE3},
    Mirror{0x22AB, 0x2AE5}, Mirror{0x22B0, 0x22B1}, Mirror{0x22B1, 0x22B0},
    Mirror{0x22B2, 0x22B3}, Mirror{0x22B3, 0x22B2}, Mirror{0x22B4, 0x22B5},
    Mirror{0x22B5, 0x22B4}, Mirror{0x22B6, 0x22B7}, Mirror{0x22B7, 0x22B6},
    Mirror{0x22B8, 0x27DC}, Mirror{0x22C9, 0x22CA}, Mirror{0x22CA, 0x22C9},
    Mirror{0x22CB, 0x22CC}, Mirror{0x22CC, 0x22CB}, Mirror{0x22CD, 0x2243},
    Mirror{0x22D0, 0x22D1}, Mirror{0x22D1, 0x22D0}, Mirror{0x22D6, 0x22D7},
    Mirror{0x22D7, 0x22D6}, Mirror{0x22D8, 0x22D9}, Mirror{0x22D9, 0x22D8},
    Mirror{0x22DA, 0x22DB}, Mirror{0x22DB, 0x22DA}, Mirror{0x22DC, 0x22DD},
    Mirror{0x22DD, 0x22DC}, Mirror{0x22DE, 0x22DF}, Mirror{0x22DF, 0x22DE},
    Mirror{0x22E0, 0x22E1}, Mirror{0x22E1, 0x22E0}, Mirror{0x22E2, 0x22E3},
    Mirror{0x22E3, 0x22E2}, Mirror{0x22E4, 0x22E5}, Mirror{0x22E5, 0x22E4},
    Mirror{0x22E6, 0x22E7}, Mirror{0x22E7, 0x22E6}, Mirror{0x22E8, 0x22E9},
    Mirror{0x22E9, 0x22E8}, Mirror{0x22EA, 0x22EB}, Mirror{0x22EB, 0x22EA},
    Mirror{0x22EC, 0x22ED}, Mirror{0x22ED, 0x22EC}, Mirror{0x22F0, 0x22F1},
    Mirror{0x22F1, 0x22F0}, Mirror{0x22F2, 0x22FA}, Mirror{0x22F3, 0x22FB},
    Mirror{0x22F4, 0x22FC}, Mirror{0x22F6, 0x22FD}, Mirror{0x22F7, 0x22FE},
    Mirror{0x22FA, 0x22F2}, Mirror{0x22FB, 0x22F3}, Mirror{0x22FC, 0x22F4},
    Mirror{0x22FD, 0x22F6}, Mirror{0x22FE, 0x22F7}, Mirror{0x2308, 0x2309},
    Mirror{0x2309, 0x2308}, Mirror{0x230A, 0x230B}, Mirror{0x230B, 0x230A},
    Mirror{0x2329, 0x232A}, Mirror{0x232A, 0x2329}, Mirror{0x2768, 0x2769},
    Mirror{0x2769, 0x2768}, Mirror{0x276A, 0x276B}, Mirror{0x276B, 0x276A},
    Mirror{0x276C, 0x276D}, Mirror{0x276D, 0x276C}, Mirror{0x276E, 0x276F},
    Mirror{0x276F, 0x276E}, Mirror{0x2770, 0x2771}, Mirror{0x2771, 0x2770},
    Mirror{0x2772, 0x2773}, Mirror{0x2773, 0x2772}, Mirror{0x2774, 0x2775},
    Mirror{0x2775, 0x2774}, Mirror{0x27C3, 0x27C4}, Mirror{0x27C4, 0x27C3},
    Mirror{0x27C5, 0x27C6}, Mirror{0x27C6, 0x27C5}, Mirror{0x27C8, 0x27C9},
    Mirror{0x27C9, 0x27C8}, Mirror{0x27CB, 0x27CD}, Mirror{0x27CD, 0x27CB},
    Mirror{0x27D5, 0x27D6}, Mirror{0x27D6, 0x27D5}, Mirror{0x27DC, 0x22B8},
    Mirror{0x27DD, 0x27DE}, Mirror{0x27DE, 0x27DD}, Mirror{0x27E2, 0x27E3},
    Mirror{0x27E3, 0x27E2}, Mirror{0x27E4, 0x27E5}, Mirror{0x27E5, 0x27E4},
    Mirror{0x27E6, 0x27E7}, Mirror{0x27E7, 0x27E6}, Mirror{0x27E8, 0x27E9},
    Mirror{0x27E9, 0x27E8}, Mirror{0x27EA, 0x27EB}, Mirror{0x27EB, 0x27EA},
    Mirror{0x27EC, 0x27ED}, Mirror{0x27ED, 0x27EC}, Mirror{0x27EE, 0x27EF},
    Mirror{0x27EF, 0x27EE}, Mirror{0x2983, 0x2984}, Mirror{0x2984, 0x2983},
    Mirror{0x2985, 0x2986}, Mirror{0x2986, 0x2985}, Mirror{0x2987, 0x2988},
    Mirror{0x2988, 0x2987}, Mirror{0x2989, 0x298A}, Mirror{0x298A, 0x2989},
    Mirror{0x298B, 0x298C}, Mirror{0x298C, 0x298B}, Mirror{0x298D, 0x2990},
    Mirror{0x298E, 0x298F}, Mirror{0x298F, 0x298E}, Mirror{0x2990, 0x298D},
    Mirror{0x2991, 0x2992}, Mirror{0x2992, 0x2991}, Mirror{0x2993, 0x2994},
    Mirror{0x2994, 0x2993}, Mirror{0x2995, 0x2996}, Mirror{0x2996, 0x2995},
    Mirror{0x2997, 0x2998}, Mirror{0x2998, 0x2997}, Mirror{0x299B, 0x2221},
    Mirror{0x29A0, 0x2222}, Mirror{0x29A3, 0x2220}, Mirror{0x29A4, 0x29A5},
    Mirror{0x29A5, 0x29A4}, Mirror{0x29A8, 0x29A9}, Mirror{0x29A9, 0x29A8},
    Mirror{0x29AA, 0x29AB}, Mirror{0x29AB, 0x29AA}, Mirror{0x29AC, 0x29AD},
    Mirror{0x29AD, 0x29AC}, Mirror{0x29AE, 0x29AF}, Mirror{0x29AF, 0x29AE},
    Mirror{0x29B8, 0x2298}, Mirror{0x29C0, 0x29C1}, Mirror{0x29C1, 0x29C0},
    Mirror{0x29C4, 0x29C5}, Mirror{0x29C5, 0x29C4}, Mirror{0x29CF, 0x29D0},
    Mirror{0x29D0, 0x29CF}, Mirror{0x29D1, 0x29D2}, Mirror{0x29D2, 0x29D1},
    Mirror{0x29D4, 0x29D5}, Mirror{0x29D5, 0x29D4}, Mirror{0x29D8, 0x29D9},
    Mirror{0x29D9, 0x29D8}, Mirror{0x29DA, 0x29DB}, Mirror{0x29DB, 0x29DA},
    Mirror{0x29E8, 0x29E9}, Mirror{0x29E9, 0x29E8}, Mirror{0x29F5, 0x2215},
    Mirror{0x29F8, 0x29F9}, Mirror{0x29F9, 0x29F8}, Mirror{0x29FC, 0x29FD},
    Mirror{0x29FD, 0x29FC}, Mirror{0x2A2B, 0x2A2C}, Mirror{0x2A2C, 0x2A2B},
    Mirror{0x2A2D, 0x2A2E}, Mirror{0x2A2E, 0x2A2D}, Mirror{0x2A34, 0x2A35},
    Mirror{0x2A35, 0x2A34}, Mirror{0x2A3C, 0x2A3D}, Mirror{0x2A3D, 0x2A3C},
    Mirror{0x2A64, 0x2A65}, Mirror{0x2A65, 0x2A64}, Mirror{0x2A79, 0x2A7A},
    Mirror{0x2A7A, 0x2A79}, Mirror{0x2A7B, 0x2A7C}, Mirror{0x2A7C, 0x2A7B},
    Mirror{0x2A7D, 0x2A7E}, Mirror{0x2A7E, 0x2A7D}, Mirror{0x2A7F, 0x2A80},
    Mirror{0x2A80, 0x2A7F}, Mirror{0x2A81, 0x2A82}, Mirror{0x2A82, 0x2A81},
    Mirror{0x2A83, 0x2A84}, Mirror{0x2A84, 0x2A83}, Mirror{0x2A85, 0x2A86},
    Mirror{0x2A86, 0x2A85}, Mirror{0x2A87, 0x2A88}, Mirror{0x2A88, 0x2A87},
    Mirror{0x2A89, 0x2A8A}, Mirror{0x2A8A, 0x2A89}, Mirror{0x2A8B, 0x2A8C},
    Mirror{0x2A8C, 0x2A8B}, Mirror{0x2A8D, 0x2A8E}, Mirror{0x2A8E, 0x2A8D},
    Mirror{0x2A8F, 0x2A90}, Mirror{0x2A90, 0x2A8F}, Mirror{0x2A91, 0x2A92},
    Mirror{0x2A92, 0x2A91}, Mirror{0x2A93, 0x2A94}, Mirror{0x2A94, 0x2A93},
    Mirror{0x2A95, 0x2A96}, Mirror{0x2A96, 0x2A95}, Mirror{0x2A97, 0x2A98},
    Mirror{0x2A98, 0x2A97}, Mirror{0x2A99, 0x2A9A}, Mirror{0x2A9A, 0x2A99},
    Mirror{0x2A9B, 0x2A9C}, Mirror{0x2A9C, 0x2A9B}, Mirror{0x2A9D, 0x2A9E},
    Mirror{0x2A9E, 0x2A9D}, Mirror{0x2A9F, 0x2AA0}, Mirror{0x2AA0, 0x2A9F},
    Mirror{0x2AA1, 0x2AA2}, Mirror{0x2AA2, 0x2AA1}, Mirror{0x2AA6, 0x2AA7},
    Mirror{0x2AA7, 0x2AA6}, Mirror{0x2AA8, 0x2AA9}, Mirror{0x2AA9, 0x2AA8},
    Mirror{0x2AAA, 0x2AAB}, Mirror{0x2AAB, 0x2AAA}, Mirror{0x2AAC, 0x2AAD},
    Mirror{0x2AAD, 0x2AAC}, Mirror{0x2AAF, 0x2AB0}, Mirror{0x2AB0, 0x2AAF},
    Mirror{0x2AB1, 0x2AB2}, Mirror{0x2AB2, 0x2AB1}, Mirror{0x2AB3, 0x2AB4},
    Mirror{0x2AB4, 0x2AB3}, Mirror{0x2AB5, 0x2AB6}, Mirror{0x2AB6, 0x2AB5},
    Mirror{0x2AB7, 0x2AB8}, Mirror{0x2AB8, 0x2AB7}, Mirror{0x2AB9, 0x2ABA},
    Mirror{0x2ABA, 0x2AB9}, Mirror{0x2ABB, 0x2ABC}, Mirror{0x2ABC, 0x2ABB},
    Mirror{0x2ABD, 0x2ABE}, Mirror{0x2ABE, 0x2ABD}, Mirror{0x2ABF, 0x2AC0},
    Mirror{0x2AC0, 0x2ABF}, Mirror{0x2AC1, 0x2AC2}, Mirror{0x2AC2, 0x2AC1},
    Mirror{0x2AC3, 0x2AC4}, Mirror{0x2AC4, 0x2AC3}, Mirror{0x2AC5, 0x2AC6},
    Mirror{0x2AC6, 0x2AC5}, Mirror{0x2AC7, 0x2AC8}, Mirror{0x2AC8, 0x2AC7},
    Mirror{0x2AC9, 0x2ACA}, Mirror{0x2ACA, 0x2AC9}, Mirror{0x2ACB, 0x2ACC},
    Mirror{0x2ACC, 0x2ACB}, Mirror{0x2ACD, 0x2ACE}, Mirror{0x2ACE, 0x2ACD},
    Mirror{0x2ACF, 0x2AD0}, Mirror{0x2AD0, 0x2ACF}, Mirror{0x2AD1, 0x2AD2},
    Mirror{0x2AD2, 0x2AD1}, Mirror{0x2AD3, 0x2AD4}, Mirror{0x2AD4, 0x2AD3},
    Mirror{0x2AD5, 0x2AD6}, Mirror{0x2AD6, 0x2AD5}, Mirror{0x2ADE, 0x22A6},
    Mirror{0x2AE3, 0x22A9}, Mirror{0x2AE4, 0x22A8}, Mirror{0x2AE5, 0x22AB},
    Mirror{0x2AEC, 0x2AED}, Mirror{0x2AED, 0x2AEC}, Mirror{0x2AEE, 0x2224},
    Mirror{0x2AF7, 0x2AF8}, Mirror{0x2AF8, 0x2AF7}, Mirror{0x2AF9, 0x2AFA},
    Mirror{0x2AFA, 0x2AF9}, Mirror{0x2BFE, 0x221F}, Mirror{0x2E02, 0x2E03},
    Mirror{0x2E03, 0x2E02}, Mirror{0x2E04, 0x2E05}, Mirror{0x2E05, 0x2E04},
    Mirror{0x2E09, 0x2E0A}, Mirror{0x2E0A, 0x2E09}, Mirror{0x2E0C, 0x2E0D},
    Mirror{0x2E0D, 0x2E0C}, Mirror{0x2E1C, 0x2E1D}, Mirror{0x2E1D, 0x2E1C},
    Mirror{0x2E20, 0x2E21}, Mirror{0x2E21, 0x2E20}, Mirror{0x2E22, 0x2E23},
    Mirror{0x2E23, 0x2E22}, Mirror{0x2E24, 0x2E25}, Mirror{0x2E25, 0x2E24},
    Mirror{0x2E26, 0x2E27}, Mirror{0x2E27, 0x2E26}, Mirror{0x2E28, 0x2E29},
    Mirror{0x2E29, 0x2E28}, Mirror{0x2E55, 0x2E56}, Mirror{0x2E56, 0x2E55},
    Mirror{0x2E57, 0x2E58}, Mirror{0x2E58, 0x2E57}, Mirror{0x2E59, 0x2E5A},
    Mirror{0x2E5A, 0x2E59}, Mirror{0x2E5B, 0x2E5C}, Mirror{0x2E5C, 0x2E5B},
    Mirror{0x3008, 0x3009}, Mirror{0x3009, 0x3008}, Mirror{0x300A, 0x300B},
    Mirror{0x300B, 0x300A}, Mirror{0x300C, 0x300D}, Mirror{0x300D, 0x300C},
    Mirror{0x300E, 0x300F}, Mirror{0x300F, 0x300E}, Mirror{0x3010, 0x3011},
    Mirror{0x3011, 0x3010}, Mirror{0x3014, 0x3015}, Mirror{0x3015, 0x3014},
    Mirror{0x3016, 0x3017}, Mirror{0x3017, 0x3016}, Mirror{0x3018, 0x3019},
    Mirror{0x3019, 0x3018}, Mirror{0x301A, 0x301B}, Mirror{0x301B, 0x301A},
    Mirror{0xFE59, 0xFE5A}, Mirror{0xFE5A, 0xFE59}, Mirror{0xFE5B, 0xFE5C},
    Mirror{0xFE5C, 0xFE5B}, Mirror{0xFE5D, 0xFE5E}, Mirror{0xFE5E, 0xFE5D},
    Mirror{0xFE64, 0xFE65}, Mirror{0xFE65, 0xFE64}, Mirror{0xFF08, 0xFF09},
    Mirror{0xFF09, 0xFF08}, Mirror{0xFF1C, 0xFF1E}, Mirror{0xFF1E, 0xFF1C},
    Mirror{0xFF3B, 0xFF3D}, Mirror{0xFF3D, 0xFF3B}, Mirror{0xFF5B, 0xFF5D},
    Mirror{0xFF5D, 0xFF5B}, Mirror{0xFF5F, 0xFF60}, Mirror{0xFF60, 0xFF5F},
    Mirror{0xFF62, 0xFF63}, Mirror{0xFF63, 0xFF62},
}};


[[nodiscard]] inline auto classify(char32_t cp) noexcept -> Type {
    const auto found = std::lower_bound(types.begin(), types.end(), cp,
                                       [](Range range, char32_t value) { return range.last < value; });
    return found != types.end() && found->first <= cp ? found->type : Type::L;
}

[[nodiscard]] constexpr auto direction(unsigned level) noexcept -> Type {
    return level % 2U == 0 ? Type::L : Type::R;
}

[[nodiscard]] constexpr auto isolate(Type t) noexcept -> bool {
    return t == Type::LRI || t == Type::RLI || t == Type::FSI;
}

[[nodiscard]] constexpr auto removed(Type t) noexcept -> bool {
    return t == Type::BN || t == Type::LRE || t == Type::RLE ||
           t == Type::LRO || t == Type::RLO || t == Type::PDF;
}

[[nodiscard]] constexpr auto resettable(Type t) noexcept -> bool {
    return t == Type::WS || isolate(t) || t == Type::PDI || removed(t);
}

[[nodiscard]] constexpr auto neutral(Type t) noexcept -> bool {
    return t == Type::B || t == Type::S || t == Type::WS || t == Type::ON ||
           isolate(t) || t == Type::PDI;
}

[[nodiscard]] constexpr auto strong(Type t) noexcept -> Type {
    return t == Type::AN || t == Type::EN ? Type::R : t;
}

struct Character {
    char32_t cp{};
    Type original{};
    Type type{};
    Level level{};
    std::size_t partner{std::string_view::npos};
};

[[nodiscard]] inline auto paragraph_base(Span<const Character> paragraph) noexcept -> Level {
    std::size_t depth = 0;
    for (const auto& c : paragraph) {
        if (isolate(c.original)) ++depth;
        else if (c.original == Type::PDI && depth != 0) --depth;
        else if (depth == 0) {
            if (c.original == Type::L) return 0;
            if (c.original == Type::R || c.original == Type::AL) return 1;
        }
    }
    return 0;
}

[[nodiscard]] inline auto fsi_direction(Span<const Character> paragraph, std::size_t first,
                                        std::size_t end) noexcept -> Type {
    for (auto i = first; i < end; ++i) {
        const auto& c = paragraph[i];
        if (isolate(c.original)) {
            if (c.partner == std::string_view::npos) break;
            i = c.partner;
        } else if (c.original == Type::L) {
            return Type::LRI;
        } else if (c.original == Type::R || c.original == Type::AL) {
            return Type::RLI;
        }
    }
    return Type::LRI;
}

inline void explicit_levels(Span<Character> p, Level base) {
    constexpr auto none = std::string_view::npos;
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (isolate(p[i].original)) {
            open.push_back(i);
        } else if (p[i].original == Type::PDI && !open.empty()) {
            p[open.back()].partner = i;
            p[i].partner = open.back();
            open.pop_back();
        }
    }

    struct Status { Level level; Type override; bool isolate; };
    std::vector<Status> stack{{base, Type::ON, false}};
    std::size_t overflow_isolate = 0;
    std::size_t overflow_embedding = 0;
    std::size_t valid_isolate = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        auto& c = p[i];
        const auto t = c.original;
        const auto previous = stack.back();
        c.level = previous.level;
        if (isolate(t) || (removed(t) && t != Type::PDF && t != Type::BN)) {
            auto selected = t;
            const auto is_isolate = isolate(t);
            if (t == Type::FSI) {
                const auto end = c.partner == none ? p.size() : c.partner;
                selected = fsi_direction(p, i + 1, end);
            }
            const auto odd = selected == Type::RLE || selected == Type::RLO || selected == Type::RLI;
            // Smallest level strictly above the current level of the requested parity.
            const auto target = static_cast<unsigned>(previous.level) + 1U +
                ((previous.level % 2U == 0) != odd ? 1U : 0U);
            if (is_isolate && previous.override != Type::ON) c.type = previous.override;
            if (target <= 125 && overflow_isolate == 0 && overflow_embedding == 0) {
                const auto override = t == Type::LRO ? Type::L : t == Type::RLO ? Type::R : Type::ON;
                stack.push_back({static_cast<Level>(target), override, is_isolate});
                if (is_isolate) ++valid_isolate;
            } else if (is_isolate) {
                ++overflow_isolate;
            } else if (overflow_isolate == 0) {
                ++overflow_embedding;
            }
        } else if (t == Type::PDI) {
            if (overflow_isolate != 0) {
                --overflow_isolate;
            } else if (valid_isolate != 0) {
                overflow_embedding = 0;
                while (!stack.back().isolate) stack.pop_back();
                stack.pop_back();
                --valid_isolate;
            }
            c.level = stack.back().level;
            if (stack.back().override != Type::ON) c.type = stack.back().override;
        } else if (t == Type::PDF) {
            if (overflow_isolate == 0) {
                if (overflow_embedding != 0) --overflow_embedding;
                else if (!previous.isolate && stack.size() > 1) stack.pop_back();
            }
        } else if (t != Type::BN && t != Type::B && previous.override != Type::ON) {
            c.type = previous.override;
        }
    }
    if (!p.empty() && p.back().original == Type::B) p.back().level = base;
}

inline void weak_types(Span<Character> p, Span<const std::size_t> run, Type sos, Type eos) {
    auto previous = sos;
    auto nearest = sos;
    for (auto index : run) {
        auto& t = p[index].type;
        if (t == Type::NSM) t = isolate(previous) || previous == Type::PDI ? Type::ON : previous;
        previous = t;
        if (t == Type::EN && nearest == Type::AL) t = Type::AN;
        if (t == Type::L || t == Type::R || t == Type::AL) nearest = t;
        if (t == Type::AL) t = Type::R;
    }
    for (std::size_t i = 1; i + 1 < run.size(); ++i) {
        auto& t = p[run[i]].type;
        const auto left = p[run[i - 1]].type;
        const auto right = p[run[i + 1]].type;
        if (left == right && ((left == Type::EN && (t == Type::ES || t == Type::CS)) ||
                              (left == Type::AN && t == Type::CS))) t = left;
    }
    for (std::size_t i = 0; i < run.size();) {
        if (p[run[i]].type != Type::ET) { ++i; continue; }
        auto j = i + 1;
        while (j < run.size() && p[run[j]].type == Type::ET) ++j;
        const auto left = i == 0 ? sos : p[run[i - 1]].type;
        const auto right = j == run.size() ? eos : p[run[j]].type;
        if (left == Type::EN || right == Type::EN)
            for (auto k = i; k < j; ++k) p[run[k]].type = Type::EN;
        i = j;
    }
    nearest = sos;
    for (auto index : run) {
        auto& t = p[index].type;
        if (t == Type::ET || t == Type::ES || t == Type::CS) t = Type::ON;
        if (t == Type::EN && nearest == Type::L) t = Type::L;
        if (t == Type::L || t == Type::R) nearest = t;
    }
}

[[nodiscard]] constexpr auto comparable_bracket(char32_t cp) noexcept -> char32_t {
    if (cp == 0x2329) return 0x3008;
    if (cp == 0x232A) return 0x3009;
    return cp;
}

inline void bracket_types(Span<Character> p, Span<const std::size_t> run, Type sos, Type embedding) {
    struct Opening { char32_t mate; std::size_t index; };
    std::vector<Opening> stack;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t i = 0; i < run.size(); ++i) {
        const auto& c = p[run[i]];
        if (c.type != Type::ON) continue;
        const auto bracket = std::lower_bound(brackets.begin(), brackets.end(), c.cp,
            [](Bracket entry, char32_t cp) { return entry.scalar < cp; });
        if (bracket == brackets.end() || bracket->scalar != c.cp) continue;
        if (bracket->opening) {
            if (stack.size() == 63) break;
            stack.push_back({comparable_bracket(bracket->mate), i});
        } else {
            for (auto n = stack.size(); n > 0; --n) {
                if (stack[n - 1].mate != comparable_bracket(c.cp)) continue;
                pairs.emplace_back(stack[n - 1].index, i);
                stack.resize(n - 1);
                break;
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    const auto opposite = embedding == Type::L ? Type::R : Type::L;
    for (const auto& [left, right] : pairs) {
        bool contains_embedding = false;
        bool contains_opposite = false;
        for (auto i = left + 1; i < right && !contains_embedding; ++i) {
            const auto t = strong(p[run[i]].type);
            contains_embedding = t == embedding;
            contains_opposite = contains_opposite || t == opposite;
        }
        if (!contains_embedding && !contains_opposite) continue;
        auto selected = embedding;
        if (!contains_embedding) {
            auto preceding = sos;
            for (auto i = left; i > 0; --i) {
                const auto t = strong(p[run[i - 1]].type);
                if (t == Type::L || t == Type::R) { preceding = t; break; }
            }
            selected = preceding;
        }
        for (const auto i : {left, right}) {
            p[run[i]].type = selected;
            for (auto j = i + 1; j < run.size() && p[run[j]].original == Type::NSM; ++j)
                p[run[j]].type = selected;
        }
    }
}

inline void neutral_and_implicit(Span<Character> p, Span<const std::size_t> run,
                                 Type sos, Type eos, Level level) {
    for (std::size_t i = 0; i < run.size();) {
        if (!neutral(p[run[i]].type)) { ++i; continue; }
        auto j = i + 1;
        while (j < run.size() && neutral(p[run[j]].type)) ++j;
        const auto left = i == 0 ? sos : strong(p[run[i - 1]].type);
        const auto right = j == run.size() ? eos : strong(p[run[j]].type);
        for (auto k = i; k < j; ++k) p[run[k]].type = left == right ? left : direction(level);
        i = j;
    }
    for (auto index : run) {
        auto& c = p[index];
        if (c.type == Type::AN || c.type == Type::EN)
            c.level += level % 2 == 0 ? 2 : 1;
        else if (c.type != direction(level))
            ++c.level;
    }
}

inline void resolve_paragraph(Span<Character> p, Level base) {
    explicit_levels(p, base);
    std::vector<std::size_t> active;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (!removed(p[i].original)) active.push_back(i);

    struct Run { std::size_t first, end; Level level; };
    std::vector<Run> runs;
    std::vector<std::size_t> run_for(p.size());
    for (std::size_t i = 0; i < active.size(); ++i) {
        const auto level = p[active[i]].level;
        if (runs.empty() || runs.back().level != level) runs.push_back({i, i, level});
        runs.back().end = i + 1;
        run_for[active[i]] = runs.size() - 1;
    }

    std::vector<std::size_t> sequence;
    for (std::size_t nr = 0; nr < runs.size(); ++nr) {
        const auto start = runs[nr];
        const auto& initial = p[active[start.first]];
        if (initial.original == Type::PDI && initial.partner != std::string_view::npos) continue;
        sequence.clear();
        auto end = start;
        for (;;) {
            for (auto j = end.first; j < end.end; ++j) sequence.push_back(active[j]);
            const auto& tail = p[sequence.back()];
            if (!isolate(tail.original) || tail.partner == std::string_view::npos) break;
            end = runs[run_for[tail.partner]];
        }
        const auto sos = direction(std::max(start.level, start.first == 0 ? base :
                                           runs[nr - 1].level));
        const auto is_unmatched = isolate(p[sequence.back()].original);
        const auto eos = direction(std::max(end.level, end.end == active.size() || is_unmatched ? base :
                                           runs[run_for[active[end.end]]].level));
        weak_types(p, sequence, sos, eos);
        bracket_types(p, sequence, sos, direction(start.level));
        neutral_and_implicit(p, sequence, sos, eos, start.level);
    }

    // L1 resets trailing whitespace and whitespace before B/S.
    bool resetting = true;
    for (auto i = p.size(); i > 0; --i) {
        auto& c = p[i - 1];
        if (c.original == Type::B || c.original == Type::S) {
            c.level = base;
            resetting = true;
        } else if (resettable(c.original)) {
            if (resetting) c.level = base;
        } else {
            resetting = false;
        }
    }
}

[[nodiscard]] inline auto mirror(char32_t cp) noexcept -> char32_t {
    const auto found = std::lower_bound(mirrors.begin(), mirrors.end(), cp,
                                       [](Mirror entry, char32_t value) { return entry.scalar < value; });
    return found != mirrors.end() && found->scalar == cp ? found->mate : cp;
}

[[nodiscard]] constexpr auto retained_format(char32_t cp) noexcept -> bool {
    // X9 excludes these from level resolution, not from character shaping.
    return cp == 0x200C || cp == 0x200D || (cp >= 0xE0020 && cp <= 0xE007F);
}

}  // namespace detail

/** Detect the base direction of the first paragraph (UAX #9 P2–P3). */
[[nodiscard]] inline auto base_direction(std::string_view text) -> Level {
    std::size_t depth = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto cp = ttytk::detail::decode_utf8_scalar(text, offset);
        offset += cp.length;
        const auto t = detail::classify(cp.value);
        if (t == detail::Type::B) break;
        if (detail::isolate(t)) ++depth;
        else if (t == detail::Type::PDI && depth != 0) --depth;
        else if (depth == 0) {
            if (t == detail::Type::L) return 0;
            if (t == detail::Type::R || t == detail::Type::AL) return 1;
        }
    }
    return 0;
}

/** Resolve bidirectional embedding levels, one per decoded Unicode scalar. */
[[nodiscard]] inline auto resolve(std::string_view text) -> std::vector<Level> {
    std::vector<detail::Character> chars;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto cp = ttytk::detail::decode_utf8_scalar(text, offset);
        offset += cp.length;
        const auto type = detail::classify(cp.value);
        chars.push_back({cp.value, type, type});
    }
    const Span<detail::Character> all{chars};
    std::size_t start = 0;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (chars[i].original == detail::Type::B) {
            const auto paragraph = all.subspan(start, i + 1 - start);
            detail::resolve_paragraph(paragraph, detail::paragraph_base(paragraph));
            start = i + 1;
        }
    }
    const auto paragraph = all.subspan(start);
    detail::resolve_paragraph(paragraph, detail::paragraph_base(paragraph));
    std::vector<Level> result;
    result.reserve(chars.size());
    for (const auto& c : chars) result.push_back(c.level);
    return result;
}

/**
 * Reorder a single line using UAX #9 L2 and mirror odd-level characters.
 * Levels must correspond to decoded scalars; on mismatch, return the input
 * unchanged. For soft-wrapped paragraphs, pass the paragraph's levels after
 * applying L1 whitespace resets at this line boundary.
 */
[[nodiscard]] inline auto reorder_line(std::string text, const std::vector<Level>& levels) -> std::string {
    const auto chars = utf8::decode(text);
    if (chars.size() != levels.size()) return text;
    struct Glyph { std::size_t start, end; Level level; };
    std::vector<Glyph> glyphs;
    Level max_level = 0;
    Level min_odd = 255;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const auto type = detail::classify(chars[i]);
        if (detail::removed(type)) {
            if (!detail::retained_format(chars[i])) continue;
            if (!glyphs.empty() && glyphs.back().end == i) {
                glyphs.back().end = i + 1;
                continue;
            }
        }
        const auto modifier = chars[i] >= 0x1F3FB && chars[i] <= 0x1F3FF;
        const auto joined_symbol = i > 0 && !glyphs.empty() && chars[i - 1] == 0x200D &&
            type == detail::Type::ON && detail::classify(chars[glyphs.back().start]) == detail::Type::ON;
        if ((type == detail::Type::NSM || modifier || joined_symbol) && !glyphs.empty() &&
            glyphs.back().end == i && glyphs.back().level == levels[i]) {
            glyphs.back().end = i + 1;
        } else {
            glyphs.push_back({i, i + 1, levels[i]});
        }
        max_level = std::max(max_level, levels[i]);
        if ((levels[i] & 1U) != 0) min_odd = std::min(min_odd, levels[i]);
    }
    for (auto level = static_cast<int>(max_level); level >= min_odd; --level) {
        for (std::size_t i = 0; i < glyphs.size();) {
            if (glyphs[i].level < level) { ++i; continue; }
            auto j = i + 1;
            while (j < glyphs.size() && glyphs[j].level >= level) ++j;
            std::reverse(glyphs.begin() + static_cast<std::ptrdiff_t>(i),
                         glyphs.begin() + static_cast<std::ptrdiff_t>(j));
            i = j;
        }
    }
    std::string result;
    result.reserve(text.size());
    for (const auto& glyph : glyphs) {
        for (auto i = glyph.start; i < glyph.end; ++i) {
            const auto cp = chars[i];
            const auto type = detail::classify(cp);
            if (detail::isolate(type) || type == detail::Type::PDI ||
                cp == 0x200E || cp == 0x200F || cp == 0x061C) continue;
            utf8::detail::append(result, (glyph.level & 1U) != 0 ? detail::mirror(cp) : cp);
        }
    }
    return result;
}

}  // namespace ttytk::bidi

/*
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
*/
