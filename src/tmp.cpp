#include <filesystem>
#include <fstream>
#include <iostream>

#include "utf8_skip.hpp"
// Data to generate masks from
u8simd_str stringToSimd(const std::string &string) {
  u8simd_str res(accomodateBytes(string.size()), 0);
  for (uint i = 0; i < res.size(); i++) {
    for (uint j = 0; j < u8simd::size(); j++) { res[i][j] = uint8_t(string[i * u8simd::size() + j]); }
  }
  return res;
}

u8simd_str readAlignedFile(std::string path) {
  namespace fs    = std::filesystem;
  bool validascii = false;
  u8simd_str buffer;

  const auto sz = fs::file_size(path);
  auto size     = accomodateBytes(sz);
  buffer        = u8simd_str(size, char8_t('\0'));
  std::ifstream file(path, std::ios::in | std::ios::binary);
  file.read((char *)buffer.data(), sz);
  return buffer;
}

void printMask(std::vector<u8mask> &masks, size_t max) {
  std::cout << "\nOutput Mask : " << '\n';
  auto max_itr = std::min(max, masks.size());
  for (uint i = 0; i < max_itr; i++) {
    for (uint idx = 0; idx < u8mask::size(); idx++) std::cout << masks[i][idx];
    std::cout << " ";
  }
}

void printMaskMap(std::vector<u8mask> &masks, std::string &utf8_str, size_t max) {
  std::cout << "\n" << "Input to out map : \n";

  uint s        = 0;
  bool run      = false;
  auto max_iter = std::min(max, utf8_str.size());
  for (uint i = 0; i < max_iter; i++) {
    uint x = i / u8mask::size(), y = i % u8mask::size();
    if (masks[x][y] != run || i == utf8_str.size() - 1) {
      auto sv = std::string_view(utf8_str);
      std::cout << sv.substr(s, i - s) << " " << s << "::" << i << " " << run << "\n";
      run = !run;
      s   = i;
    }
  }
}

std::string readFile(std::string path) {
  namespace fs    = std::filesystem;
  bool validascii = false;
  std::string buffer;
  const auto sz = fs::file_size(path);
  buffer        = std::string(sz, '\0');
  std::ifstream file(path, std::ios::in | std::ios::binary);
  file.read((char *)buffer.data(), sz);
  return buffer;
}

void file_test() {
  std::string filename = "big.txt";

  u8simd_str file = readAlignedFile(filename);
  const int n     = 1000;
  auto start      = std::chrono::high_resolution_clock::now();
  for (auto x = 0; x < n; x++) { mark_utf8_bytes2(file); }
  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> duration = (end - start);
  std::cout << "Time taken in : " << duration.count() / n << " ms" << std::endl;
  // printMask(masks, 1);
  std::string content = readFile(filename);
  // printMaskMap(masks, content);
}

int main() {
  std::string utf8_str =
    "Hello, (update 2) 世界! meow 🐱. \n This 'thing' has overflow 🐮 issues";  // Example UTF-8 string
  u8simd_str data = stringToSimd(utf8_str);

  constexpr auto simd_width = u8mask::size() * sizeof(u8mask::value_type) * 8;

  std::cout << "Input : " << utf8_str << " Size : " << utf8_str.size() << "\n";
  std::cout << "Input Simd chunks : " << data.size() << ":" << u8simd_str::value_type::size() << "\n";

  std::cout << "Total Simd Width: " << simd_width << "\n";
  std::cout << "u8simd size: " << u8simd::size() << "\n";
  std::cout << "u8mask size: " << u8mask::size() << "\n\n";

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<u8mask> masks = mark_utf8_bytes2(data);

  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> duration = (end - start);
  std::cout << "Time taken in : " << duration.count() << " ms" << std::endl;

  printMaskMap(masks, utf8_str);

  file_test();

  return 0;
}
