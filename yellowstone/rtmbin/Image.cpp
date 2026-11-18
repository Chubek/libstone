#include <rtmbin/rtmbin.hpp>

#include "qELF.hpp"
#include "qMachO.hpp"
#include "qPE.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace rtmbin {
namespace {

using qbfd::ObjectFile;
using u64 = std::uint64_t;
constexpr auto max_u64 = std::numeric_limits<u64>::max();

u64 address_limit(unsigned bits) noexcept {
  return bits == 32 ? std::numeric_limits<std::uint32_t>::max() : max_u64;
}

Error read_error(const qbfd::Error& error) {
  const auto code = error.code == qbfd::Error::Code::Unsupported
                        ? ErrorCode::Unsupported
                        : ErrorCode::Malformed;
  return {code, "qBFD: " + error.message};
}

qbfd::Expected<std::unique_ptr<ObjectFile>> open_image(std::span<const std::uint8_t> bytes) {
  if (qbfd::elf::ELFFile::sniff(bytes)) return qbfd::elf::ELFFile::open(bytes, {});
  if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z')
    return qbfd::pe::PEFile::open(bytes, {});
  if (qbfd::macho::MachOFile::sniff(bytes)) return qbfd::macho::MachOFile::open(bytes, {});
  return qbfd::Error{qbfd::Error::Code::Unsupported, "expected ELF, PE, or thin Mach-O"};
}

// qBFD exposes the validated command-table geometry, but not segment flags.
// SG_HIGHVM places the file span at the *end* of the VM range, which is not
// represented by our prefix mapping. Reject it rather than give wrong offsets.
bool has_highvm(const qbfd::macho::MachOFile& file, std::span<const std::uint8_t> bytes) {
  const auto layout = file.layout();
  const qbfd::detail::Reader reader{bytes, file.endian()};
  u64 offset = layout.commandTable;
  for (std::uint32_t i = 0; i < layout.commandCount; ++i) {
    const auto type = reader.u32(offset);
    if (type == 1 || type == 0x19) {
      constexpr auto sg_highvm = 0x1u;
      if (reader.u32(offset + (layout.wide ? 68 : 52)) & sg_highvm) return true;
    }
    offset += reader.u32(offset + 4);
  }
  return false;
}

Result<std::vector<Range>> image_ranges(const ObjectFile& file,
                                        std::span<const std::uint8_t> bytes,
                                        u64 address_max) {
  std::vector<Range> ranges;
  if (file.format() == qbfd::Format::PE) {
    const auto& pe = static_cast<const qbfd::pe::PEFile&>(file);
    const auto opt = pe.layout().fileHeader + qbfd::pe::k::FileHeaderSize;
    const u64 headers_size = qbfd::pe::Reader{bytes}.u32(opt + 60); // SizeOfHeaders
    const auto headers_end = pe.layout().sectionTable +
                             pe.rawSections().size() * qbfd::pe::k::SectionHeaderSize;
    if (headers_size < headers_end || headers_size > bytes.size() ||
        headers_size > pe.sizeOfImage())
      return Error{ErrorCode::Malformed, "invalid PE SizeOfHeaders"};
    if (pe.sizeOfImage() - 1 > address_max - pe.imageBase())
      return Error{ErrorCode::Malformed, "PE SizeOfImage exceeds address width"};
    for (const auto& s : pe.sections()) {
      if (s.size == 0) continue;
      const auto& raw = pe.rawSections()[s.index];
      if (raw.virtualAddress < headers_size || raw.virtualAddress >= pe.sizeOfImage() ||
          s.size > pe.sizeOfImage() - raw.virtualAddress)
        return Error{ErrorCode::Malformed, "PE section lies outside declared image payload"};
      if (s.fileSize && !(raw.characteristics & qbfd::pe::k::ScnUninitData) &&
          s.fileOffset < headers_size)
        return Error{ErrorCode::Malformed, "file-backed PE section overlaps image headers"};
      const auto file_size = (s.flags & qbfd::sec::Bss) ? 0 : s.fileSize;
      const auto file_offset = file_size ? s.fileOffset : 0;
      ranges.push_back({s.name, s.vma, s.size, file_offset, file_size,
                        (s.flags & qbfd::sec::Exec) != 0});
    }
  } else {
    const bool elf = file.format() == qbfd::Format::ELF;
    if (!elf && has_highvm(static_cast<const qbfd::macho::MachOFile&>(file), bytes))
      return Error{ErrorCode::Unsupported, "Mach-O SG_HIGHVM segments are unsupported"};
    for (std::size_t i = 0; i < file.segments().size(); ++i) {
      const auto& s = file.segments()[i];
      constexpr auto pt_load = 1u;
      if ((elf && s.nativeType != pt_load) || !s.memSize) continue;
      auto name = elf ? "PT_LOAD[" + std::to_string(i) + "]" : s.name;
      ranges.push_back({std::move(name), s.vaddr, s.memSize, s.fileOffset,
                        s.fileSize, (s.flags & qbfd::seg::Exec) != 0});
    }
  }
  if (ranges.empty()) return Error{ErrorCode::Unsupported, "image has no memory ranges"};

  for (const auto& r : ranges) {
    // Check the last byte, so an interval ending just beyond UINT64_MAX is
    // representable as (base, length), without unsigned end-address wrap.
    if (r.linked_address > address_max ||
        r.memory_size - 1 > address_max - r.linked_address ||
        r.file_size > r.memory_size || r.file_offset > bytes.size() ||
        r.file_size > bytes.size() - r.file_offset)
      return Error{ErrorCode::Malformed, "image range exceeds its address or file bounds"};
  }
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return a.linked_address < b.linked_address;
  });
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    const auto& previous = ranges[i - 1];
    if (ranges[i].linked_address - previous.linked_address < previous.memory_size)
      return Error{ErrorCode::Unsupported, "overlapping declared image memory ranges"};
  }
  return ranges;
}

} // namespace

