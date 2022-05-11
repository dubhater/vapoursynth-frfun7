#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <emmintrin.h>

#include <VapourSynth.h>
#include <VSHelper.h>




// FIXME: This plugin does not contain C reference algorithms, requires Intel SSE2
// since it was reconstructed from having only an inline mmx assembler.

#ifdef _WIN32
#define AVS_FORCEINLINE __forceinline
#else
#define AVS_FORCEINLINE inline __attribute__((always_inline))
#endif

AVS_FORCEINLINE __m128i _mm_load_si32(const uint8_t* ptr) {
  return _mm_castps_si128(_mm_load_ss((const float*)(ptr)));
}

// SAD of 2x(2x4) reference and 4x4 actual bytes
// return value is in sad
AVS_FORCEINLINE void simd_sad16(__m128i ref01, __m128i ref23, int offset, const uint8_t* rdst, int rdp_aka_pitch, int &sad)
{
  auto src0 = _mm_load_si32(rdst + offset);
  auto src1 = _mm_load_si32(rdst + rdp_aka_pitch + offset);
  auto src01 = _mm_or_si128(_mm_slli_epi64(src0, 32), src1); // make 8 side-by-side byte from 2x4
  auto sad01 = _mm_sad_epu8(src01, ref01); // sad against 0th and 1st line reference 4 pixels

  auto src2 = _mm_load_si32(rdst + rdp_aka_pitch * 2 + offset);
  auto src3 = _mm_load_si32(rdst + rdp_aka_pitch * 3 + offset);
  auto src23 = _mm_or_si128(_mm_slli_epi64(src2, 32), src3); // make 8 side-by-side byte from 2x4
  auto sad23 = _mm_sad_epu8(src23, ref23); // sad against 2nd and 3rd line reference 4 pixels

  sad = _mm_cvtsi128_si32(_mm_add_epi64(sad01, sad23)); // lower 64 is enough
}

// increments acc if rsad < threshold.
// sad: sad input from prev. function sad16 (=sad4x4)
// sad_acc: sad accumulator
AVS_FORCEINLINE void simd_comp(int &sad, int& sad_acc, int threshold)
{
  const int one_if_inc = sad < threshold ? 1 : 0;
  sad = one_if_inc ? 0xFFFFFFFF : 0; // output mask, used in acc4/acc16
  sad_acc += one_if_inc;
  // output sad will get 0xFFFFFFFF or 0
  // original code:
  // 1 shr 31 sra 31 = 0xffffffff
  // 0 shr 31 sra 31 = 0x00000000
  // Need for next step for acc4/acc16
}

AVS_FORCEINLINE void simd_acc4(__m128i &mmA, __m128i mm3, int &mask_by_sadcomp)
{
  // mmA: 4 input words
  // mm3: 4 bytes
  // mask_by_sadcomp: all 0 or all FF
  __m128i mm2 = _mm_cvtsi32_si128(mask_by_sadcomp); // 0x00000000 of 0xFFFFFFFF from 'comp'
  mm3 = _mm_and_si128(mm3, mm2); // mask
  auto zero = _mm_setzero_si128();
  mmA = _mm_add_epi16(mmA, _mm_unpacklo_epi8(mm3, zero)); // 8 bytes 4 words
}

// fills mm4..mm7 with 4 words
AVS_FORCEINLINE void simd_acc16(int offset, const uint8_t *rdst, int rdp_aka_pitch, int& mask_by_sadcomp, __m128i &mm4, __m128i& mm5, __m128i& mm6, __m128i& mm7)
{
  simd_acc4(mm4, _mm_load_si32(rdst + offset), mask_by_sadcomp);
  simd_acc4(mm5, _mm_load_si32(rdst + 1 * rdp_aka_pitch + offset), mask_by_sadcomp);
  simd_acc4(mm6, _mm_load_si32(rdst + 2 * rdp_aka_pitch + offset), mask_by_sadcomp);
  simd_acc4(mm7, _mm_load_si32(rdst + 3 * rdp_aka_pitch + offset), mask_by_sadcomp);
}

AVS_FORCEINLINE void simd_check(__m128i ref01, __m128i ref23, int offset, const uint8_t* rdst, int rdp_aka_pitch, int& racc, int threshold,
  __m128i& mm4, __m128i& mm5, __m128i& mm6, __m128i& mm7)
{
  int sad;
  simd_sad16(ref01, ref23, offset, rdst, rdp_aka_pitch, sad);
  simd_comp(sad, racc, threshold);
  simd_acc16(offset, rdst, rdp_aka_pitch, sad, mm4, mm5, mm6, mm7);
}

AVS_FORCEINLINE void simd_acheck(__m128i ref01, __m128i ref23, int offset, const uint8_t* rdst, int rdp_aka_pitch, int& racc, int threshold,
  __m128i& mm4, __m128i& mm5, __m128i& mm6, __m128i& mm7,
  int &edx_sad_summing)
{
  int sad;
  simd_sad16(ref01, ref23, offset, rdst, rdp_aka_pitch, sad);
  edx_sad_summing += sad;
  simd_comp(sad, racc, threshold);
  simd_acc16(offset, rdst, rdp_aka_pitch, sad, mm4, mm5, mm6, mm7);
}

