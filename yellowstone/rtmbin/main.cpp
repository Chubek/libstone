#include <rtmbin/rtmbin.hpp>

#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace {

using namespace rtmbin;
using u64 = std::uint64_t;

constexpr std::string_view usage =
  "Usage:\n"
  "  rtmbin layout IMAGE\n"
  "  rtmbin attribute IMAGE PC [--runtime-base BASE]\n"
  "  rtmbin profile IMAGE SAMPLES [--runtime-base BASE]\n"
  "  rtmbin plan IMAGE POINTS [--runtime-base BASE]\n"
  "  rtmbin graph MODEL\n"
  "\n"
  "SAMPLES: one PC [WEIGHT] per line. POINTS: one NAME PC [SIZE] per line.\n"
  "MODEL: one entry ID record and block ID [ID [ID]] records.\n"
  "Numbers are unsigned decimal or 0x-prefixed hex. Text inputs allow # comments.\n"
  "If BASE is omitted, PCs are linked virtual addresses. Images are never run or modified.\n";

std::optional<u64> number(std::string_view token) {
  int base = 10;
  if (token.starts_with("0x") || token.starts_with("0X")) {
    base = 16;
    token.remove_prefix(2);
  }
  if (token.empty()) return {};
  u64 value = 0;
  const auto end = token.data() + token.size();
  const auto parsed = std::from_chars(token.data(), end, value, base);
  if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
  return value;
}

// Binary names and parser diagnostics may contain terminal control bytes.
std::string escaped(std::string_view value) {
  std::string out;
  constexpr std::string_view hex = "0123456789abcdef";
  for (unsigned char ch : value) {
    if (ch == '\\' || ch == '"') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch < 0x20 || ch >= 0x7f) {
      out += "\\x";
      out += hex[ch >> 4];
      out += hex[ch & 0xf];
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out;
}

int error(std::string_view message) {
  std::cerr << "rtmbin: " << escaped(message) << '\n';
  return 1;
}

template <class ParseLine>
std::optional<Error> parse_lines(const std::filesystem::path& path, ParseLine parse_line) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return Error{ErrorCode::Io, "text input must be a readable regular file"};
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "could not open text input"};
  constexpr std::size_t max_text_size = 16u * 1024 * 1024;
  constexpr std::size_t max_line_size = 4096;
  std::string line;
  std::size_t line_no = 0, bytes = 0;
  // Read at most the declared limits, even if the file changes after opening.
  while (true) {
    line.clear();
    for (int ch; (ch = in.get()) != std::char_traits<char>::eof();) {
      if (++bytes > max_text_size)
        return Error{ErrorCode::Limit, "text input exceeds the 16 MiB limit"};
      if (ch == '\n') break;
      if (line.size() == max_line_size)
        return Error{ErrorCode::Limit, "text input line exceeds 4096 bytes"};
      line.push_back(static_cast<char>(ch));
    }
    if (in.bad() || (in.fail() && !in.eof())) return Error{ErrorCode::Io, "could not read text input"};
    if (line.empty() && in.eof()) break;
    ++line_no;
    if (const auto pos = line.find('#'); pos != std::string::npos) line.resize(pos);
    std::istringstream fields(line);
    if (auto err = parse_line(fields)) {
      err->message = "line " + std::to_string(line_no) + ": " + err->message;
      return err;
    }
  }
  return {};
}

Result<std::vector<Sample>> read_samples(const std::filesystem::path& path) {
  constexpr std::size_t max_samples = 100000;
  std::vector<Sample> samples;
  auto err = parse_lines(path, [&](std::istringstream& fields) -> std::optional<Error> {
    std::string pc, weight, extra;
    if (!(fields >> pc)) return {};
    const auto address = number(pc);
    auto count = fields >> weight ? number(weight) : std::optional<u64>(1);
    if (!address || !count || !*count || (fields >> extra))
      return Error{ErrorCode::InvalidInput, "expected PC and optional positive weight"};
    if (samples.size() == max_samples)
      return Error{ErrorCode::Limit, "sample input exceeds 100000 records"};
    samples.push_back({*address, *count});
    return {};
  });
  if (err) return *err;
  return samples;
}