std::string_view name(Format format) noexcept {
  switch (format) {
    case Format::ELF: return "ELF";
    case Format::PE: return "PE";
    case Format::MachO: return "Mach-O";
  }
  return "unknown";
}

Result<Image> Image::from_bytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > max_image_size)
    return Error{ErrorCode::Limit, "image exceeds the 256 MiB input limit"};
  if (qbfd::macho::isFat(bytes))
    return Error{ErrorCode::Unsupported, "universal Mach-O: supply a separate thin slice"};
  auto opened = open_image(bytes);
  if (!opened) return read_error(opened.error());
  const ObjectFile& object = **opened;
  if (object.fileType() != qbfd::FileType::Executable &&
      object.fileType() != qbfd::FileType::SharedObject)
    return Error{ErrorCode::Unsupported, "expected an executable or shared image"};

  Image image;
  switch (object.format()) {
    case qbfd::Format::ELF: image.format_ = Format::ELF; break;
    case qbfd::Format::PE: image.format_ = Format::PE; break;
    case qbfd::Format::MachO: image.format_ = Format::MachO; break;
    default: return Error{ErrorCode::Unsupported, "unsupported image format"};
  }
  image.architecture_ = qbfd::toString(object.arch());
  image.address_bits_ = object.is64Bit() ? 64u : 32u;
  image.preferred_base_ = object.imageBase();
  const auto limit = address_limit(image.address_bits_);
  if (image.preferred_base_ > limit)
    return Error{ErrorCode::Malformed, "preferred base exceeds image address width"};
  auto ranges = image_ranges(object, bytes, limit);
  if (const auto* error = std::get_if<Error>(&ranges)) return *error;
  image.ranges_ = std::get<std::vector<Range>>(std::move(ranges));
  return image;
}

Result<Image> Image::from_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return Error{ErrorCode::Io, "image input must be a readable regular file"};
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return Error{ErrorCode::Io, "could not open image file"};
  const auto length = input.tellg();
  if (length < 0) return Error{ErrorCode::Io, "could not determine image file size"};
  if (static_cast<u64>(length) > max_image_size)
    return Error{ErrorCode::Limit, "image exceeds the 256 MiB input limit"};
  input.seekg(0);
  if (!input) return Error{ErrorCode::Io, "could not rewind image file"};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size())))
    return Error{ErrorCode::Io, "could not read image file at its stated size"};
  if (input.peek() != std::char_traits<char>::eof() || input.bad())
    return Error{ErrorCode::Io, "could not read image file at its stated size"};
  return from_bytes(bytes);
}

