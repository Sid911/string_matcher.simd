#include "utf8_skip.hpp"

alignas(stdx::memory_alignment_v<u8mask>) static constexpr std::array<bool, u8mask::size() * 2> mem{ true, true, true };

std::vector<u8mask> mark_utf8_bytes2(u8simd_str &data, uint32_t offset) {
  static u8mask mask_b1{ &mem[2], stdx::element_aligned };
  static u8mask mask_b2{ &mem[1], stdx::element_aligned };
  static u8mask mask_b3{ &mem[0], stdx::vector_aligned };

  std::vector<u8mask> masks(data.size() - offset);
  // This is very expensive on memory there is no need to maintain entire registers
  u8simd overflow1{};
  u8simd overflow2{};
  u8simd overflow3{};
  for (size_t i = offset; i < data.size(); i++) {
    u8simd bytes = data[i];

    // Save the last bits of info which will overflow

    auto byte_s1 = shiftElementRight<1>(bytes);
    auto byte_s2 = shiftElementRight<2>(bytes);
    auto byte_s3 = shiftElementRight<3>(bytes);

    // pretty damn skeptical of what this produces
    // stdx::where(mask_b1, byte_s1) = overflow1;
    // stdx::where(mask_b2, byte_s2) = overflow2;
    // stdx::where(mask_b3, byte_s3) = overflow3;

    byte_s1 |= overflow1;
    byte_s2 |= overflow2;
    byte_s3 |= overflow3;


    constexpr auto size = sizeof(u8simd::value_type);
    u8mask mask         = masks[i];
    // Identify lead bytes of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    mask |= bytes >= 0xC0;  // Mark lead byte
    mask |= byte_s1 >= 0xC0;  // Mark 2nd byte
    mask |= byte_s2 >= 0xE0;  // Mark 3rd byte
    mask |= byte_s3 >= 0xF0;  // Mark 4th byte
    masks[i] = mask;

    // Also I should be able to remove this shift just by reversing the overflow
    overflow3 = shiftElementLeftCrossLane<u8simd::size() - 3>(bytes);
    overflow2 = shiftElementLeftCrossLane<u8simd::size() - 2>(bytes);
    overflow1 = shiftElementLeftCrossLane<u8simd::size() - 1>(bytes);
  }
  return masks;
}


std::vector<u8mask> mark_utf8_bytes(const u8simd_str &data) {
  std::vector<u8mask> masks(data.size() + 1);  // 1 more because of overflow
  std::size_t i = 0;

  while (i < data.size()) {
    u8simd bytes = data[i];
    std::array<u8mask, 2> overflow{ masks[i], {} };  // assuming simd is more than 4 bytes :)
    // Identify lead bytes of UTF-8 sequences (0xxxxxxx or 11xxxxxx)
    auto lead_byte_mask = (bytes >= 0xC0);

    for (std::size_t j = 0; j < u8simd::size(); ++j) {
      uint8_t byte = bytes[j];
      if (lead_byte_mask[j]) {
        if (byte >= 0xC0 && byte < 0xE0) {
          // Mark 2-byte sequence
          overflow[0][j]     = true;
          overflow[0][j + 1] = true;  // might overflow?
        } else if (byte >= 0xE0 && byte < 0xF0) {
          // Mark 3-byte sequence
          overflow[0][j]     = true;
          overflow[0][j + 1] = true;  // might overflow?
          overflow[0][j + 2] = true;  // might overflow?
        } else if (byte >= 0xF0) {
          // Mark 4-byte sequence
          overflow[0][j]     = true;
          overflow[0][j + 1] = true;  // might overflow?
          overflow[0][j + 2] = true;  // might overflow?
          overflow[0][j + 3] = true;  // might overflow?
        }
      }
    }

    masks[i] = overflow[0];

    i += 1;
  }

  return std::move(masks);
}


