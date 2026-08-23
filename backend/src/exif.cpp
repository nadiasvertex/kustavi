#include "exif.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kustavi::exif {

namespace {

/// Only the first bytes of a file can hold EXIF for our purposes; bounds the
/// read for very large files.
constexpr std::size_t k_max_read_bytes =
    static_cast<std::size_t>(8) * 1024 * 1024;

struct byte_view {
  std::span<const uint8_t> data;
  bool little_endian = true;

  [[nodiscard]] auto u16(std::size_t off) const -> std::optional<uint16_t> {
    if (off + 2 > data.size()) {
      return std::nullopt;
    }
    uint16_t v = static_cast<uint16_t>(data[off]) |
                 (static_cast<uint16_t>(data[off + 1]) << 8);
    if (!little_endian) {
      v = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
    return v;
  }

  [[nodiscard]] auto u32(std::size_t off) const -> std::optional<uint32_t> {
    if (off + 4 > data.size()) {
      return std::nullopt;
    }
    uint32_t v = 0;
    if (little_endian) {
      for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(data[off + i]) << (8 * i);
      }
    } else {
      for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(data[off + 3 - i]) << (8 * i);
      }
    }
    return v;
  }
};

struct ifd_entry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  std::size_t value_off = 0;
};

auto type_size(uint16_t type) -> std::size_t {
  switch (type) {
  case 1: // BYTE
  case 2: // ASCII
    return 1;
  case 3: // SHORT
    return 2;
  case 4: // LONG
    return 4;
  case 5: // RATIONAL
    return 8;
  default:
    return 0;
  }
}

/** Walks one IFD; `ifd_off` is absolute, sub-IFD offsets are relative to
 * `tiff_base`. */
auto read_ifd(const byte_view &view, std::size_t tiff_base, std::size_t ifd_off)
    -> std::vector<ifd_entry> {
  std::vector<ifd_entry> entries;
  const auto count = view.u16(ifd_off);
  if (!count) {
    return entries;
  }
  std::size_t off = ifd_off + 2;
  for (std::size_t i = 0; i < static_cast<std::size_t>(*count); ++i) {
    if (off + 12 > view.data.size()) {
      break;
    }
    const auto tag = view.u16(off);
    const auto type = view.u16(off + 2);
    const auto n = view.u32(off + 4);
    const auto val = view.u32(off + 8);
    if (!tag || !type || !n || !val) {
      off += 12;
      continue;
    }
    const auto elem_size = type_size(*type);
    if (elem_size == 0) {
      off += 12;
      continue;
    }
    ifd_entry entry;
    entry.tag = *tag;
    entry.type = *type;
    entry.count = *n;
    const std::size_t total = static_cast<std::size_t>(*n) * elem_size;
    entry.value_off = (total <= 4) ? off + 8 : tiff_base + *val;
    entries.push_back(entry);
    off += 12;
  }
  return entries;
}

auto find_entry(const std::vector<ifd_entry> &entries, uint16_t tag)
    -> const ifd_entry * {
  for (const auto &entry : entries) {
    if (entry.tag == tag) {
      return &entry;
    }
  }
  return nullptr;
}

