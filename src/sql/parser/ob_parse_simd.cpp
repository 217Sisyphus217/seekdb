/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "share/ob_define.h"
#if defined(__GNUC__) && defined(__x86_64__) 
#include "data_plane/vector/ob_vector_op_common.h"
#endif

namespace oceanbase
{
namespace common
{

#if defined(__GNUC__) && defined(__x86_64__) 
OB_DECLARE_AVX512_SPECIFIC_CODE (
inline static void process_binary_avx512(const char **src, const char *end, char **dest)
{
  __m512i zero = _mm512_set1_epi8('0');
  __m512i nine = _mm512_set1_epi8('9');
  __m512i extra = _mm512_set1_epi8(9);
  __m512i low_mask = _mm512_set1_epi16(0x00FF);
  __m512i high_mask = _mm512_set1_epi16(0xFF00);
  __m512i low_nibble_mask = _mm512_set1_epi8(0x0F);
  
  for (; *src + 64 <= end; *src += 64, *dest += 32) {
    __m512i input = _mm512_loadu_si512((__m512i*)*src);
    __m512i rawbytes = _mm512_add_epi64(_mm512_and_si512(input, low_nibble_mask), 
                                        _mm512_maskz_loadu_epi8(~(_mm512_cmpge_epu8_mask(input, zero) & _mm512_cmple_epu8_mask(input, nine)), &extra));
    __m512i low_bytes = _mm512_slli_epi16(_mm512_and_si512(rawbytes, low_mask), 4);
    __m512i high_bytes = _mm512_srli_epi16(_mm512_and_si512(rawbytes, high_mask), 8);
    __m256i tmp = _mm512_cvtepi16_epi8(_mm512_or_si512(low_bytes, high_bytes));
    _mm256_storeu_si256((__m256i*)*dest, tmp);
  }
}
)

OB_DECLARE_AVX2_SPECIFIC_CODE (
inline static __m256i char_nibble(const __m256i value) {
  __m256i _9 = _mm256_set1_epi16(9);
  __m256i _15 = _mm256_set1_epi16(0xf);
  __m256i and15 = _mm256_and_si256(value, _15);
  __m256i sr6 = _mm256_srai_epi16(value, 6);
  __m256i mul = _mm256_maddubs_epi16(sr6, _9);
  __m256i add = _mm256_add_epi16(mul, and15);
  return add;
}

inline static void process_binary_avx2(const char **src, const char *end, char **dest)
{
  const char *src_ = *src;
  char *dest_ = *dest;
  int len = (end - src_) / 2;
  static const __m256i A_MASK = _mm256_setr_epi8(
    0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14, -1,
    0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14, -1);
  static const __m256i B_MASK = _mm256_setr_epi8(
    1, -1, 3, -1, 5, -1, 7, -1, 9, -1, 11, -1, 13, -1, 15, -1,
    1, -1, 3, -1, 5, -1, 7, -1, 9, -1, 11, -1, 13, -1, 15, -1);
  const __m256i* src_256 = reinterpret_cast<const __m256i*>(src_);
  __m256i* dest_256 = reinterpret_cast<__m256i*>(dest_);
  while (len >= 32) {
    __m256i av1 = _mm256_lddqu_si256(src_256++); // 32 nibbles, 16 bytes
    __m256i av2 = _mm256_lddqu_si256(src_256++);
    
    // Separate high and low nibbles and extend into 16-bit elements
    __m256i a1 = _mm256_shuffle_epi8(av1, A_MASK);
    __m256i b1 = _mm256_shuffle_epi8(av1, B_MASK);
    __m256i a2 = _mm256_shuffle_epi8(av2, A_MASK);
    __m256i b2 = _mm256_shuffle_epi8(av2, B_MASK);

    // Convert ASCII values to nibbles
    a1 = char_nibble(a1);
    a2 = char_nibble(a2);
    b1 = char_nibble(b1);
    b2 = char_nibble(b2);

    // Nibbles to bytes
    __m256i a4_1 = _mm256_slli_epi16(a1, 4);
    __m256i a4_2 = _mm256_slli_epi16(a2, 4);
    __m256i a4orb_1 = _mm256_or_si256(a4_1, b1);
    __m256i a4orb_2 = _mm256_or_si256(a4_2, b2);
    __m256i pck1 = _mm256_packus_epi16(a4orb_1, a4orb_2); // lo1 lo2 hi1 hi2
    const int _0213 = 0b11011000;
    __m256i bytes = _mm256_permute4x64_epi64(pck1, _0213);

    _mm256_storeu_si256(dest_256++, bytes);
    len -= 32;
  }
  *src = reinterpret_cast<const char*>(src_256);
  *dest = reinterpret_cast<char *>(dest_256);
}
)

// called in parse_node.c
extern "C" void ob_parse_binary_simd(const char **src, const char *end, char **dest)
{
  #if OB_USE_MULTITARGET_CODE
  if (common::is_arch_supported(ObTargetArch::AVX512)) {
    common::specific::avx512::process_binary_avx512(src, end, dest);
  } else if (common::is_arch_supported(ObTargetArch::AVX2)) {
    common::specific::avx2::process_binary_avx2(src, end, dest);
  }
  #endif
}


#endif
} // namespace common
} // namespace oceanbase
