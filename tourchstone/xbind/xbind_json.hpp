#ifndef XBIND_JSON_HPP
#define XBIND_JSON_HPP

#include <string>
#include <string_view>

namespace xbind::detail {
inline std::string json_quote(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (size_t i = 0; i < value.size();) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c >= 128) {
      // Object names need not be UTF-8. Preserve valid sequences and escape
      // invalid bytes so the inventory is always parseable as JSON.
      const size_t length = c >= 0xf0 && c <= 0xf4 ? 4 :
                            c >= 0xe0 && c <= 0xef ? 3 :
                            c >= 0xc2 && c <= 0xdf ? 2 : 0;
      bool valid = length && value.size() - i >= length;
      if (valid) {
        const auto next = static_cast<unsigned char>(value[i + 1]);
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90))
          valid = false;
        for (size_t j = 1; j < length; ++j)
          valid &= (static_cast<unsigned char>(value[i + j]) & 0xc0) == 0x80;
      }
      if (valid) {
        out.append(value.substr(i, length));
        i += length;
        continue;
      }
    }
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32 || c >= 128) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += char(c);
    }
    ++i;
  }
  return out + '"';
}
}

#endif