auto read_ascii(const byte_view &view, const ifd_entry &entry)
    -> std::optional<std::string> {
  if (entry.type != 2 /* ASCII */) {
    return std::nullopt;
  }
  if (entry.value_off + entry.count > view.data.size()) {
    return std::nullopt;
  }
  std::string text;
  text.reserve(std::min<std::size_t>(entry.count, 32));
  for (std::size_t i = 0; i < entry.count; ++i) {
    const auto byte = view.data[entry.value_off + i];
    if (byte == 0) {
      break;
    }
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

/** Reads a single 32-bit pointer value (type LONG, count 1). */
auto read_u32_value(const byte_view &view, const ifd_entry &entry)
    -> std::optional<uint32_t> {
  if (entry.type != 4 /* LONG */ || entry.count != 1) {
    return std::nullopt;
  }
  return view.u32(entry.value_off);
}

/** Reads a GPS coordinate stored as three RATIONAL values (deg/min/sec). */
auto rational_degrees(const byte_view &view, const ifd_entry &entry)
    -> std::optional<double> {
  if (entry.type != 5 /* RATIONAL */ || entry.count != 3) {
    return std::nullopt;
  }
  if (entry.value_off + 24 > view.data.size()) {
    return std::nullopt;
  }
  double degrees = 0.0;
  std::size_t off = entry.value_off;
  for (int i = 0; i < 3; ++i, off += 8) {
    const auto num = view.u32(off);
    const auto den = view.u32(off + 4);
    if (!num || !den || *den == 0) {
      return std::nullopt;
    }
    const double value = static_cast<double>(*num) / static_cast<double>(*den);
    if (i == 0) {
      degrees = value;
    } else if (i == 1) {
      degrees += value / 60.0;
    } else {
      degrees += value / 3600.0;
    }
  }
  return degrees;
}

/** Parses "YYYY:MM:DD HH:MM:SS" (EXIF local time) into Unix milliseconds. */
auto parse_exif_datetime(const std::string &s) -> std::optional<std::int64_t> {
  if (s.size() < 19) {
    return std::nullopt;
  }
  const auto num = [&](std::size_t off, std::size_t len) -> std::optional<int> {
    int v = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const char c = s[off + i];
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const auto year = num(0, 4);
  const auto month = num(5, 2);
  const auto day = num(8, 2);
  const auto hour = num(11, 2);
  const auto minute = num(14, 2);
  const auto second = num(17, 2);
  if (!year || !month || !day || !hour || !minute || !second) {
    return std::nullopt;
  }
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 ||
      *minute > 59 || *second > 60) {
    return std::nullopt;
  }
  std::tm tm{};
  tm.tm_year = *year - 1900;
  tm.tm_mon = *month - 1;
  tm.tm_mday = *day;
  tm.tm_hour = *hour;
  tm.tm_min = *minute;
  tm.tm_sec = *second;
  tm.tm_isdst = -1;
  const std::time_t epoch = std::mktime(&tm);
  if (epoch < 0) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(epoch) * 1000;
}

/** Locates the TIFF block of a JPEG's EXIF APP1 segment. */
auto find_jpeg_tiff(std::span<const uint8_t> buf) -> std::size_t {
  constexpr std::size_t none = SIZE_MAX;
  if (buf.size() < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
    return none;
  }
  std::size_t off = 2;
  while (off + 4 <= buf.size()) {
    if (buf[off] != 0xFF) {
      return none;
    }
    const uint8_t marker = buf[off + 1];
    if (marker == 0xD8 /* SOI */ || marker == 0x01 /* TEM */ ||
        (marker >= 0xD0 && marker <= 0xD7 /* RSTn */)) {
      off += 2;
      continue;
    }
    const uint16_t len =
        static_cast<uint16_t>(static_cast<uint16_t>(buf[off + 2]) << 8) |
        static_cast<uint16_t>(buf[off + 3]);
    if (len < 2) {
      return none;
    }
    const std::size_t payload = off + 4;
    if (marker == 0xE1 /* APP1 */) {
      static constexpr std::array<uint8_t, 6> k_exif_header = {'E', 'x', 'i',
                                                               'f', 0,   0};
      if (payload + k_exif_header.size() <= buf.size() &&
          std::ranges::equal(buf.subspan(payload, k_exif_header.size()),
                             k_exif_header)) {
        return payload + k_exif_header.size();
      }
    }
    if (marker == 0xDA /* SOS */) {
      return none;
    }
    off = payload + (len - 2);
  }
  return none;
}

} // namespace

auto read_exif(const std::filesystem::path &path) -> exif_info {
  exif_info info;

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return info;
  }
  file.seekg(0, std::ios::end);
  const auto file_size = file.tellg();
  if (file_size <= 0) {
    return info;
  }
  const std::size_t want = static_cast<std::size_t>(std::min<std::int64_t>(
      file_size, static_cast<std::int64_t>(k_max_read_bytes)));
  std::vector<char> raw(want);
  file.seekg(0, std::ios::beg);
  file.read(raw.data(), static_cast<std::streamsize>(want));
  const auto got = static_cast<std::size_t>(file.gcount());
  if (got < 8) {
    return info;
  }
  std::vector<uint8_t> buf(got);
  std::copy_n(raw.begin(), static_cast<std::ptrdiff_t>(got), buf.begin());

  std::size_t tiff_base = SIZE_MAX;
  if (buf[0] == 0xFF && buf[1] == 0xD8) {
    tiff_base = find_jpeg_tiff(buf);
  } else if (got >= 4 && ((buf[0] == 'I' && buf[1] == 'I') ||
                          (buf[0] == 'M' && buf[1] == 'M'))) {
    tiff_base = 0;
  }
  if (tiff_base == SIZE_MAX || tiff_base + 8 > buf.size()) {
    return info;
  }

  const auto magic =
      static_cast<uint16_t>(static_cast<uint16_t>(buf[tiff_base + 2]) |
                            (static_cast<uint16_t>(buf[tiff_base + 3]) << 8));
  if (magic != 42) {
    return info;
  }
  const bool little = buf[tiff_base] == 'I';

  byte_view view{.data = buf, .little_endian = little};
  const auto ifd0_off = view.u32(tiff_base + 4);
  if (!ifd0_off) {
    return info;
  }
  const auto ifd0 = read_ifd(view, tiff_base, *ifd0_off);

  // Timestamp: ExifIFD.DateTimeOriginal, falling back to IFD0.DateTime.
  const auto *exif_ptr = find_entry(ifd0, 0x8769);
  std::vector<ifd_entry> exif_entries;
  if (exif_ptr != nullptr) {
    if (const auto off = read_u32_value(view, *exif_ptr)) {
      exif_entries = read_ifd(view, tiff_base, *off);
    }
  }
  const auto *datetime = find_entry(exif_entries, 0x9003);
  if (datetime == nullptr) {
    datetime = find_entry(ifd0, 0x0132);
  }
  if (datetime != nullptr) {
    if (const auto text = read_ascii(view, *datetime)) {
      info.taken_unix_ms = parse_exif_datetime(*text);
    }
  }

  // GPS: GPSIFD.{LatRef,Lat,LonRef,Lon}.
  const auto *gps_ptr = find_entry(ifd0, 0x8825);
  if (gps_ptr != nullptr) {
    if (const auto off = read_u32_value(view, *gps_ptr)) {
      const auto gps = read_ifd(view, tiff_base, *off);
      const auto *lat_ref = find_entry(gps, 0x0001);
      const auto *lat = find_entry(gps, 0x0002);
      const auto *lon_ref = find_entry(gps, 0x0003);
      const auto *lon = find_entry(gps, 0x0004);
      if ((lat_ref != nullptr) && (lat != nullptr)) {
        if (auto d = rational_degrees(view, *lat)) {
          auto ref = read_ascii(view, *lat_ref);
          if (ref && !ref->empty() && (*ref)[0] == 'S') {
            *d = -*d;
          }
          info.latitude = d;
        }
      }
      if ((lon_ref != nullptr) && (lon != nullptr)) {
        if (auto d = rational_degrees(view, *lon)) {
          auto ref = read_ascii(view, *lon_ref);
          if (ref && !ref->empty() && (*ref)[0] == 'W') {
            *d = -*d;
          }
          info.longitude = d;
        }
      }
    }
  }

  return info;
}
} // namespace kustavi::exif
