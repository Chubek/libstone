#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace ttytk::terminfo {
namespace detail {

// Standard terminfo disk indices and their corresponding termcap codes.
// Consult terminfo(5)'s capability table and the ncurses term.h index order.
inline constexpr std::string_view bool_names =
    "bw am xsb xhp xenl eo gn hc km hs in da db mir msgr os eslok xt hz ul xon nxon mc5i chts nrrmc "
    "npc ndscr ccc bce hls xhpa crxm daisy xvpa sam cpix lpix OTbs OTns OTnc OTMT OTNL OTpt OTxr";
inline constexpr std::string_view bool_codes =
    "bw am xb xs xn eo gn hc km hs in da db mi ms os es xt hz ul xo nx 5i HC NR NP ND cc ut hl YA YB "
    "YC YD YE YF YG bs ns nc MT NL pt xr";
inline constexpr std::string_view num_names =
    "cols it lines lm xmc pb vt wsl nlab lh lw ma wnum colors pairs ncv bufsz spinv spinh maddr mjump "
    "mcs mls npins orc orl orhi orvi cps widcs btns bitwin bitype OTug OTdC OTdN OTdB OTdT OTkn";
inline constexpr std::string_view num_codes =
    "co it li lm sg pb vt ws Nl lh lw ma MW Co pa NC Ya Yb Yc Yd Ye Yf Yg Yh Yi Yj Yk Yl Ym Yn BT Yo "
    "Yp ug dC dN dB dT kn";
inline constexpr std::string_view str_names =
    "cbt bel cr csr tbc clear el ed hpa cmdch cup cud1 home civis cub1 mrcup cnorm cuf1 ll cuu1 cvvis "
    "dch1 dl1 dsl hd smacs blink bold smcup smdc dim smir invis prot rev smso smul ech rmacs sgr0 "
    "rmcup rmdc rmir rmso rmul flash ff fsl is1 is2 is3 if ich1 il1 ip kbs ktbc kclr kctab kdch1 kdl1 "
    "kcud1 krmir kel ked kf0 kf1 kf10 kf2 kf3 kf4 kf5 kf6 kf7 kf8 kf9 khome kich1 kil1 kcub1 kll knp "
    "kpp kcuf1 kind kri khts kcuu1 rmkx smkx lf0 lf1 lf10 lf2 lf3 lf4 lf5 lf6 lf7 lf8 lf9 rmm smm nel "
    "pad dch dl cud ich indn il cub cuf rin cuu pfkey pfloc pfx mc0 mc4 mc5 rep rs1 rs2 rs3 rf rc vpa "
    "sc ind ri sgr hts wind ht tsl uc hu iprog ka1 ka3 kb2 kc1 kc3 mc5p rmp acsc pln kcbt smxon rmxon "
    "smam rmam xonc xoffc enacs smln rmln kbeg kcan kclo kcmd kcpy kcrt kend kent kext kfnd khlp kmrk "
    "kmsg kmov knxt kopn kopt kprv kprt krdo kref krfr krpl krst kres ksav kspd kund kBEG kCAN kCMD "
    "kCPY kCRT kDC kDL kslt kEND kEOL kEXT kFND kHLP kHOM kIC kLFT kMSG kMOV kNXT kOPT kPRV kPRT kRDO "
    "kRPL kRIT kRES kSAV kSPD kUND rfi kf11 kf12 kf13 kf14 kf15 kf16 kf17 kf18 kf19 kf20 kf21 kf22 "
    "kf23 kf24 kf25 kf26 kf27 kf28 kf29 kf30 kf31 kf32 kf33 kf34 kf35 kf36 kf37 kf38 kf39 kf40 kf41 "
    "kf42 kf43 kf44 kf45 kf46 kf47 kf48 kf49 kf50 kf51 kf52 kf53 kf54 kf55 kf56 kf57 kf58 kf59 kf60 "
    "kf61 kf62 kf63 el1 mgc smgl smgr fln sclk dclk rmclk cwin wingo hup dial qdial tone pulse hook "
    "pause wait u0 u1 u2 u3 u4 u5 u6 u7 u8 u9 op oc initc initp scp setf setb cpi lpi chr cvr defc "
    "swidm sdrfq sitm slm smicm snlq snrmq sshm ssubm ssupm sum rwidm ritm rlm rmicm rshm rsubm rsupm "
    "rum mhpa mcud1 mcub1 mcuf1 mvpa mcuu1 porder mcud mcub mcuf mcuu scs smgb smgbp smglp smgrp smgt "
    "smgtp sbim scsd rbim rcsd subcs supcs docr zerom csnm kmous minfo reqmp getm setaf setab pfxl "
    "devt csin s0ds s1ds s2ds s3ds smglr smgtb birep binel bicr colornm defbi endbi setcolor slines "
    "dispc smpch rmpch smsc rmsc pctrm scesc scesa ehhlm elhlm elohlm erhlm ethlm evhlm sgr1 slength "
    "OTi2 OTrs OTnl OTbc OTko OTma OTG2 OTG3 OTG1 OTG4 OTGR OTGL OTGU OTGD OTGH OTGV OTGC meml memu "
    "box1";
inline constexpr std::string_view str_codes =
    "bt bl cr cs ct cl ce cd ch CC cm do ho vi le CM ve nd ll up vs dc dl ds hd as mb md ti dm mh im "
    "mk mp mr so us ec ae me te ed ei se ue vb ff fs i1 is i3 if ic al ip kb ka kC kt kD kL kd kM kE "
    "kS k0 k1 k; k2 k3 k4 k5 k6 k7 k8 k9 kh kI kA kl kH kN kP kr kF kR kT ku ke ks l0 l1 la l2 l3 l4 "
    "l5 l6 l7 l8 l9 mo mm nw pc DC DL DO IC SF AL LE RI SR UP pk pl px ps pf po rp r1 r2 r3 rf rc cv "
    "sc sf sr sa st wi ta ts uc hu iP K1 K3 K2 K4 K5 pO rP ac pn kB SX RX SA RA XN XF eA LO LF @1 @2 "
    "@3 @4 @5 @6 @7 @8 @9 @0 %1 %2 %3 %4 %5 %6 %7 %8 %9 %0 &1 &2 &3 &4 &5 &6 &7 &8 &9 &0 *1 *2 *3 *4 "
    "*5 *6 *7 *8 *9 *0 #1 #2 #3 #4 %a %b %c %d %e %f %g %h %i %j !1 !2 !3 RF F1 F2 F3 F4 F5 F6 F7 F8 "
    "F9 FA FB FC FD FE FF FG FH FI FJ FK FL FM FN FO FP FQ FR FS FT FU FV FW FX FY FZ Fa Fb Fc Fd Fe "
    "Ff Fg Fh Fi Fj Fk Fl Fm Fn Fo Fp Fq Fr cb MC ML MR Lf SC DK RC CW WG HU DI QD TO PU fh PA WA u0 "
    "u1 u2 u3 u4 u5 u6 u7 u8 u9 op oc Ic Ip sp Sf Sb ZA ZB ZC ZD ZE ZF ZG ZH ZI ZJ ZK ZL ZM ZN ZO ZP "
    "ZQ ZR ZS ZT ZU ZV ZW ZX ZY ZZ Za Zb Zc Zd Ze Zf Zg Zh Zi Zj Zk Zl Zm Zn Zo Zp Zq Zr Zs Zt Zu Zv "
    "Zw Zx Zy Km Mi RQ Gm AF AB xl dv ci s0 s1 s2 s3 ML MT Xy Zz Yv Yw Yx Yy Yz YZ S1 S2 S3 S4 S5 S6 "
    "S7 S8 Xh Xl Xo Xr Xt Xv sA YI i2 rs nl bc ko ma G2 G3 G1 G4 GR GL GU GD GH GV GC ml mu bx";

inline auto next_name(std::string_view& names) -> std::string_view {
    const auto end = names.find(' ');
    const auto name = names.substr(0, end);
    names.remove_prefix(end == std::string_view::npos ? names.size() : end + 1);
    return name;
}

inline auto index_of(std::string_view names, const std::string_view name) -> std::size_t {
    for (std::size_t i = 0; !names.empty(); ++i)
        if (next_name(names) == name) return i;
    return std::string_view::npos;
}

}  // namespace detail