AVS_FORCEINLINE void simd_stor4(uint8_t* esi, __m128i& mmA, __m128i mm0_multiplier, __m128i mm3_rounder, __m128i mm2_zero)
{
  // ((((mm1 << 2) * multiplier) >> 16 ) + 1) >> 1
  auto mm1 = mmA;
  mm1 = _mm_slli_epi16(mm1, 2);
  mm1 = _mm_mulhi_epu16(mm1, mm0_multiplier); // pmulhuw, really unsigned
  mm1 = _mm_adds_epu16(mm1, mm3_rounder);
  mm1 = _mm_srli_epi16(mm1, 1);
  mm1 = _mm_packus_epi16(mm1, mm2_zero); // 4 words to 4 bytes
  *(uint32_t*)(esi) = _mm_cvtsi128_si32(mm1);
}

template<int R> // radius; 3 or 0 is used
AVS_FORCEINLINE void frcore_filter_b4r0or2or3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  // convert to upper left corner of the radius
  ptra += -R * pitcha - R; // cpln(-3, -3) or cpln(0, 0)

  int weight_acc = 0;

  // reference pixels
  auto m0 = _mm_load_si32(ptrr); // 4 bytes
  auto m1 = _mm_load_si32(ptrr + pitchr * 1);
  auto m2 = _mm_load_si32(ptrr + pitchr * 2);
  auto m3 = _mm_load_si32(ptrr + pitchr * 3);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  // accumulators
  // each collects 4 words (weighted sums)
  // which will be finally scaled back and stored as 4 bytes
  auto mm4 = _mm_setzero_si128();
  auto mm5 = _mm_setzero_si128();
  auto mm6 = _mm_setzero_si128();
  auto mm7 = _mm_setzero_si128();

  if constexpr (R >= 2)
  {
    if constexpr (R >= 3)
    {
      // -3 // top line of y= -3..+3
      simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
      simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
      simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
      simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
      ptra += pitcha; // next line
    }

    // -2
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    if constexpr (R >= 3)
    {
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    }
    ptra += pitcha; // next line

    // -1
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    if constexpr (R >= 3)
    {
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    }
    ptra += pitcha; // next line
  }

  //; 0
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 2)
  {
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    if constexpr (R >= 3)
    {
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    }
  }

  if constexpr (R >= 2)
  {

    ptra += pitcha;

    // +1
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    if constexpr (R >= 3)
    {
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    }

    ptra += pitcha;
    // +2
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
    if constexpr (R >= 3)
    {
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    }

    if constexpr (R >= 3)
    {
      ptra += pitcha;
      simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
      simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
      simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 0
      simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 1
      simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 2
      simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7); // base - 3
    }

  }

  // mm4 - mm7 has accumulated sum, weight is ready here

  *weight = weight_acc;

  auto zero = _mm_setzero_si128(); // packer zero
  auto rounder_one = _mm_set1_epi16(1);

  // scale 4 - 7 by weight
  auto weight_recip = _mm_set1_epi16(inv_table[weight_acc]);

  simd_stor4(ptrb + 0 * pitchb, mm4, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 1 * pitchb, mm5, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 2 * pitchb, mm6, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 3 * pitchb, mm7, weight_recip, rounder_one, zero);
}

AVS_FORCEINLINE void frcore_filter_b4r3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  frcore_filter_b4r0or2or3_simd<3>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, inv_table, weight);
}

AVS_FORCEINLINE void frcore_filter_b4r2_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  frcore_filter_b4r0or2or3_simd<2>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, inv_table, weight);
}

AVS_FORCEINLINE void frcore_filter_b4r0_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  frcore_filter_b4r0or2or3_simd<0>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, inv_table, weight);
}

