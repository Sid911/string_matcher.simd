#include <cstdint>
#include <experimental/bits/simd.h>
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

u8string readFile(std::string path) {
  namespace fs = std::filesystem;
  u8string buffer;
  const auto sz = fs::file_size(path);
  buffer.resize(sz);
  buffer = u8string(sz, '\0');
  std::basic_ifstream<uint8_t> file(path, std::ios::in | std::ios::binary);
  file.read(buffer.data(), sz);
  return buffer;
}

std::vector<u8string> readFileLines(std::string path) {
  std::vector<u8string> buffer;
  std::basic_ifstream<uint8_t> file(path, std::ios::in | std::ios::binary);
  u8string line;

  while (std::getline(file, line)) { buffer.push_back(line); }
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
  uint32_t index;
  int offset;
};

u8simd_str readAlignedFile(std::string path) {
  std::basic_ifstream<uint8_t> file(path, std::ios::binary);
  auto file_size = std::filesystem::file_size(path);

  std::vector<uint8_t> buffer(file_size);
  file.read(buffer.data(), file_size);
  u8simd_str simd_string;
  u8simd temp;
  for (size_t i = 0; i < file_size; i += u8simd::size()) {
    temp.copy_from(&buffer[i], stdx::element_aligned);
    simd_string.push_back(temp);
  }
  return simd_string;
}


std::vector<u8simd_str> readAlignedFileLines(std::string path) {
  std::basic_ifstream<uint8_t> file(path);
  std::vector<u8simd_str> simd_lines;

  // Determine the maximum line length
  u8string line;
  size_t max_line_length = 0;
  while (std::getline(file, line)) { max_line_length = std::max(max_line_length, line.size()); }
  file.clear();
  file.seekg(0);

  // Align the maximum line length to SIMD vector size
  size_t aligned_line_length = (max_line_length + u8simd::size() - 1) / u8simd::size() * u8simd::size();

  // Read lines into a buffer, padding with zeros
  std::vector<uint8_t> buffer(aligned_line_length);
  while (std::getline(file, line)) {
    std::copy(line.begin(), line.end(), buffer.begin());
    std::fill(buffer.begin() + line.size(), buffer.end(), 0);

    // Load SIMD vectors from the buffer
    u8simd_str simd_line;
    u8simd temp;
    for (size_t i = 0; i < aligned_line_length; i += u8simd::size()) {
      temp.copy_from(&buffer[i], stdx::element_aligned);
      simd_line.push_back(temp);
    }
    simd_lines.push_back(simd_line);
  }

  return simd_lines;
}


struct SimdMatcher {

  alignas(
    stdx::memory_alignment_v<u8mask>) static constexpr std::array<bool, u8mask::size() * 2> mem{ true, true, true };

  static u8mask mask_b1;
  static u8mask mask_b2;
  static u8mask mask_b3;

  SimdMatcher() {
    static u8mask mask_b1{ &mem[2], stdx::element_aligned };
    static u8mask mask_b2{ &mem[1], stdx::element_aligned };
    static u8mask mask_b3{ &mem[0], stdx::vector_aligned };
  }

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

  u8mask maskUtf8ChunkSafe(u8simd &chunk, ChunkOverflow3 &overflow) {
    auto chunk_sr1 = shiftElementRight<1>(chunk);
    auto chunk_sr2 = shiftElementRight<2>(chunk);
    auto chunk_se3 = shiftElementRight<3>(chunk);

    // Take care of overflow
    chunk_sr1 |= overflow.o1;
    chunk_sr2 |= overflow.o2;
    chunk_se3 |= overflow.o3;

    constexpr auto size = sizeof(u8simd::value_type);
    // Identify lead bytes of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    u8mask mask = chunk >= 0xC0;  // Mark lead byte
    mask |= chunk_sr1 >= 0xC0;  // Mark 2nd byte
    mask |= chunk_sr2 >= 0xE0;  // Mark 3rd byte
    mask |= chunk_se3 >= 0xF0;  // Mark 4th byte
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

    ChunkOverflow3 chunk_overflow{};

    auto first_mask = str[offset] == type;
    // set only the bytes which aren't utf8 and escape chars
    first_mask &= !maskUtf8Chunk(str[offset]);
    first_mask &= !maskEscCharChunk(str[offset]);

    auto quote_count = stdx::popcount(first_mask);
    // I think we can afford this for the sake of per u8simd alignment
    auto index = stdx::find_last_set(first_mask);

    auto index_valid_early_ret = quote_count > 1 && index > init_chunk_idx;
    if (index_valid_early_ret || str.size() == 1) return { offset, index };
    if (str.empty() || quote_count == 0) { throw std::invalid_argument("String must start with a quote"); }

    chunk_overflow.set(str[offset]);
    u8simd quote = str[offset][index];
    auto x       = 0u;

    for (u8simd chunk : str | stdrv::drop(offset + 1)) {
      auto utf8_mask = maskUtf8ChunkSafe(chunk, chunk_overflow);
      auto esc_mask  = maskEscCharChunkSafe(chunk, chunk_overflow);
      // If we found the quote in the chunk
      auto end_quote_mask       = chunk == quote;
      auto valid_end_quote_mask = end_quote_mask && !utf8_mask && !esc_mask;

      if (stdx::any_of(end_quote_mask)) {
        auto index = stdx::find_first_set(end_quote_mask);
        return { x, index };
      }
      // Reassign overflow buffer
      chunk_overflow.set(chunk);
      x++;
    }
    return { 0, 0 };
  }
};

int main() {
  try {
    std::string filename = "harry potter.txt";
    u8string file        = readFile(filename);
    // const int n          = 1'000;

    auto start = std::chrono::high_resolution_clock::now();
    auto res1  = matchCssString(file);
    auto end   = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> duration = (end - start);
    std::cout << "Time taken in " << "Normal " << " version: " << duration.count() << " ms" << std::endl;

    auto aligned_file = readAlignedFile(filename);
    SimdMatcher simd_matcher;
    start     = std::chrono::high_resolution_clock::now();
    auto res2 = simd_matcher.matchString(aligned_file, '\"');
    end       = std::chrono::high_resolution_clock::now();

    duration = (end - start);
    std::cout << "Time taken in " << "Normal " << " version: " << duration.count() << " ms" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