/** Parsed terminal description with standard slot arrays and named extensions. */
struct Entry {
    std::string names{};
    std::vector<bool> flags{};
    std::vector<int> numbers{};
    std::vector<std::optional<std::string>> strings{};
    std::unordered_map<std::string, bool> extended_flags{};
    std::unordered_map<std::string, int> extended_numbers{};
    std::unordered_map<std::string, std::optional<std::string>> extended_strings{};

    [[nodiscard]] auto flag(const std::size_t index) const -> bool {
        return index < flags.size() && flags[index];
    }
    [[nodiscard]] auto num(const std::size_t index) const -> std::optional<int> {
        if (index >= numbers.size() || numbers[index] < 0) return std::nullopt;
        return numbers[index];
    }
    [[nodiscard]] auto str(const std::size_t index) const -> std::optional<std::string_view> {
        if (index >= strings.size() || !strings[index]) return std::nullopt;
        return *strings[index];
    }
    [[nodiscard]] auto flag(const std::string_view name) const -> bool {
        const auto it = extended_flags.find(std::string{name});
        return it != extended_flags.end() ? it->second : flag(detail::index_of(detail::bool_names, name));
    }
    [[nodiscard]] auto num(const std::string_view name) const -> std::optional<int> {
        const auto it = extended_numbers.find(std::string{name});
        if (it == extended_numbers.end()) return num(detail::index_of(detail::num_names, name));
        return it->second < 0 ? std::nullopt : std::optional{it->second};
    }
    [[nodiscard]] auto str(const std::string_view name) const -> std::optional<std::string_view> {
        const auto it = extended_strings.find(std::string{name});
        if (it == extended_strings.end()) return str(detail::index_of(detail::str_names, name));
        return it->second ? std::optional<std::string_view>{*it->second} : std::nullopt;
    }
};