// R == 2 or 3 (initially was: only 3)
template<int R>
AVS_FORCEINLINE void frcore_filter_adapt_b4r2or3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, int sThresh2, int sThresh3, const int* inv_table, int* weight)
{
  // convert to upper left corner of the radius
  ptra += -1 * pitcha - R; // cpln(-3, -1)

  int weight_acc = 0; // xor ecx, ecx

  // reference pixels
  auto m0 = _mm_load_si32(ptrr); // 4 bytes
  auto m1 = _mm_load_si32(ptrr + pitchr * 1);
  auto m2 = _mm_load_si32(ptrr + pitchr * 2);
  auto m3 = _mm_load_si32(ptrr + pitchr * 3);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  // accumulators
  // each collects 4 words (weighted sums)
  // which will be finally scaled back and stored as 4 bytes
  auto mm4 = _mm_setzero_si128();
  auto mm5 = _mm_setzero_si128();
  auto mm6 = _mm_setzero_si128();
  auto mm7 = _mm_setzero_si128();

  int edx_sad_summing = 0;

  // ; -1
  simd_acheck(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  ptra += pitcha; // next line

  // ; 0
  simd_acheck(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  ptra += pitcha; // next line

  // ; +1
  simd_acheck(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
  simd_acheck(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);

  if (edx_sad_summing >= sThresh2)
  {
    // Expand the search for distances not covered in the first pass
    ptra -= 3 * pitcha; // move to -2

    // ; -2
    simd_acheck(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    ptra += pitcha; // next line

    // ; -1
    simd_acheck(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    ptra += pitcha; // next line

    // ; 0
    simd_acheck(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    ptra += pitcha; // next line

    // ; +1
    simd_acheck(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    ptra += pitcha; // next line

    // ; +2
    simd_acheck(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);
    simd_acheck(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7, edx_sad_summing);

    if constexpr (R >= 3) {
      if (edx_sad_summing >= sThresh3)
      {
        // Expand the search for distances not covered in the first-second pass
        ptra -= 5 * pitcha; // move to -3

        // no more need for acheck.edx_sad_summing

        // ; -3
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; -2
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; -1
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; 0
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; +1
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; +2
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        ptra += pitcha; // next line

        // ; +3
        simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
        simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
      }
    }
  }

  // mm4 - mm7 has accumulated sum, weight is ready here

  *weight = weight_acc;

  auto zero = _mm_setzero_si128(); // packer zero
  auto rounder_one = _mm_set1_epi16(1);

  // scale 4 - 7 by weight
  auto weight_recip = _mm_set1_epi16(inv_table[weight_acc]);

  simd_stor4(ptrb + 0 * pitchb, mm4, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 1 * pitchb, mm5, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 2 * pitchb, mm6, weight_recip, rounder_one, zero);
  simd_stor4(ptrb + 3 * pitchb, mm7, weight_recip, rounder_one, zero);
}

AVS_FORCEINLINE void frcore_filter_adapt_b4r3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, int sThresh2, int sThresh3, const int* inv_table, int* weight)
{
  frcore_filter_adapt_b4r2or3_simd<3>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, sThresh2, sThresh3, inv_table, weight);
}

AVS_FORCEINLINE void frcore_filter_adapt_b4r2_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, int sThresh2, int sThresh3, const int* inv_table, int* weight)
{
  frcore_filter_adapt_b4r2or3_simd<2>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, sThresh2, sThresh3, inv_table, weight);
}

AVS_FORCEINLINE void simd_blend_store4(uint8_t* esi, __m128i mmA, __m128i mm2_multiplier, __m128i mm1_rounder, __m128i mm0_zero)
{
  auto mm3 = _mm_unpacklo_epi8(_mm_load_si32(esi), mm0_zero);
  // tmp= ((esi << 6) * multiplier) >> 16  ( == [esi]/1024 * multiplier)
  // mmA = (mmA + tmp + rounder_16) / 32
  // ((((mm1 << 2) * multiplier) >> 16 ) + 1) >> 1
  mm3 = _mm_slli_epi16(mm3, 6);
  mm3 = _mm_mulhi_epi16(mm3, mm2_multiplier); // pmulhw, signed
  mmA = _mm_adds_epu16(mmA, mm3);
  mmA = _mm_adds_epu16(mmA, mm1_rounder);
  mmA = _mm_srli_epi16(mmA, 5);
  mmA = _mm_packus_epi16(mmA, mm0_zero); // 4 words to 4 bytes
  *(uint32_t*)(esi) = _mm_cvtsi128_si32(mmA);
}

// used in mode_temporal
// R is 2 or 3
template<int R>
AVS_FORCEINLINE void frcore_filter_overlap_b4r2or3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  ptra += -R * pitcha - R; // cpln(-3, -3) or cpln(-2, -2)

  int weight_acc = 0;

  // reference pixels
  auto m0 = _mm_load_si32(ptrr); // 4 bytes
  auto m1 = _mm_load_si32(ptrr + pitchr * 1);
  auto m2 = _mm_load_si32(ptrr + pitchr * 2);
  auto m3 = _mm_load_si32(ptrr + pitchr * 3);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  // accumulators
  // each collects 4 words (weighted sums)
  // which will be finally scaled back and stored as 4 bytes
  auto mm4 = _mm_setzero_si128();
  auto mm5 = _mm_setzero_si128();
  auto mm6 = _mm_setzero_si128();
  auto mm7 = _mm_setzero_si128();

  if constexpr (R >= 3)
  {
    // -3 // top line of y= -3..+3
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    ptra += pitcha; // next line
  }
  // -2
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 3)
  {
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }
  ptra += pitcha; // next line

  // -1
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 3)
  {
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }
  ptra += pitcha; // next line

  //; 0
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 3)
  {
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }
  ptra += pitcha;

  // +1
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 3)
  {
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }

  ptra += pitcha;
  // +2
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  if constexpr (R >= 3)
  {
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }

  if constexpr (R >= 3)
  {
    ptra += pitcha;
    simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 3, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 4, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 5, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
    simd_check(ref01, ref23, 6, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  }

  // mm4 - mm7 has accumulated sum, weight is ready here

  // weight variable is a multi-purpose one, here we get a 32 bit value,
  // which is really two 16 bit words
  // lower 16 and upper 16 bit has separate meaning
  int prev_weight = *weight;

  // write back real weight, probably a later process need it
  *weight = weight_acc;

  // scale 4 - 7 by weight and store(here with blending)
  auto weight_recip = _mm_set1_epi16(inv_table[weight_acc]);

  mm4 = _mm_mulhi_epi16(mm4, weight_recip);
  mm5 = _mm_mulhi_epi16(mm5, weight_recip);
  mm6 = _mm_mulhi_epi16(mm6, weight_recip);
  mm7 = _mm_mulhi_epi16(mm7, weight_recip);

  // FIXED: original mmx was shifting a whole 64 bit together but there are 4x16 bit numbers here
  mm4 = _mm_slli_epi16(mm4, 7); // psllq mm4, 7  !! psllq = _mm_slli_epi64(reg, 7)
  mm5 = _mm_slli_epi16(mm5, 7); // psllq mm5, 7
  mm6 = _mm_slli_epi16(mm6, 7); // psllq mm6, 7
  mm7 = _mm_slli_epi16(mm7, 7); // psllq mm7, 7

  auto weight_lo16 = _mm_set1_epi16(prev_weight & 0xFFFF); // lower 16 bit

  mm4 = _mm_mulhi_epi16(mm4, weight_lo16);
  mm5 = _mm_mulhi_epi16(mm5, weight_lo16);
  mm6 = _mm_mulhi_epi16(mm6, weight_lo16);
  mm7 = _mm_mulhi_epi16(mm7, weight_lo16);

  auto weight_hi16 = _mm_set1_epi16(prev_weight >> 16); // upper 16 bit

  auto zero = _mm_setzero_si128(); // packer zero mm0

  /*
    blend is >>5 inside then it would need rounder_16. and not 24 (16+8)
    // to be change to the good one after porting
    pcmpeqd mm1, mm1      1111111111111111
    psrlw	mm1, 14                       11
    psllw	mm1, 3                     11000 // 16+8? why not 16
  */
  auto rounder_sixteen = _mm_set1_epi16(16); // FIXED: this must be 16

  simd_blend_store4(ptrb + 0 * pitchb, mm4, weight_hi16, rounder_sixteen, zero);
  simd_blend_store4(ptrb + 1 * pitchb, mm5, weight_hi16, rounder_sixteen, zero);
  simd_blend_store4(ptrb + 2 * pitchb, mm6, weight_hi16, rounder_sixteen, zero);
  simd_blend_store4(ptrb + 3 * pitchb, mm7, weight_hi16, rounder_sixteen, zero);
}

