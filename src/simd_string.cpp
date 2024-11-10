#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <uchar.h>
#include <utf8_skip.hpp>

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

u8string readFile(std::string &path) {
  namespace fs = std::filesystem;
  u8string buffer;
  const auto sz = fs::file_size(path);
  std::cout << sz << std::endl;
  buffer.resize(sz, '\0');
  std::basic_ifstream<uint8_t> file(path);
  file.read(buffer.data(), sz);
  return buffer;
}

// std::vector<u8string> readFileLines(std::string &path) {
//   std::vector<u8string> buffer;
//   std::basic_ifstream<uint8_t> file(path, std::ios::binary);

//   if (!file.is_open()) { throw std::runtime_error("Could not open file: " + path); }

//   u8string line;
//   while (std::getline(file, line)) {
//     buffer.emplace_back(reinterpret_cast<const uint8_t *>(line.data()), line.length());
//   }
//   if (file.bad()) { throw std::runtime_error("Error occurred while reading file: " + path); }
//   buffer.shrink_to_fit();
//   return buffer;
// }

std::vector<u8string> readFileLines(const std::string &path) {
  std::vector<u8string> buffer;
  std::ifstream file(path, std::ios::binary);

  if (!file.is_open()) { throw std::runtime_error("Could not open file: " + path); }

  constexpr size_t READ_SIZE = 4096;
  std::vector<char> readBuffer(READ_SIZE);
  u8string currentLine;

  while (file) {
    file.read(readBuffer.data(), READ_SIZE);
    std::streamsize count = file.gcount();

    for (std::streamsize i = 0; i < count; ++i) {
      char ch = readBuffer[i];
      if (ch == '\n') {
        buffer.push_back(currentLine);
        currentLine.clear();
      } else if (ch != '\r') {
        currentLine.push_back(static_cast<uint8_t>(ch));
      }
    }
  }

  if (!currentLine.empty()) { buffer.push_back(currentLine); }

  buffer.shrink_to_fit();
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

const u8simd chunk_bsls    = uint8_t('\\');
const u8simd chunk_newline = uint8_t('\n');

struct SimdOffset {
  int64_t index;
  int offset;
};

std::vector<u8simd_str> readAlignedFileLines(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) { throw std::runtime_error("Failed to open file"); }

  std::vector<u8simd_str> aligned_lines;
  u8string buffer;
  std::string line;

  constexpr size_t simd_size = u8simd::size();

  while (std::getline(file, line)) {
    buffer.assign(line.begin(), line.end());

    // Align the buffer to SIMD width
    size_t buffer_size  = buffer.size();
    size_t aligned_size = ((buffer_size + simd_size - 1) / simd_size) * simd_size;
    buffer.resize(aligned_size, uint8_t{ 0 });  // pad with zeroes

    u8simd_str simd_chunks;
    for (size_t i = 0; i < aligned_size; i += simd_size) {
      u8simd chunk(&buffer[i], stdx::element_aligned);
      simd_chunks.push_back(chunk);
    }
    aligned_lines.push_back(std::move(simd_chunks));
  }

  return aligned_lines;
}

struct SimdMatcher {
  struct ChunkOverflow1 {
    u8simd o1;
    void set(u8simd &chunk) { o1 = shiftElementLeft<u8simd::size() - 1>(chunk); }
  };
  struct ChunkOverflow2 : ChunkOverflow1 {
    u8simd o2;
    void set(u8simd &chunk) {
      o2 = shiftElementLeft<u8simd::size() - 2>(chunk);
      ChunkOverflow1::set(chunk);
    }
  };
  struct ChunkOverflow3 : ChunkOverflow2 {
    u8simd o3;
    void set(u8simd &chunk) {
      o3 = shiftElementLeft<u8simd::size() - 3>(chunk);
      ChunkOverflow2::set(chunk);
    }
  };

  u8mask maskUtf8Chunk(u8simd &chunk) {
    auto chunk_s1 = shiftElementRight<1>(chunk);
    auto chunk_s2 = shiftElementRight<2>(chunk);
    auto chunk_s3 = shiftElementRight<3>(chunk);

    constexpr auto size = sizeof(u8simd::value_type);
    // Identify lead chunks of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    u8mask mask = chunk >= 0xC0;  // Mark lead byte
    mask |= chunk_s1 >= 0xC0;  // Mark 2nd byte
    mask |= chunk_s2 >= 0xE0;  // Mark 3rd byte
    mask |= chunk_s3 >= 0xF0;  // Mark 4th byte
    return mask;
  }

  u8mask maskUtf8AndEscChunk(u8simd &chunk) {
    auto chunk_s1 = shiftElementRight<1>(chunk);
    auto chunk_s2 = shiftElementRight<2>(chunk);
    auto chunk_s3 = shiftElementRight<3>(chunk);

    constexpr auto size = sizeof(u8simd::value_type);
    // Identify lead chunks of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    u8mask mask = chunk >= 0xC0;  // Mark lead byte
    mask |= chunk_s1 >= 0xC0;  // Mark 2nd byte
    mask |= chunk_s1 == chunk_bsls;  // Mask Escape byte letters
    mask |= chunk_s2 >= 0xE0;  // Mark 3rd byte
    mask |= chunk_s3 >= 0xF0;  // Mark 4th byte
    return mask;
  }
  u8mask maskUtf8ChunkSafe(u8simd &chunk, ChunkOverflow3 &overflow) {
    auto chunk_sr1 = shiftElementRight<1>(chunk);
    auto chunk_sr2 = shiftElementRight<2>(chunk);
    auto chunk_sr3 = shiftElementRight<3>(chunk);

    // Take care of overflow
    chunk_sr1 |= overflow.o1;
    chunk_sr2 |= overflow.o2;
    chunk_sr3 |= overflow.o3;

    constexpr auto size = sizeof(u8simd::value_type);
    // Identify lead bytes of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    u8mask mask = chunk >= 0xC0;  // Mark lead byte
    mask |= chunk_sr1 >= 0xC0;  // Mark 2nd byte
    mask |= chunk_sr2 >= 0xE0;  // Mark 3rd byte
    mask |= chunk_sr3 >= 0xF0;  // Mark 4th byte
    return mask;
  }