Result<std::vector<PlanPoint>> read_plan(const std::filesystem::path& path) {
  std::vector<PlanPoint> points;
  auto err = parse_lines(path, [&](std::istringstream& fields) -> std::optional<Error> {
    std::string label, pc, size, extra;
    if (!(fields >> label)) return {};
    if (!(fields >> pc)) return Error{ErrorCode::InvalidInput, "missing plan point PC"};
    const auto address = number(pc);
    auto length = fields >> size ? number(size) : std::optional<u64>(1);
    if (!address || !length || !*length || (fields >> extra))
      return Error{ErrorCode::InvalidInput, "expected name, PC and optional positive size"};
    if (points.size() == max_plan_points)
      return Error{ErrorCode::Limit, "plan exceeds 10000 points"};
    points.push_back({std::move(label), *address, *length});
    return {};
  });
  if (err) return *err;
  return points;
}

Result<GraphSummary> read_graph(const std::filesystem::path& path) {
  std::vector<Block> blocks;
  std::optional<BlockId> entry;
  auto err = parse_lines(path, [&](std::istringstream& fields) -> std::optional<Error> {
    constexpr auto invalid = ErrorCode::InvalidInput;
    std::string record, token;
    if (!(fields >> record)) return {};
    if (!(fields >> token)) return Error{invalid, "missing block ID"};
    const auto parse_id = [](std::string_view text) -> std::optional<BlockId> {
      const auto value = number(text);
      if (!value || *value >= std::numeric_limits<BlockId>::max()) return {};
      return static_cast<BlockId>(*value);
    };
    const auto id = parse_id(token);
    if (!id) return Error{invalid, "invalid block ID"};
    if (record == "entry") {
      if (entry || (fields >> token))
        return Error{invalid, "expected exactly one entry ID"};
      entry = *id;
    } else if (record == "block") {
      if (blocks.size() == max_model_blocks)
        return Error{ErrorCode::Limit, "graph exceeds 1024 blocks"};
      Block block{*id, {}};
      while (fields >> token) {
        const auto successor = parse_id(token);
        if (!successor || block.successors.size() == 2)
          return Error{invalid, "expected at most two successor IDs"};
        block.successors.push_back(*successor);
      }
      blocks.push_back(std::move(block));
    } else {
      return Error{invalid, "expected entry or block record"};
    }
    return {};
  });
  if (err) return *err;
  if (!entry) return Error{ErrorCode::InvalidInput, "model has no entry record"};
  return summarize_graph(*entry, blocks);
}

void print_range_ids(std::string_view label, const std::vector<BlockId>& ids) {
  std::cout << label;
  for (auto id : ids) std::cout << ' ' << id;
  std::cout << '\n';
}

void print_image(const Image& image) {
  std::cout << "format=" << name(image.format()) << " arch=" << image.architecture()
            << " bits=" << image.address_bits() << " preferred_base=0x" << std::hex
            << image.preferred_base() << std::dec << '\n';
}

void print_ranges(const Image& image, const Profile* profile = nullptr) {
  for (std::size_t i = 0; i < image.ranges().size(); ++i) {
    const auto& r = image.ranges()[i];
    std::cout << "range=" << i << " name=\"" << escaped(r.name) << "\" address=0x"
              << std::hex << r.linked_address << " mem_size=0x" << r.memory_size
              << " file_offset=0x" << r.file_offset << " file_size=0x" << r.file_size
              << std::dec << " exec=" << (r.executable ? "yes" : "no");
    if (profile) std::cout << " weight=" << profile->weight_by_range[i];
    std::cout << '\n';
  }
}