AVS_FORCEINLINE void frcore_filter_overlap_b4r3_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  frcore_filter_overlap_b4r2or3_simd<3>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, inv_table, weight);
}

// used in adaptive overlapping
// bottleneck in P = 1
AVS_FORCEINLINE void frcore_filter_overlap_b4r2_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{
  frcore_filter_overlap_b4r2or3_simd<2>(ptrr, pitchr, ptra, pitcha, ptrb, pitchb, thresh, inv_table, weight);
}

// mmA is input/output. In simd_blend_store4 mmA in input only
AVS_FORCEINLINE void simd_blend_diff4(uint8_t* esi, __m128i &mmA, __m128i mm2_multiplier, __m128i mm1_rounder, __m128i mm0_zero)
{
  auto mm3 = _mm_unpacklo_epi8(_mm_load_si32(esi), mm0_zero);
  // tmp= ((esi << 6) * multiplier) >> 16  ( == [esi]/1024 * multiplier)
  // mmA = (mmA + tmp + rounder_16) / 32
  // ((((mm1 << 2) * multiplier) >> 16 ) + 1) >> 1
  mm3 = _mm_slli_epi16(mm3, 6);
  mm3 = _mm_mulhi_epi16(mm3, mm2_multiplier); // pmulhw, signed
  mmA = _mm_adds_epu16(mmA, mm3);
  mmA = _mm_adds_epu16(mmA, mm1_rounder);
  mmA = _mm_srli_epi16(mmA, 5);
  mmA = _mm_packus_epi16(mmA, mm0_zero); // 4 words to 4 bytes
  *(uint32_t*)(esi) = _mm_cvtsi128_si32(mmA);
  mmA = _mm_sad_epu8(mmA, mm3); // this is the only difference from simd_blend_store4
}