namespace detail {
struct Reader {
    std::string_view bytes;
    std::size_t pos{};
    bool valid{true};

    auto take(const std::size_t count) -> std::string_view {
        if (!valid || count > bytes.size() - pos) {
            valid = false;
            return {};
        }
        const auto value = bytes.substr(pos, count);
        pos += count;
        return value;
    }
    void align() { if (pos % 2 != 0) take(1); }
    auto word(const unsigned width = 2) -> int {
        const auto value = take(width);
        if (!valid) return 0;
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < width; ++i)
            bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[i])) << (8U * i);
        if (width == 2 && (bits & 0x8000U)) bits |= 0xffff0000U;
        return std::bit_cast<std::int32_t>(bits);
    }
    auto count() -> std::size_t {
        const auto n = word();
        if (n < 0) valid = false;
        return n < 0 ? 0 : static_cast<std::size_t>(n);
    }
};

inline auto table_string(const std::string_view table, const int offset) -> std::optional<std::string_view> {
    if (offset < 0 || static_cast<std::size_t>(offset) >= table.size()) return std::nullopt;
    const auto end = table.find('\0', static_cast<std::size_t>(offset));
    if (end == std::string_view::npos) return std::nullopt;
    return table.substr(static_cast<std::size_t>(offset), end - static_cast<std::size_t>(offset));
}

inline auto read_words(Reader& reader, const std::size_t count, const unsigned width = 2)
    -> std::vector<int> {
    std::vector<int> out;
    if (!reader.valid || count > (reader.bytes.size() - reader.pos) / width) {
        reader.valid = false;
        return out;
    }
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(reader.word(width));
    return out;
}
}  // namespace detail

