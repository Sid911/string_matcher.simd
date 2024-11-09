
#include <cstdint>
#include <experimental/bits/simd.h>
#include <experimental/simd>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <uchar.h>

namespace stdx = std::experimental;

using u8string      = std::basic_string<char8_t>;
using u8string_view = std::basic_string_view<char8_t>;

template<typename T, typename... Args>
void measureTime(const T &lines,
  int n,
  const std::function<void(const T &)> &func,
  const std::string &version,
  Args... args) {

  auto start = std::chrono::high_resolution_clock::now();

  func(lines, args...);

  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> duration = (end - start) / n;
  std::cout << "Time taken in " << version << " version: " << duration.count() << " ms" << std::endl;
}

//! ---------------------------- Helpers non Simd --------------------------------------------

u8string readFile(std::string path) {
  namespace fs    = std::filesystem;
  bool validascii = false;
  u8string buffer;
  const auto sz = fs::file_size(path);
  buffer.resize(sz);
  buffer = u8string(sz, '\0');
  std::ifstream file(path, std::ios::in | std::ios::binary);
  file.read((char *)buffer.data(), sz);
  return buffer;
}

struct CodePoint {
  uint8_t len;  // Length of the UTF-8 sequence
  uint32_t value;  // Unicode code point value
};

// Returns the UTF-8 code point in the string starting at the given index,
// along with its length in bytes.
CodePoint codePointAt(const u8string &str, size_t i) {
  if (i >= str.length()) { throw std::out_of_range("Index out of range"); }

  uint8_t len = 0;
  if ((str[i] & 0x80) == 0) {
    len = 1;
  } else if ((str[i] & 0xE0) == 0xC0) {
    len = 2;
  } else if ((str[i] & 0xF0) == 0xE0) {
    len = 3;
  } else if ((str[i] & 0xF8) == 0xF0) {
    len = 4;
  } else {
    throw std::runtime_error("Invalid UTF-8 sequence");
  }

  uint32_t codepoint = 0;
  switch (len) {
  case 1: codepoint = str[i]; break;
  case 2: codepoint = ((str[i] & 0x1F) << 6) | (str[i + 1] & 0x3F); break;
  case 3: codepoint = ((str[i] & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) | (str[i + 2] & 0x3F); break;
  case 4:
    codepoint =
      ((str[i] & 0x07) << 18) | ((str[i + 1] & 0x3F) << 12) | ((str[i + 2] & 0x3F) << 6) | (str[i + 3] & 0x3F);
    break;
  default: throw std::runtime_error("Invalid UTF-8 sequence");
  }

  return { len, codepoint };
}

bool isNewline(char ch) { return ch == '\n' || ch == '\r'; }

std::optional<size_t> matchEscape(const u8string &str) {
  if (str[0] != '\\') { throw std::invalid_argument("Expected escape character"); }

  if (str.length() < 2) return std::nullopt;

  char8_t ch = str[1];
  if (ch < 128) {
    if (isNewline(ch)) return std::nullopt;
    return 2;
  }

  CodePoint cp = codePointAt(str, 1);
  return 1 + cp.len;
}

std::optional<size_t> matchCssStringRest(const u8string &str, char quote) {
  size_t i = 0;
  while (i < str.length()) {
    char8_t byte = str[i];
    if (byte < 128) {
      if (byte == quote) return i + 1;

      if (byte == '\\') {
        auto offset = matchEscape(str.substr(i));
        if (!offset) return std::nullopt;
        i += *offset;
      } else if (isNewline(byte)) {
        return std::nullopt;
      } else {
        i += 1;
      }
    } else {
      i += codePointAt(str, i).len;
    }
  }

  return std::nullopt;
}

std::optional<size_t> matchCssString(const u8string &str) {
  if (str.empty() && !(str[0] != '"' || str[0] != '\'')) {
    throw std::invalid_argument("String must start with a quote");
  }
  char quote = str[0];

  size_t i = 1;
  while (i < str.length()) {
    uint byte = str[i];
    if (byte < 128) {
      if (byte == quote) return i + 1;

      if (byte == '\\') {
        auto offset = matchEscape(str.substr(i));
        if (!offset) return std::nullopt;
        i += *offset;
      } else if (isNewline(byte)) {
        return std::nullopt;
      } else {
        i += 1;
      }
    } else {
      i += codePointAt(str, i).len;
    }
  }

  return std::nullopt;
}


//! ---------------------------- Helpers Simd -------------------------------------------------

using u8simd           = stdx::native_simd<uint8_t>;
using u8simd_str     = std::basic_string<u8simd>;
using u8simd_sv = std::basic_string_view<u8simd>;

const u8simd chunk_128s    = 128;
const u8simd chunk_bsls    = uint8_t('\\');
const u8simd chunk_newline = uint8_t('\n');

struct SimdOffset {
  uint32_t index;
  int offset;
};

u8simd_str readAlignedFile(std::string path) {
  namespace fs    = std::filesystem;
  bool validascii = false;
  u8simd_str buffer;

  const auto sz = fs::file_size(path);
  auto size     = sz / u8simd::size() + sz % u8simd::size() ? 1 : 0;
  buffer.resize(size);
  buffer = u8simd_str(size, char8_t('\0'));
  std::ifstream file(path, std::ios::in | std::ios::binary);
  file.read((char *)buffer.data(), sz);
  return buffer;
}

SimdOffset matchCssStringSimd(const u8simd_str& str, char8_t type) {
  auto first_mask  = str[0] == type;
  auto quote_count = stdx::popcount(first_mask);
  // I think we can afford this for the sake of per u8simd alignment
  auto offset = stdx::find_last_set(first_mask);
  // Incorrect? yes!
  if (quote_count > 1 || str.size() < u8simd::size()) return { 0, offset };
  if (str.empty() || quote_count == 0) { throw std::invalid_argument("String must start with a quote"); }

  u8simd quote = str[0][offset];

  auto x = 0u;
  for (u8simd chunk : str) {
    // Check for escape characters and end quote in one pass
    auto escape_mask = chunk == chunk_bsls;

    // If we found the quote in the chunk
    auto end_quote_mask = chunk == quote;

    if (stdx::any_of(end_quote_mask)) { 
      auto index = stdx::find_first_set(end_quote_mask); 
      return {x, index};
    }
    x++;
  }

  auto remains = matchCssStringRestSimd(str.substr(x), quote);
  return remains;
}

int main() {
  try {
    std::string filename = "harry potter.txt";
    u8string file        = readFile(filename);
    const int n          = 1'000;

    measureTime<u8string>(file, n, matchCssString, "regular");

    std::cout << "Simd Width :" << u8simd::size() << '\n';
    auto simdFile = readAlignedFile(filename);
    // measureTime<u8SimdStringView>(simdFile, n, matchCssStringSimd, "SIMD");
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