AVS_FORCEINLINE void frcore_filter_diff_b4r1_simd(const uint8_t* ptrr, int pitchr, const uint8_t* ptra, int pitcha, uint8_t* ptrb, int pitchb, int thresh, const int* inv_table, int* weight)
{

  ptra += -1 * pitcha - 1; //  cpln(-1, -1)

  int weight_acc = 0;

  // reference pixels
  auto m0 = _mm_load_si32(ptrr); // 4 bytes
  auto m1 = _mm_load_si32(ptrr + pitchr * 1);
  auto m2 = _mm_load_si32(ptrr + pitchr * 2);
  auto m3 = _mm_load_si32(ptrr + pitchr * 3);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  // accumulators
  // each collects 4 words (weighted sums)
  // which will be finally scaled back and stored as 4 bytes
  auto mm4 = _mm_setzero_si128();
  auto mm5 = _mm_setzero_si128();
  auto mm6 = _mm_setzero_si128();
  auto mm7 = _mm_setzero_si128();

  // -1
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  ptra += pitcha; // next line

  // 0
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  ptra += pitcha; // next line

  // 0
  simd_check(ref01, ref23, 0, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 1, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);
  simd_check(ref01, ref23, 2, ptra, pitcha, weight_acc, thresh, mm4, mm5, mm6, mm7);

  // mm4 - mm7 has accumulated sum, weight is ready here

  // weight variable is a multi-purpose one, here we get a 32 bit value,
  // which is really two 16 bit words
  // lower 16 and upper 16 bit has separate meaning
  int prev_weight = *weight;

  // scale 4 - 7 by weight and store(here with blending)
  auto weight_recip = _mm_set1_epi16(inv_table[weight_acc]);

  mm4 = _mm_mulhi_epi16(mm4, weight_recip);
  mm5 = _mm_mulhi_epi16(mm5, weight_recip);
  mm6 = _mm_mulhi_epi16(mm6, weight_recip);
  mm7 = _mm_mulhi_epi16(mm7, weight_recip);

  // FIXED: original mmx was shifting a whole 64 bit together but there are 4x16 bit numbers here
  mm4 = _mm_slli_epi16(mm4, 7); // psllq mm4, 7  !! psllq = _mm_slli_epi64(reg, 7)
  mm5 = _mm_slli_epi16(mm5, 7); // psllq mm5, 7
  mm6 = _mm_slli_epi16(mm6, 7); // psllq mm6, 7
  mm7 = _mm_slli_epi16(mm7, 7); // psllq mm7, 7

  auto weight_lo16 = _mm_set1_epi16(prev_weight & 0xFFFF); // lower 16 bit

  mm4 = _mm_mulhi_epi16(mm4, weight_lo16);
  mm5 = _mm_mulhi_epi16(mm5, weight_lo16);
  mm6 = _mm_mulhi_epi16(mm6, weight_lo16);
  mm7 = _mm_mulhi_epi16(mm7, weight_lo16);

  auto weight_hi16 = _mm_set1_epi16(prev_weight >> 16); // upper 16 bit

  auto zero = _mm_setzero_si128(); // packer zero mm0

  /*
    blend is >>5 inside then it would need rounder_16. and not 24 (16+8)
    // to be change to the good one after porting
    pcmpeqd mm1, mm1      1111111111111111
    psrlw	mm1, 14                       11
    psllw	mm1, 3                     11000 // 16+8? why not 16
  */
  auto rounder_sixteen = _mm_set1_epi16(16); // FIXED: this must be 16

  simd_blend_diff4(ptrb + 0 * pitchb, mm4, weight_hi16, rounder_sixteen, zero);
  simd_blend_diff4(ptrb + 1 * pitchb, mm5, weight_hi16, rounder_sixteen, zero);
  simd_blend_diff4(ptrb + 2 * pitchb, mm6, weight_hi16, rounder_sixteen, zero);
  simd_blend_diff4(ptrb + 3 * pitchb, mm7, weight_hi16, rounder_sixteen, zero);

  *weight = _mm_cvtsi128_si32(_mm_add_epi16(_mm_add_epi16(mm4, mm5), _mm_add_epi16(mm6, mm7)));
  // mm4, mm5, mm6, mm7 are changed, outputs are SAD
}

AVS_FORCEINLINE void frcore_dev_b4_simd(const uint8_t* ptra, int pitcha, int* dev)
{

  ptra += - 1; // cpln(-1, 0).ptr;

  // reference pixels
  auto m0 = _mm_load_si32(ptra + 1); // 4 bytes
  auto m1 = _mm_load_si32(ptra + pitcha * 1 + 1);
  auto m2 = _mm_load_si32(ptra + pitcha * 2 + 1);
  auto m3 = _mm_load_si32(ptra + pitcha * 3 + 1);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  ptra += pitcha;

  int sad1;
  simd_sad16(ref01, ref23, 0, ptra, pitcha, sad1);

  int sad2;
  simd_sad16(ref01, ref23, 2, ptra, pitcha, sad2);

  *dev = std::min(sad1, sad2);
}

AVS_FORCEINLINE void frcore_sad_b4_simd(const uint8_t* ptra, int pitcha, const uint8_t* ptrb, int pitchb, int* sad)
{
  // reference pixels
  auto m0 = _mm_load_si32(ptra); // 4 bytes
  auto m1 = _mm_load_si32(ptra + pitcha * 1);
  auto m2 = _mm_load_si32(ptra + pitcha * 2);
  auto m3 = _mm_load_si32(ptra + pitcha * 3);

  // 4x4 pixels to 2x8 bytes
  auto ref01 = _mm_or_si128(_mm_slli_epi64(m0, 32), m1); // mm0: 2x4 = 8 bytes
  auto ref23 = _mm_or_si128(_mm_slli_epi64(m2, 32), m3); // mm1: 2x4 = 8 bytes

  int sad1;
  simd_sad16(ref01, ref23, 0, ptrb, pitchb, sad1);

  *sad = sad1;
}

AVS_FORCEINLINE int get_weight(int alpha)
{
  int a = ((alpha * (1 << 15)) / ((alpha + 1)));
  int b = ((1 << 15) / ((alpha + 1)));
  return (a << 16) | b;
}