/** Parses a legacy or ncurses extended compiled entry without linking curses. */
[[nodiscard]] inline auto load_from(const std::string_view data) -> Result<Entry> {
    detail::Reader reader{data};
    const auto magic = reader.word();
    const auto names_size = reader.count();
    const auto bool_count = reader.count();
    const auto num_count = reader.count();
    const auto str_count = reader.count();
    const auto table_size = reader.count();
    if (!reader.valid || (magic != 0432 && magic != 01036))
        return Error{EINVAL, "invalid terminfo header"};
    const auto width = magic == 01036 ? 4U : 2U;
    const auto names = reader.take(names_size);
    if (names.size() < 2 || names.find('\0') != names.size() - 1)
        return Error{EINVAL, "invalid terminfo names"};
    Entry entry;
    entry.names = names.substr(0, names.size() - 1);
    for (const auto byte : reader.take(bool_count)) entry.flags.push_back(byte == '\1');
    if (reader.pos != data.size() || num_count != 0 || str_count != 0 || table_size != 0)
        reader.align();
    entry.numbers = detail::read_words(reader, num_count, width);
    const auto offsets = detail::read_words(reader, str_count);
    const auto table = reader.take(table_size);
    if (!reader.valid) return Error{EINVAL, "truncated terminfo entry"};
    for (const auto offset : offsets) {
        const auto value = detail::table_string(table, offset);
        if (!value && offset != -1 && offset != -2) return Error{EINVAL, "invalid terminfo string offset"};
        entry.strings.emplace_back(value ? std::optional<std::string>{*value} : std::nullopt);
    }
    if (reader.pos == data.size()) return entry;
    reader.align();
    if (reader.pos == data.size()) return entry;

    const auto ext_flags = reader.count();
    const auto ext_numbers = reader.count();
    const auto ext_strings = reader.count();
    const auto ext_items = reader.count();
    const auto ext_size = reader.count();
    const auto ext_flags_data = reader.take(ext_flags);
    reader.align();
    const auto ext_num_data = detail::read_words(reader, ext_numbers, width);
    const auto ext_names = ext_flags + ext_numbers + ext_strings;
    const auto ext_offsets = detail::read_words(reader, ext_strings + ext_names);
    const auto ext_table = reader.take(ext_size);
    if (!reader.valid || reader.pos != data.size())
        return Error{EINVAL, "invalid extended terminfo section"};

    std::size_t names_base = 0, present = 0;
    std::vector<std::optional<std::string_view>> values;
    for (std::size_t i = 0; i < ext_strings; ++i) {
        const auto offset = ext_offsets[i];
        const auto value = detail::table_string(ext_table, offset);
        if (!value && offset != -1 && offset != -2)
            return Error{EINVAL, "invalid extended terminfo string offset"};
        if (value) {
            ++present;
            names_base = std::max(names_base, static_cast<std::size_t>(offset) + value->size() + 1);
        }
        values.push_back(value);
    }
    if (ext_items != ext_names + present)
        return Error{EINVAL, "invalid extended terminfo item count"};
    for (std::size_t i = 0; i < ext_names; ++i) {
        const auto name = detail::table_string(ext_table.substr(names_base), ext_offsets[ext_strings + i]);
        if (!name || name->empty()) return Error{EINVAL, "invalid extended terminfo capability name"};
        std::string key{*name};
        if (i < ext_flags) entry.extended_flags.emplace(std::move(key), ext_flags_data[i] == '\1');
        else if (i - ext_flags < ext_numbers)
            entry.extended_numbers.emplace(std::move(key), ext_num_data[i - ext_flags]);
        else {
            const auto value = values[i - ext_flags - ext_numbers];
            entry.extended_strings.emplace(std::move(key), value ? std::optional<std::string>{*value} : std::nullopt);
        }
    }
    return entry;
}

/** Lists environment-specified directories followed by system defaults. */
[[nodiscard]] inline auto search_paths() -> std::vector<std::string> {
    constexpr std::string_view default_path = "/usr/share/terminfo";
    std::vector<std::string> paths;
    if (const char* env = std::getenv("TERMINFO"); env && *env) paths.emplace_back(env);
    if (const char* home = std::getenv("HOME"); home && *home) paths.emplace_back(std::string{home} + "/.terminfo");
    if (const char* dirs = std::getenv("TERMINFO_DIRS"); dirs != nullptr) {
        std::string_view remaining{dirs};
        for (;;) {
            const auto end = remaining.find(':');
            const auto path = remaining.substr(0, end);
            paths.emplace_back(path.empty() ? default_path : path);
            if (end == std::string_view::npos) break;
            remaining.remove_prefix(end + 1);
        }
    }
    paths.emplace_back("/etc/terminfo");
    paths.emplace_back("/lib/terminfo");
    paths.emplace_back(default_path);
    return paths;
}

/** Loads a compiled entry from either character or hexadecimal directory trees. */
[[nodiscard]] inline auto load(const std::string& name) -> Result<Entry> {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
        return Error{EINVAL, "invalid terminal name"};
    constexpr std::string_view hex = "0123456789abcdef";
    const auto first = static_cast<unsigned char>(name.front());
    const std::string directories[]{std::string(1, name.front()), {hex[first >> 4], hex[first & 15]}};
    std::optional<Error> failure;
    for (const auto& path : search_paths()) {
        for (const auto& directory : directories) {
            const auto file = std::filesystem::path(path) / directory / name;
            std::error_code error;
            if (!std::filesystem::is_regular_file(file, error)) continue;
            std::ifstream stream(file, std::ios::binary);
            if (!stream) continue;
            std::string data(32769, '\0');
            stream.read(data.data(), static_cast<std::streamsize>(data.size()));
            if (stream.bad() || stream.gcount() >= static_cast<std::streamsize>(data.size())) {
                failure = Error{EINVAL, "unable to read compiled terminfo entry"};
                continue;
            }
            data.resize(static_cast<std::size_t>(stream.gcount()));
            auto entry = load_from(data);
            if (entry) return entry;
            failure = entry.error();
        }
    }
    return failure ? *failure : Error{ENOENT, "terminfo entry not found"};
}