Attribution Image::attribute(u64 pc, std::optional<u64> runtime_base) const noexcept {
  using S = AttributionStatus;
  const auto limit = address_limit(address_bits_);
  if (pc > limit || (runtime_base && (pc < *runtime_base ||
                                       pc - *runtime_base > limit - preferred_base_)))
    return {S::AddressOutOfRange, {}, {}, {}};
  const auto linked = runtime_base ? preferred_base_ + (pc - *runtime_base) : pc;
  const auto next = std::upper_bound(ranges_.begin(), ranges_.end(), linked,
                                     [](u64 address, const Range& range) {
    return address < range.linked_address;
  });
  if (next == ranges_.begin()) return {S::OutsideRanges, linked, {}, {}};
  const auto& range = *(next - 1);
  const auto delta = linked - range.linked_address;
  if (delta >= range.memory_size) return {S::OutsideRanges, linked, {}, {}};
  const auto index = static_cast<std::size_t>((next - 1) - ranges_.begin());
  if (!range.executable) return {S::NonExecutable, linked, index, {}};
  if (delta >= range.file_size) return {S::NotFileBacked, linked, index, {}};
  return {S::Attributed, linked, index, range.file_offset + delta};
}

Result<Profile> Image::profile(std::span<const Sample> samples,
                               std::optional<u64> runtime_base) const {
  Profile p;
  p.weight_by_range.resize(ranges_.size());
  for (const auto& sample : samples) {
    if (!sample.weight) return Error{ErrorCode::InvalidInput, "sample weight must be positive"};
    if (sample.weight > max_u64 - p.total_weight)
      return Error{ErrorCode::Overflow, "combined sample weight exceeds uint64_t"};
    p.total_weight += sample.weight;
    const auto a = attribute(sample.pc, runtime_base);
    // All bucket additions are bounded by the successfully checked total.
    switch (a.status) {
      case AttributionStatus::Attributed:
        p.weight_by_range[*a.range_index] += sample.weight;
        p.attributed_weight += sample.weight;
        break;
      case AttributionStatus::OutsideRanges: p.outside_ranges_weight += sample.weight; break;
      case AttributionStatus::NonExecutable: p.non_executable_weight += sample.weight; break;
      case AttributionStatus::NotFileBacked: p.not_file_backed_weight += sample.weight; break;
      case AttributionStatus::AddressOutOfRange: p.address_out_of_range_weight += sample.weight; break;
    }
  }
  return p;
}

Result<std::vector<VerifiedPoint>> Image::verification_plan(
    std::span<const PlanPoint> points, std::optional<u64> runtime_base) const {
  if (points.size() > max_plan_points)
    return Error{ErrorCode::Limit, "plan exceeds 10000 points"};
  std::set<std::string_view> names;
  std::vector<VerifiedPoint> result;
  result.reserve(points.size());
  constexpr std::string_view allowed =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";
  const auto limit = address_limit(address_bits_);
  for (const auto& point : points) {
    if (point.name.empty() || point.name.size() > 64 ||
        point.name.find_first_not_of(allowed) != std::string::npos)
      return Error{ErrorCode::InvalidInput, "invalid plan point name"};
    if (!names.insert(point.name).second)
      return Error{ErrorCode::InvalidInput, "duplicate plan point name: " + point.name};
    if (!point.size)
      return Error{ErrorCode::InvalidInput, "plan point size must be positive: " + point.name};
    const auto a = attribute(point.pc, runtime_base);
    if (!a.linked_pc || point.size - 1 > limit - point.pc ||
        point.size - 1 > limit - *a.linked_pc)
      return Error{ErrorCode::Overflow, "plan point exceeds address bounds: " + point.name};
    if (a.status != AttributionStatus::Attributed)
      return Error{ErrorCode::InvalidInput, "plan point is not file-backed executable: " + point.name};
    const auto& range = ranges_[*a.range_index];
    const auto bytes_remaining = range.file_size - (*a.file_offset - range.file_offset);
    if (point.size > bytes_remaining)
      return Error{ErrorCode::InvalidInput, "plan point span crosses a file-backed range end: " + point.name};
    result.push_back({point.name, *a.linked_pc, *a.file_offset, point.size, *a.range_index});
  }
  return result;
}

} // namespace rtmbin