AVS_FORCEINLINE int clipb(int weight) {
  return weight < 0 ? 0 : weight > 255 ? 255 : weight;
}



typedef struct Frfun7Data {
    VSNodeRef *clip;
    const VSVideoInfo *vi;

    int process[3];

    int inv_table[1024];
    int lambda, Thresh_luma, Thresh_chroma;
    int P;
    int P1_param;
    int R_1stpass; // Radius of first pass, originally 3, can be 2 as well
} Frfun7Data;


static void VS_CC frfun7Init(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;

    Frfun7Data *d = (Frfun7Data *) *instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}


static const VSFrameRef *VS_CC frfun7GetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

    const Frfun7Data *d = (const Frfun7Data *) *instanceData;

    const int P = d->P;
    const int Thresh_luma = d->Thresh_luma;
    const int Thresh_chroma = d->Thresh_chroma;
    const int R_1stpass = d->R_1stpass;
    const int lambda = d->lambda;
    const int *inv_table = d->inv_table;
    const int P1_param = d->P1_param;

    const bool mode_adaptive_overlapping = P & 1;
    const bool mode_temporal = P & 2;
    const bool mode_adaptive_radius = P & 4;

    if (activationReason == arInitial) {
        if (mode_temporal)
            vsapi->requestFrameFilter(std::max(0, n - 1), d->clip, frameCtx);

        vsapi->requestFrameFilter(n, d->clip, frameCtx);

        if (mode_temporal)
            vsapi->requestFrameFilter(std::min(n + 1, d->vi->numFrames - 1), d->clip, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *cf = vsapi->getFrameFilter(n, d->clip, frameCtx);

        const VSFormat *fmt = vsapi->getFrameFormat(cf);

        if (fmt->bitsPerSample > 8) {
            vsapi->setFilterError("Frfun7: only 8 bit video is allowed", frameCtx);
            vsapi->freeFrame(cf);
            return nullptr;
        }

        if (fmt->colorFamily != cmGray && fmt->colorFamily != cmYUV) {
            vsapi->setFilterError("Frfun7: only gray or YUV video is allowed", frameCtx);
            vsapi->freeFrame(cf);
            return nullptr;
        }

        const VSFrameRef *pf = nullptr; // previous
        const VSFrameRef *nf = nullptr; // next

        if (mode_temporal) {
          pf = vsapi->getFrameFilter(std::max(0, n - 1), d->clip, frameCtx);
          nf = vsapi->getFrameFilter(std::min(n + 1, d->vi->numFrames - 1), d->clip, frameCtx);
        }

        const VSFrameRef *frames[3] = {
            d->process[0] ? nullptr : cf,
            d->process[1] ? nullptr : cf,
            d->process[2] ? nullptr : cf
        };
        int planes[3] = { 0, 1, 2 };

        VSFrameRef *df = vsapi->newVideoFrame2(fmt,
                                               vsapi->getFrameWidth(cf, 0),
                                               vsapi->getFrameHeight(cf, 0),
                                               frames, planes, cf, core);


        uint8_t *wpln = nullptr; // weight buffer videosize_x/4,videosize_y/4
        int wp_width = vsapi->getFrameWidth(cf, 0) / 4; // internal subsampling is 4
        int wp_height = vsapi->getFrameHeight(cf, 0) / 4;
        const int ALIGN = 32;
        int wp_stride = (((wp_width)+(ALIGN)-1) & (~((ALIGN)-1)));

        if (mode_adaptive_overlapping)
            wpln = vs_aligned_malloc<uint8_t>(wp_stride * wp_height, ALIGN);


        const int num_of_planes = d->vi->format->numPlanes;
        for (int plane = 0; plane < num_of_planes; plane++) { // PLANES LOOP

          const bool chroma = plane > 0;

          if (!d->process[plane])
            continue;


          const int dim_x = vsapi->getFrameWidth(cf, plane);
          const int dim_y = vsapi->getFrameHeight(cf, plane);

          // prev/next: only for temporal
          const uint8_t* srcp_prev_orig = nullptr;
          const uint8_t* srcp_next_orig = nullptr;
          int src_prev_pitch = 0;
          int src_next_pitch = 0;

          if (mode_temporal) {
            srcp_prev_orig = vsapi->getReadPtr(pf, plane);
            src_prev_pitch = vsapi->getStride(pf, plane);

            srcp_next_orig = vsapi->getReadPtr(nf, plane);
            src_next_pitch = vsapi->getStride(nf, plane);
          }

          const uint8_t* srcp_orig = vsapi->getReadPtr(cf, plane);
          const int src_pitch = vsapi->getStride(cf, plane);

          uint8_t* dstp_orig = vsapi->getWritePtr(df, plane);
          const int dstp_pitch = vsapi->getStride(df, plane);

          int tmax = Thresh_luma;
          if (chroma) tmax = Thresh_chroma;

          const int R = R_1stpass; // originally 3, experimental: 2
          constexpr int B = 4;
          constexpr int S = 4;

          for (int y = 0; y < dim_y + B - 1; y += S)
          {
            int sy = y;
            int by = y;
            if (sy < R) sy = R;
            if (sy > dim_y - R - B) sy = dim_y - R - B;
            if (by > dim_y - B) by = dim_y - B;

            uint8_t* dstp_curr_by = dstp_orig + dstp_pitch * by;
            const uint8_t* srcp_curr_sy = srcp_orig + src_pitch * sy; // cpln(sx, sy)
            const uint8_t* srcp_curr_by = srcp_orig + src_pitch * by; // cpln(bx, by)

            for (int x = 0; x < dim_x + B - 1; x += S)
            {
              int sx = x;
              int bx = x;
              if (sx < R) sx = R;
              if (sx > dim_x - R - B) sx = dim_x - R - B;
              if (bx > dim_x - B) bx = dim_x - B;

              uint8_t* dstp = dstp_curr_by + bx;
              const uint8_t* srcp_s = srcp_curr_sy + sx; // cpln(sx, sy)
              const uint8_t* srcp_b = srcp_curr_by + bx; // cpln(bx, by)

              int dev, devp, devn;
              frcore_dev_b4_simd(srcp_s, src_pitch, &dev);

              // only for temporal use
              const uint8_t* srcp_next_s = nullptr;
              const uint8_t* srcp_prev_s = nullptr;

              if (mode_temporal)
              {
                srcp_prev_s = srcp_prev_orig + src_prev_pitch * sy + sx; // ppln(sx, sy)
                frcore_sad_b4_simd(srcp_s, src_pitch, srcp_prev_s, src_prev_pitch, &devp);

                srcp_next_s = srcp_next_orig + src_next_pitch * sy + sx; // npln(sx, sy)
                frcore_sad_b4_simd(srcp_s, src_pitch, srcp_next_s, src_next_pitch, &devn);

                dev = std::min(dev, devn);
                dev = std::min(dev, devp);
              }

              int thresh = ((dev * lambda) >> 10);
              thresh = (thresh > tmax) ? tmax : thresh;
              if (thresh < 1) thresh = 1;


              int weight;
              if (mode_temporal) {
                frcore_filter_b4r0_simd(srcp_b, src_pitch, srcp_b, src_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);

                int k = 1;
                if (devp < thresh)
                {
                  weight = get_weight(k); // two 16 bit values inside
                  R == 2 ?
                    frcore_filter_overlap_b4r2_simd(srcp_b, src_pitch, srcp_prev_s, src_prev_pitch, dstp, dstp_pitch, thresh, inv_table, &weight):
                    frcore_filter_overlap_b4r3_simd(srcp_b, src_pitch, srcp_prev_s, src_prev_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);
                  k++;
                }

                if (devn < thresh)
                {
                  weight = get_weight(k); // two 16 bit values inside
                  R == 2 ?
                    frcore_filter_overlap_b4r2_simd(srcp_b, src_pitch, srcp_next_s, src_next_pitch, dstp, dstp_pitch, thresh, inv_table, &weight) :
                    frcore_filter_overlap_b4r3_simd(srcp_b, src_pitch, srcp_next_s, src_next_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);
                }
              }
              else
              {
                // not temporal
                if (sx == x && sy == y && mode_adaptive_radius) {
                  constexpr int thresh2 = 16 * 9; // First try with R=1 then if over threshold R=2 then R=3
                  constexpr int thresh3 = 16 * 25; // only when R=3
                  R == 2 ?
                    frcore_filter_adapt_b4r2_simd(srcp_b, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, thresh2, thresh3, inv_table, &weight) :
                    frcore_filter_adapt_b4r3_simd(srcp_b, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, thresh2, thresh3, inv_table, &weight);
                }
                else {
                  // Nothing or adaptive_overlapping or some case of adaptive_radius
                  R == 2 ?
                    frcore_filter_b4r2_simd(srcp_b, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, inv_table, &weight) :
                    frcore_filter_b4r3_simd(srcp_b, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);
                }
              }

            }
          }

          if (mode_adaptive_overlapping)
          {
            for (int y = 2; y < dim_y - B; y += S)
            {
              constexpr int R = 1;

              int sy = y;
              if (sy < R) sy = R;
              if (sy > dim_y - R - B) sy = dim_y - R - B;

              const uint8_t* srcp_curr_sy = srcp_orig + src_pitch * sy; // cpln(sx, sy)
              const uint8_t* srcp_curr_y = srcp_orig + src_pitch * y; // cpln(x, y)
              uint8_t* dstp_curr_y = dstp_orig + dstp_pitch * y ;

              for (int x = 2; x < dim_x - B; x += S)
              {
                int sx = x;
                if (sx < R) sx = R;
                if (sx > dim_x - R - B) sx = dim_x - R - B;

                int dev = 10;
                const uint8_t* srcp_s = srcp_curr_sy + sx; // cpln(sx, sy)
                frcore_dev_b4_simd(srcp_s, src_pitch, &dev);

                int thresh = ((dev * lambda) >> 10);
                thresh = (thresh > tmax) ? tmax : thresh;
                if (thresh < 1) thresh = 1;

                const uint8_t* srcp_xy = srcp_curr_y + x; // cpln(x, y)
                uint8_t* dstp = dstp_curr_y + x;

                int weight = get_weight(1);
                frcore_filter_diff_b4r1_simd(srcp_xy, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);

                wpln[wp_stride * (y / 4) + (x / 4)] = clipb(weight);
              }
            }

            for (int kk = 1; kk < 9; kk++)
            {
              constexpr int R = 2;

              int k = kk;

              for (int y = (k / 3) + 1; y < dim_y - B; y += S)
              {
                int sy = y;
                if (sy < R) sy = R;
                if (sy > dim_y - R - B) sy = dim_y - R - B;

                const uint8_t* srcp_curr_sy = srcp_orig + src_pitch * sy;
                const uint8_t* srcp_curr_y = srcp_orig + src_pitch * y;
                uint8_t* dstp_curr_y = dstp_orig + dstp_pitch * y;

                for (int x = (k % 3) + 1; x < dim_x - B; x += S)
                {
                  int sx = x;
                  if (sx < R) sx = R;
                  if (sx > dim_x - R - B) sx = dim_x - R - B;

                  if (wpln[wp_stride * (y / 4) + (x / 4)] < P1_param)
                    continue;

                  int dev = 10;
                  const uint8_t* srcp_s = srcp_curr_sy + sx; // cpln(sx, sy)
                  frcore_dev_b4_simd(srcp_s, src_pitch, &dev);

                  int thresh = ((dev * lambda) >> 10);
                  thresh = (thresh > tmax) ? tmax : thresh;
                  if (thresh < 1) thresh = 1;

                  uint8_t* dstp = dstp_curr_y + x;
                  const uint8_t* srcp_xy = srcp_curr_y + x; // cpln(x, y)
                  int weight = get_weight(k); // two 16 bit words inside
                  frcore_filter_overlap_b4r2_simd(srcp_xy, src_pitch, srcp_s, src_pitch, dstp, dstp_pitch, thresh, inv_table, &weight);
                }
              }

            }
          } // adaptive overlapping
        } // PLANES LOOP

        vsapi->freeFrame(cf);
        vsapi->freeFrame(pf);
        vsapi->freeFrame(nf);
        if (wpln)
            vs_aligned_free(wpln);

        return df;
    }

    return nullptr;
}