namespace detail {
using Parameter = std::variant<std::int64_t, std::string>;

inline auto number(const Parameter& p) -> std::int64_t {
    if (const auto* n = std::get_if<std::int64_t>(&p)) return *n;
    return 0;
}
inline auto wrap(const std::uint64_t bits) -> std::int64_t { return std::bit_cast<std::int64_t>(bits); }
inline auto binary(const char op, const std::int64_t a, const std::int64_t b) -> std::int64_t {
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    switch (op) {
        case '+': return wrap(ua + ub);
        case '-': return wrap(ua - ub);
        case '*': return wrap(ua * ub);
        case '/': return b == 0 ? 0 : a == std::numeric_limits<std::int64_t>::min() && b == -1 ? a : a / b;
        case 'm': return b == 0 || (a == std::numeric_limits<std::int64_t>::min() && b == -1) ? 0 : a % b;
        case '&': return a & b;
        case '|': return a | b;
        case '^': return a ^ b;
        case '=': return a == b;
        case '>': return a > b;
        case '<': return a < b;
        case 'A': return a != 0 && b != 0;
        case 'O': return a != 0 || b != 0;
        default: return 0;
    }
}

inline auto expand(const std::string_view pattern, std::array<Parameter, 9> parameters) -> std::string {
    std::string out;
    std::vector<Parameter> stack;
    std::array<Parameter, 26> dynamic{};
    thread_local std::array<Parameter, 26> persistent{};
    struct Condition { bool enclosing; bool selected; };
    std::vector<Condition> conditions;
    bool active = true;
    const auto pop = [&]() -> Parameter {
        if (stack.empty()) return std::int64_t{0};
        auto value = std::move(stack.back());
        stack.pop_back();
        return value;
    };
    for (std::size_t i = 0; i < pattern.size();) {
        const char next = pattern[i++];
        if (next != '%' || i == pattern.size()) {
            if (active) out += next;
            continue;
        }
        const char op = pattern[i++];
        switch (op) {
            case '?': conditions.push_back({active, false}); continue;
            case 't':
                if (!conditions.empty()) {
                    auto& condition = conditions.back();
                    active = active && !condition.selected && number(pop()) != 0;
                    condition.selected = condition.selected || active;
                }
                continue;
            case 'e':
                if (!conditions.empty()) active = conditions.back().enclosing && !conditions.back().selected;
                continue;
            case ';':
                if (!conditions.empty()) {
                    active = conditions.back().enclosing;
                    conditions.pop_back();
                }
                continue;
            case 'p':
                if (i < pattern.size() && pattern[i] >= '1' && pattern[i] <= '9') {
                    if (active) stack.push_back(parameters[pattern[i] - '1']);
                    ++i;
                }
                continue;
            case 'P': case 'g':
                if (i < pattern.size()) {
                    const char name = pattern[i++];
                    Parameter* target = name >= 'a' && name <= 'z' ? &dynamic[name - 'a'] :
                                        name >= 'A' && name <= 'Z' ? &persistent[name - 'A'] : nullptr;
                    if (active && target) {
                        if (op == 'P') *target = pop();
                        else stack.push_back(*target);
                    }
                }
                continue;
            case '\'':
                if (pattern.size() - i >= 2 && pattern[i + 1] == '\'') {
                    if (active) stack.emplace_back(static_cast<std::int64_t>(static_cast<unsigned char>(pattern[i])));
                    i += 2;
                }
                continue;
            case '{': {
                const auto end = pattern.find('}', i);
                if (end == std::string_view::npos) return out;
                if (active) {
                    std::int64_t constant = 0;
                    const auto parsed = std::from_chars(pattern.data() + i, pattern.data() + end, constant);
                    if (parsed.ec == std::errc{} && parsed.ptr == pattern.data() + end)
                        stack.emplace_back(constant);
                }
                i = end + 1;
                continue;
            }
            default: break;
        }
        if (!active) continue;
        if (op == 'i') {
            for (unsigned k = 0; k < 2; ++k) parameters[k] = binary('+', number(parameters[k]), 1);
        } else if (op == '%') out += '%';
        else if (op == 'c') out += static_cast<char>(number(pop()));
        else if (op == 'l') {
            const auto value = pop();
            const auto* text = std::get_if<std::string>(&value);
            stack.emplace_back(text ? static_cast<std::int64_t>(text->size()) : 0);
        } else if (op == '!' || op == '~') {
            const auto value = number(pop());
            stack.emplace_back(op == '!' ? static_cast<std::int64_t>(value == 0) : ~value);
        } else if (std::string_view{"+-*/m&|^=<>AO"}.find(op) != std::string_view::npos) {
            const auto right = number(pop());
            stack.emplace_back(binary(op, number(pop()), right));
        } else {
            // Accept only documented printf fields; do not forward arbitrary directives.
            auto end = i - 1 + (op == ':' ? 1U : 0U);
            const auto start = end;
            const auto is_field = [](const char c) {
                return std::string_view{"-+# .0123456789"}.find(c) != std::string_view::npos;
            };
            while (end < pattern.size() && is_field(pattern[end])) ++end;
            if (end == pattern.size() || std::string_view{"doxXs"}.find(pattern[end]) == std::string_view::npos)
                continue;
            i = end + 1;
            const auto fields = pattern.substr(start, end - start);
            auto digit = fields.find_first_of("0123456789");
            bool bounded = true;
            while (digit != std::string_view::npos) {
                unsigned size = 0;
                const auto parsed = std::from_chars(fields.data() + digit, fields.data() + fields.size(), size);
                if (parsed.ec != std::errc{} || size > 4096) { bounded = false; break; }
                digit = fields.find_first_of("0123456789", static_cast<std::size_t>(parsed.ptr - fields.data()));
            }
            // Validate flags, optional width, and optional precision individually.
            auto field = fields.find_first_not_of("-+# 0");
            if (field == std::string_view::npos) field = fields.size();
            const auto flags = fields.substr(0, field);
            while (field < fields.size() && fields[field] >= '0' && fields[field] <= '9') ++field;
            if (field < fields.size() && fields[field] == '.') {
                ++field;
                while (field < fields.size() && fields[field] >= '0' && fields[field] <= '9') ++field;
            }
            const char conversion = pattern[end];
            if (!bounded || field != fields.size() ||
                (conversion == 's' && flags.find_first_of("+# 0") != std::string_view::npos) ||
                (conversion == 'd' && flags.find('#') != std::string_view::npos) ||
                (conversion != 'd' && flags.find_first_of("+ ") != std::string_view::npos)) continue;
            std::string format{'%'};
            format += fields;
            if (conversion != 's') format += "ll";
            format += conversion;
            const auto value = pop();
            const auto print = [&](const auto argument) {
                const int count = std::snprintf(nullptr, 0, format.c_str(), argument);
                if (count < 0) return;
                const auto old = out.size();
                out.resize(old + static_cast<std::size_t>(count));
                std::snprintf(out.data() + old, static_cast<std::size_t>(count) + 1, format.c_str(), argument);
            };
            if (conversion == 's') {
                const auto* string = std::get_if<std::string>(&value);
                print(string ? string->c_str() : "");
            } else if (conversion == 'd') print(static_cast<long long>(number(value)));
            else print(static_cast<unsigned long long>(number(value)));
        }
    }
    return out;
}
}  // namespace detail

/** Expands a terminfo capability using numeric parameters. */
[[nodiscard]] inline auto tparm(const std::string_view pattern, const std::vector<int>& values) -> std::string {
    std::array<detail::Parameter, 9> parameters{};
    for (std::size_t i = 0; i < std::min(values.size(), parameters.size()); ++i)
        parameters[i] = static_cast<std::int64_t>(values[i]);
    return detail::expand(pattern, std::move(parameters));
}

/** Expands a capability with up to nine integer or string parameters. */
template <typename... Args>
    requires (sizeof...(Args) <= 9 &&
              ((std::is_integral_v<std::remove_cvref_t<Args>> || std::is_convertible_v<Args, std::string_view>) && ...))
[[nodiscard]] inline auto tparm(const std::string_view pattern, Args&&... values) -> std::string {
    const auto convert = [](auto&& value) -> detail::Parameter {
        if constexpr (std::is_integral_v<std::remove_cvref_t<decltype(value)>>)
            return static_cast<std::int64_t>(value);
        else
            return std::string{std::string_view{value}};
    };
    return detail::expand(pattern, {convert(std::forward<Args>(values))...});
}

}  // namespace ttytk::terminfo