std::string_view status_name(AttributionStatus status) {
  switch (status) {
    case AttributionStatus::Attributed: return "attributed";
    case AttributionStatus::OutsideRanges: return "outside_ranges";
    case AttributionStatus::NonExecutable: return "non_executable";
    case AttributionStatus::NotFileBacked: return "not_file_backed";
    case AttributionStatus::AddressOutOfRange: return "address_out_of_range";
  }
  return "unknown";
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  const auto bad_usage = [] {
    std::cerr << usage;
    return 2;
  };
  if (argc < 3) return bad_usage();
  const std::string_view command(argv[1]);
  if (command == "graph" && argc == 3) {
    auto result = read_graph(argv[2]);
    if (const auto* e = std::get_if<Error>(&result)) return error(e->message);
    const auto& report = std::get<GraphSummary>(result);
    print_range_ids("reachable:", report.reachable);
    print_range_ids("unreachable:", report.unreachable);
    for (const auto& cycle : report.reachable_cycles) print_range_ids("cycle:", cycle);
    return std::cout ? 0 : 1;
  }

  const bool is_layout = command == "layout" && argc == 3;
  const bool is_attribute = command == "attribute";
  const bool is_plan = command == "plan";
  if (!is_layout && (!(command == "profile" || is_attribute || is_plan) || (argc != 4 && argc != 6)))
    return bad_usage();
  std::optional<u64> runtime_base;
  if (argc == 6) {
    if (std::string_view(argv[4]) != "--runtime-base" || !(runtime_base = number(argv[5])))
      return bad_usage();
  }
  const auto pc = is_attribute ? number(argv[3]) : std::optional<u64>{};
  if (is_attribute && !pc) return bad_usage();

  auto loaded = Image::from_file(argv[2]);
  if (const auto* e = std::get_if<Error>(&loaded)) return error(e->message);
  const auto& image = std::get<Image>(loaded);
  if (is_layout) {
    print_image(image);
    print_ranges(image);
  } else if (is_attribute) {
    const auto a = image.attribute(*pc, runtime_base);
    std::cout << "status=" << status_name(a.status);
    if (a.linked_pc) std::cout << " linked_pc=0x" << std::hex << *a.linked_pc;
    if (a.range_index) std::cout << " range=" << std::dec << *a.range_index;
    if (a.file_offset) std::cout << " file_offset=0x" << std::hex << *a.file_offset;
    std::cout << std::dec << '\n';
  } else if (is_plan) {
    auto points = read_plan(argv[3]);
    if (const auto* e = std::get_if<Error>(&points)) return error(e->message);
    auto plan = image.verification_plan(std::get<std::vector<PlanPoint>>(points), runtime_base);
    if (const auto* e = std::get_if<Error>(&plan)) return error(e->message);
    print_image(image);
    for (const auto& point : std::get<std::vector<VerifiedPoint>>(plan)) {
      std::cout << "point=\"" << point.name << "\" range=" << point.range_index
                << " linked_pc=0x" << std::hex << point.linked_pc
                << " file_offset=0x" << point.file_offset << " size=0x" << point.size
                << std::dec << '\n';
    }
  } else {
    auto samples = read_samples(argv[3]);
    if (const auto* e = std::get_if<Error>(&samples)) return error(e->message);
    auto profile = image.profile(std::get<std::vector<Sample>>(samples), runtime_base);
    if (const auto* e = std::get_if<Error>(&profile)) return error(e->message);
    const auto& p = std::get<Profile>(profile);
    print_image(image);
    std::cout << "total=" << p.total_weight << " attributed=" << p.attributed_weight
              << " outside_ranges=" << p.outside_ranges_weight
              << " non_executable=" << p.non_executable_weight
              << " not_file_backed=" << p.not_file_backed_weight
              << " address_out_of_range=" << p.address_out_of_range_weight << '\n';
    print_ranges(image, &p);
  }
  return std::cout ? 0 : 1;
}