static void VS_CC frfun7Free(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

    Frfun7Data *d = (Frfun7Data *)instanceData;

    vsapi->freeNode(d->clip);
    free(d);
}


static void VS_CC frfun7Create(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    Frfun7Data d;
    memset(&d, 0, sizeof(d));

    int err;

    double lambda = vsapi->propGetFloat(in, "lambda", 0, &err);
    if (err)
        lambda = 1.1;

    d.lambda = (int)(lambda * 1024); // 10 bit integer arithmetic


    double t = vsapi->propGetFloat(in, "t", 0, &err);
    if (err)
        t = 6;

    d.Thresh_luma = (int)(t * 16); // internal subsampling is 4x4, probably x16 covers that


    double tuv = vsapi->propGetFloat(in, "tuv", 0, &err);
    if (err)
        tuv = 2;

    d.Thresh_chroma = (int)(tuv * 16);


    d.P = int64ToIntS(vsapi->propGetInt(in, "p", 0, &err));
    if (err)
        d.P = 0;

    d.P &= 7;


    d.P1_param = int64ToIntS(vsapi->propGetInt(in, "tp1", 0, &err));
    if (err)
        d.P1_param = 0;


    d.R_1stpass = int64ToIntS(vsapi->propGetInt(in, "r1", 0, &err));
    if (err)
        d.R_1stpass = 3;


    d.process[0] = d.Thresh_luma != 0;
    d.process[1] = d.Thresh_chroma != 0;
    d.process[2] = d.process[1];


    if (d.lambda < 0) {
        vsapi->setError(out, "Frfun7: lambda cannot be negative");
        return;
    }

    if (d.Thresh_luma < 0 || d.Thresh_chroma < 0) {
        vsapi->setError(out, "Frfun7: threshold cannot be negative");
        return;
    }

    if (d.R_1stpass != 2 && d.R_1stpass != 3) {
        vsapi->setError(out, "Frfun7: r1 (1st pass radius) must be 2 or 3");
        return;
    }


    d.clip = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.vi = vsapi->getVideoInfo(d.clip);


    // pre-build reciprocial table
    for (int i = 1; i < 1024; i++) {
      // 1/x table 1..1023 for 15 bit integer arithmetic
      d.inv_table[i] = (int)((1 << 15) / (double)i);
    }
    d.inv_table[1] = 32767; // 2^15 - 1


    Frfun7Data *data = (Frfun7Data *)malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Frfun7", frfun7Init, frfun7GetFrame, frfun7Free, fmParallel, 0, data, core);
}


VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.nodame.frfun7", "frfun7", "A spatial denoising filter", (3 << 16) | 5, 1, plugin);
    registerFunc("Frfun7",
                 "clip:clip;"
                 "lambda:float:opt;"
                 "t:float:opt;"
                 "tuv:float:opt;"
                 "p:int:opt;"
                 "tp1:int:opt;"
                 "r1:int:opt;"
                 , frfun7Create, nullptr, plugin);
}