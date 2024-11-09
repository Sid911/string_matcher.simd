#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <experimental/bits/simd.h>
#include <experimental/simd>
#include <uchar.h>
#include <vector>

namespace stdx {
using namespace std::experimental;
using namespace std::experimental::__proposed;
}  // namespace stdx

using u8simd  = stdx::native_simd<uint8_t>;
using u8mask  = stdx::native_simd_mask<uint8_t>;
using u32simd = stdx::resize_simd_t<u8simd::size() / 4, u8simd>;

using u8string      = std::basic_string<uint8_t>;
using u8string_view = std::basic_string_view<uint8_t>;

using u8simd_str = std::basic_string<u8simd>;
using u8simd_sv  = std::basic_string_view<u8simd>;

constexpr size_t accomodateBytes(size_t size) { return (size / u8simd::size()) + ((size % u8simd::size()) ? 1 : 0); }

template<typename T>
concept SimdType = requires {
  stdx::is_simd_v<T> || stdx::is_simd_mask_v<T>;
  { T::size() } -> std::same_as<std::size_t>;
};

template<uint16_t shift>
[[nodiscard]]
auto shiftElementLeft(SimdType auto reg) {
  using InputType = decltype(reg);
  using ValueType = InputType::value_type;

  static constexpr auto half_size  = InputType::size() / 2;
  static constexpr auto simd_width = InputType::size() * sizeof(ValueType) * 8;
  static constexpr bool cross      = shift > half_size;

  if constexpr (shift == 0) return &reg;
  if constexpr (simd_width == 256) {
    const __m256i &src = (__m256i &)(reg);

    // If this is a cross lane shift
    if constexpr (cross) {
      // Swap and erase MSB only leaving specified size
      __m256i permuted = _mm256_permute2x128_si256(src, src, 0xF1);
      // Shift the permuted register
      __m256i shifted = _mm256_srli_si256(permuted, shift - half_size);
      return (InputType)shifted;
    }
    // If this isn't cross lane
    else {
      __m256i shifted = _mm256_srli_si256(src, shift);
      // Swap and erase LSB leaving specified
      __m256i swap_erase = _mm256_permute2x128_si256(src, src, 0x81);
      swap_erase         = _mm256_slli_si256(swap_erase, half_size - shift);
      // Handle lane discards
      __m256i dst = _mm256_or_si256(shifted, swap_erase);
      return (InputType)dst;
    }
  } else
    static_assert(false, "Simd of this type is not supported");
}

// Discards last values and shift right only supports x86 for now and does not support rebind api
template<uint16_t shift>
[[nodiscard]]
auto shiftElementRight(SimdType auto reg) {
  using InputType = decltype(reg);
  using ValueType = InputType::value_type;

  static constexpr auto simd_width = InputType::size() * sizeof(ValueType) * 8;
  static constexpr auto half_size  = InputType::size() / 2;

  if constexpr (shift == 0) return &reg;
  if constexpr (shift > half_size) static_assert(false, "Cross Lane is not supported");

  if constexpr (simd_width == 256) {
    const __m256i &src = (__m256i &)(reg);
    // shift each 128 lanes to left while bringing in zeros
    // maybe use _mm256_permutevar8x32_epi32 ? Seems more latency for shaking off overflow
    __m256i shifted       = _mm256_slli_si256(src, shift);
    __m256i lane_overflow = _mm256_permute2x128_si256(src, src, 0x08);  // Bring lower bytes to upper
    lane_overflow         = _mm256_srli_si256(lane_overflow, half_size - shift);  // trim
    __m256i dst           = _mm256_or_si256(shifted, lane_overflow);
    return (InputType)dst;
  }

  else if constexpr (simd_width == 128) {
    const __m128i &src = (__m128i &)reg;
    __m128i shifted    = _mm_slli_si128(src, shift);
    // Todo: make sure this works
    __m128i overflow = _mm_shuffle_epi32(src, 0xB1);
    overflow         = _mm_slli_si128(overflow, half_size - shift);
    auto dst         = _mm_or_si128(shifted, overflow);
    return (InputType)dst;
  } else
    static_assert(false, "Simd of this type is not supported");
}

[[nodiscard]]
auto orFirst32Bits(SimdType auto reg, uint32_t value) {
  using InputType = decltype(reg);
  using ValueType = InputType::value_type;

  static constexpr auto simd_width = InputType::size() * sizeof(ValueType) * 8;
  static constexpr auto half_size  = InputType::size() / 2;

  if constexpr (simd_width == 256) {
    const __m256i &src = (__m256i &)reg;
    __m128i lower      = _mm256_castsi256_si128(src);
    lower              = _mm_or_si128(lower, _mm_set_epi32(value, 0, 0, 0));
    return _mm256_insertf128_si256(src, lower, 0);
  } else if constexpr (simd_width == 128) {
    const __m128i &src = (__m128i &)reg;
    return _mm_or_si128(src, _mm_set_epi32(value, 0, 0, 0));
  } else {
    static_assert(false, "Simd of this type is not supported");
  }
}

std::vector<u8mask> mark_utf8_bytes2(u8simd_str &data, uint32_t offset = 0);
std::vector<u8mask> mark_utf8_bytes(const u8simd_str &data);

u8simd_str stringToSimd(const std::string &string);
u8simd_str readAlignedFile(std::string path);

void printMask(std::vector<u8mask> &masks, size_t max = 20);
void printMaskMap(std::vector<u8mask> &masks, std::string &utf8_str, size_t max = 500);