  u8mask maskUtf8AndEscChunkSafe(u8simd &chunk, ChunkOverflow3 &overflow) {
    auto chunk_sr1 = shiftElementRight<1>(chunk);
    auto chunk_sr2 = shiftElementRight<2>(chunk);
    auto chunk_sr3 = shiftElementRight<3>(chunk);

    // Take care of overflow
    chunk_sr1 |= overflow.o1;
    chunk_sr2 |= overflow.o2;
    chunk_sr3 |= overflow.o3;

    constexpr auto size = sizeof(u8simd::value_type);
    // Identify lead bytes of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    u8mask mask = chunk >= 0xC0;  // Mark lead byte
    mask |= chunk_sr1 >= 0xC0;  // Mark 2nd byte
    mask |= chunk_sr1 == chunk_bsls;  // Check for escape adn mask it
    mask |= chunk_sr2 >= 0xE0;  // Mark 3rd byte
    mask |= chunk_sr3 >= 0xF0;  // Mark 4th byte
    return mask;
  }
  u8mask maskEscCharChunk(u8simd &chunk) {
    const auto chunk_sr1 = shiftElementRight<1>(chunk);
    const u8simd cmp     = chunk_bsls;

    // u8mask mask = chunk == cmp;
    u8mask mask = chunk_sr1 == cmp;
    return mask;
  }

  u8mask maskEscCharChunkSafe(u8simd &chunk, ChunkOverflow1 &overflow) {
    auto chunk_sr1   = shiftElementRight<1>(chunk);
    const u8simd cmp = chunk_bsls;

    chunk_sr1 |= overflow.o1;

    // I mean, we can skip this :P
    // u8mask mask = chunk == cmp;
    u8mask mask = chunk_sr1 == cmp;
    return mask;
  }


  SimdOffset matchString(u8simd_str &str, char8_t type, uint32_t offset = 0, uint32_t init_chunk_idx = 0) {
    namespace stdr  = std::ranges;
    namespace stdrv = stdr::views;
    if (str.size() <= offset) return { offset, -1 };
    ChunkOverflow3 chunk_overflow{};

    auto first_mask = str[offset] == type;
    // set only the bytes which aren't utf8 and escape chars
    first_mask &= !maskUtf8AndEscChunk(str[offset]);

    auto quote_count = stdx::popcount(first_mask);
    if (quote_count == 0) return { offset, -1 };
    // I think we can afford this for the sake of per u8simd alignment
    int index = stdx::find_last_set(first_mask);

    auto index_valid_early_ret = quote_count > 1 && index > init_chunk_idx;
    if (index_valid_early_ret || str.size() == 1) return { offset, index };

    chunk_overflow.set(str[offset]);
    u8simd quote = str[offset][index];
    auto x       = 0u;

    for (u8simd chunk : str | stdrv::drop(offset + 1)) {
      auto utf8_mask = maskUtf8AndEscChunkSafe(chunk, chunk_overflow);
      // auto esc_mask  = maskEscCharChunkSafe(chunk, chunk_overflow);
      // If we found the quote in the chunk
      auto end_quote_mask       = chunk == quote;
      auto valid_end_quote_mask = end_quote_mask && !utf8_mask;

      if (stdx::any_of(end_quote_mask)) {
        auto index = stdx::find_first_set(end_quote_mask);
        return { x, index };
      }
      // Reassign overflow buffer
      chunk_overflow.set(chunk);
      x++;
    }
    return { offset, -1 };
  }
};

int main() {
  std::string filename = "random_words.txt";
  auto lines           = readFileLines(filename);
  const int n = 1'000;

  auto start1 = std::chrono::high_resolution_clock::now();
  for (auto &line : lines) { matchCssString(line); }
  auto end1 = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> duration1 = (end1 - start1);
  std::cout << "Time taken in " << "Linear" << " version: " << duration1.count() << " ms" << std::endl;

  auto aligned_lines = readAlignedFileLines(filename);
  SimdMatcher simd_matcher;
  auto start = std::chrono::high_resolution_clock::now();
  // std::vector<SimdOffset> vec;
  // vec.reserve(aligned_lines.size());
  for (auto x = 0; x < n; x++)
    for (auto &line : aligned_lines) {
      simd_matcher.matchString(line, '\"');
      // vec.push_back(res);
    }
  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> duration = (end - start) / n;
  // std::cout << "\nOutputs : " << '\n';
  // for (auto &offset : vec | std::ranges::views::take(50)) { std::cout << offset.index << ":" << offset.offset << " "; }
  std::cout << "Time taken in " << "Simd" << " version: " << duration.count() << " ms" << std::endl;
  return 0;
}
