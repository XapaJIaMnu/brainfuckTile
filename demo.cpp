#include <immintrin.h>
#include <iostream>
#include <stdlib.h>
#include <cstring>
#include <math.h>

/************************************************************************************ util ************************************************************************************/
template <class T>
int numDigits(T number) {
    int digits = 0;
    if (number <= 0) {
      digits = 1; // count the minus and take care of the zero case
    }
    while (number) {
        number /= 10;
        digits++;
    }
    return digits;
}

template<class intType>
void printMat(intType * a, size_t rows, size_t cols, std::string name, int digits = 0) {
  std::cerr << name << std::endl;
  for (size_t i = 0; i < rows; i++) {
    for (size_t j = 0; j < cols; j++) {
      int numbah = (int)a[i*cols + j];
      // Pad for nice printing
      int mydigits = digits - numDigits(numbah);
      if (mydigits > 0) {
        for (int t = 0; t < mydigits; t++) {
          std::cerr << ' ';
        }
      }
      std::cerr << numbah << " ";
    }
      std::cerr << std::endl;
  }
  std::cerr << std::endl;
}

template<class intType>
void toColMajor(intType *in, intType * out, size_t rows, size_t cols) {
  for (size_t i = 0; i < rows; i++) {
    for (size_t j = 0; j < cols; j++) {
      out[j*rows + i] = in[i*cols + j];
    }
  }
}

template<class intType>
void gemmRowMColM(intType *A, intType * B, size_t rowsA, size_t width, size_t colsB, int * C) {
  for (size_t i = 0; i<rowsA; i++) {
    for (size_t j = 0; j< colsB; j++) {
      for (size_t w = 0; w<width; w++) {
        C[i*colsB + j] += A[i*width + w]*B[j*width +w];
      }
    }
  }
}

/************************************************************************************ mm128 code ************************************************************************************/
void prepareBtile(__m128i *bmat, __m128i *breord) {
  //Assuming at the moment that we are dealing with 4x16 X 16x gemm, as this is just a demo
  //Assume B comes in columnMajor format

  // Column 0 is just BLEND/ We use int32 blend, as we only are intersted in groups of 4
  breord[0] = _mm_blend_epi32(bmat[0],   bmat[1], 0b0010);
  breord[0] = _mm_blend_epi32(breord[0], bmat[2], 0b0100);
  breord[0] = _mm_blend_epi32(breord[0], bmat[3], 0b1000);

  // Column 1 is BLEND + shuffle.
  breord[1] = _mm_blend_epi32(bmat[1],   bmat[0], 0b0010);
  breord[1] = _mm_blend_epi32(breord[1], bmat[3], 0b0100);
  breord[1] = _mm_blend_epi32(breord[1], bmat[2], 0b1000);

  auto static const constexpr mask1 = _MM_SHUFFLE(2,3,0,1); // it's reversed because of being big endian
  breord[1] = _mm_shuffle_epi32(breord[1], mask1);  // We use int32 shuffle as we conveniently need to shuffle int8s in groups of four and 4int8s are the same width as a int32t

  // Column 2 again BLEND + shuffle
  breord[2] = _mm_blend_epi32(bmat[2],   bmat[3], 0b0010);
  breord[2] = _mm_blend_epi32(breord[2], bmat[1], 0b0100);
  breord[2] = _mm_blend_epi32(breord[2], bmat[0], 0b1000);

  auto static const constexpr mask2 = _MM_SHUFFLE(1,0,2,3); // it's reversed because of being big endian
  breord[2] = _mm_shuffle_epi32(breord[2], mask2);

  // Column 3 final BLEND + shuffle
  breord[3] = _mm_blend_epi32(bmat[3],   bmat[2], 0b0010);
  breord[3] = _mm_blend_epi32(breord[3], bmat[0], 0b0100);
  breord[3] = _mm_blend_epi32(breord[3], bmat[1], 0b1000);
  auto static const constexpr mask3 = _MM_SHUFFLE(0,1,3,2); // it's reversed because of being big endian
  breord[3] = _mm_shuffle_epi32(breord[3], mask3);
}

void multiplyTile(__m128i * amat, __m128i * breord, __m128i * res) {
  __m128i atmp; // Temporary register for reodering A

  // We could potentially hold the whole tile in registers, since we don't require that many by statically unrolling this loop
  // The advantage of this method is that we should have extreme memory locality and cache locality. While A is indeed manipulated
  // on the fly, it is kept in registers which should make the operation crazy fast.
  // B is accessed consecutively and the whole tile could be kept into registers if we unroll the loop
  // C is accessed one register at a time and consecutively. No expensive scatter instructions
  for (int i = 0; i < 4; i++) {
    // We load A once, perform four multiply-adds and write into C. The four dpbusds operations will produce for consecutive for-major results
    // Additional work is 3 permute operations and additional space required is one temporary register
    res[i] = _mm_set1_epi32(0); // Set the initial result to zero. Potentially, do some bias reading here, or this could be done outside

    // Multiply 0
    res[i] = _mm_dpbusds_epi32 (res[i], amat[i], breord[0]);

    // Multiply 1: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask1 = _MM_SHUFFLE(2,3,0,1); // it's reversed because of being big endian
    atmp = _mm_shuffle_epi32(amat[i], mask1);
    res[i] = _mm_dpbusds_epi32 (res[i], atmp, breord[1]);

    // Multiply 2: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask2 = _MM_SHUFFLE(1,0,2,3);
    atmp = _mm_shuffle_epi32(amat[i], mask2);
    res[i] = _mm_dpbusds_epi32 (res[i], atmp, breord[2]);

    // Multiply 3: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask3 = _MM_SHUFFLE(0,1,3,2); // it's reversed because of being big endian
    atmp = _mm_shuffle_epi32(amat[i], mask3);
    res[i] = _mm_dpbusds_epi32 (res[i], atmp, breord[3]);
  }
}

/************************************************************************************ mm256 code ************************************************************************************/

inline void prepareBtileSubRoutine(__m256i *bmat, __m256i *breord) {
  //Second part of shuffling requires us to lane swap all of bmat. Call this function twice to get it

  // Column 0 is just BLEND/ We use int32 blend, as we only are intersted in groups of 4
  breord[0] = _mm256_blend_epi32(bmat[0],   bmat[1], 0b00000010);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[2], 0b00000100);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[3], 0b00001000);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[4], 0b00010000);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[5], 0b00100000);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[6], 0b01000000);
  breord[0] = _mm256_blend_epi32(breord[0], bmat[7], 0b10000000);

  // Column 1 is BLEND + shuffle.
  breord[1] = _mm256_blend_epi32(bmat[1],   bmat[0], 0b00000010);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[3], 0b00000100);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[2], 0b00001000);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[5], 0b00010000);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[4], 0b00100000);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[7], 0b01000000);
  breord[1] = _mm256_blend_epi32(breord[1], bmat[6], 0b10000000);

  auto static const constexpr mask1 = _MM_SHUFFLE(2,3,0,1); // it's reversed because of being big endian
  breord[1] = _mm256_shuffle_epi32(breord[1], mask1);  // We use int32 shuffle as we conveniently need to shuffle int8s in groups of four and 4int8s are the same width as a int32t

  // Column 2 again BLEND + shuffle
  breord[2] = _mm256_blend_epi32(bmat[2],   bmat[3], 0b00000010);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[1], 0b00000100);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[0], 0b00001000);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[6], 0b00010000);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[7], 0b00100000);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[5], 0b01000000);
  breord[2] = _mm256_blend_epi32(breord[2], bmat[4], 0b10000000);

  auto static const constexpr mask2 = _MM_SHUFFLE(1,0,2,3); // it's reversed because of being big endian
  breord[2] = _mm256_shuffle_epi32(breord[2], mask2);

  // Column 3 final BLEND + shuffle
  breord[3] = _mm256_blend_epi32(bmat[3],   bmat[2], 0b00000010);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[0], 0b00000100);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[1], 0b00001000);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[7], 0b00010000);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[6], 0b00100000);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[4], 0b01000000);
  breord[3] = _mm256_blend_epi32(breord[3], bmat[5], 0b10000000);
  auto static const constexpr mask3 = _MM_SHUFFLE(0,1,3,2); // it's reversed because of being big endian
  breord[3] = _mm256_shuffle_epi32(breord[3], mask3);
}

inline void swapLanes(__m256i *input, __m256i *output) {
  for (int i = 0; i < 8; i++) {
    output[i] = _mm256_permute2x128_si256(input[i], input[i], 0b0101); // Technically we use only one input but this seems to work
  }
}

void prepareBtile(__m256i *bmat, __m256i *breord) {
  // Split into two parts that do identical things, except with lane swapped bmat
  prepareBtileSubRoutine(bmat, breord);

  //Second part of shuffling requires us to lane swap all of bmat
  __m256i * bmatlaneswap = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024));
  std::memcpy(bmatlaneswap, bmat, 32*8*sizeof(int8_t));
  swapLanes(bmat, bmatlaneswap);
  prepareBtileSubRoutine(bmatlaneswap, &breord[4]);

  delete bmatlaneswap;
}


void multiplyTile(__m256i * amat, __m256i * breord, __m256i * res) {
  __m256i atmp; // Temporary register for reodering A
  __m256i laneSwappedA;

  // We could potentially hold the whole tile in registers, since we don't require that many by statically unrolling this loop
  // The advantage of this method is that we should have extreme memory locality and cache locality. While A is indeed manipulated
  // on the fly, it is kept in registers which should make the operation crazy fast.
  // B is accessed consecutively and the whole tile could be kept into registers if we unroll the loop
  // C is accessed one register at a time and consecutively. No expensive scatter instructions
  for (int i = 0; i < 8; i++) {
    // We load A once, perform four multiply-adds and write into C. The four dpbusds operations will produce for consecutive for-major results
    // Additional work is 3 permute operations and additional space required is one temporary register
    res[i] = _mm256_set1_epi32(0); // Set the initial result to zero. Potentially, do some bias reading here, or this could be done outside

    // Multiply 0
    res[i] = _mm256_dpbusds_epi32 (res[i], amat[i], breord[0]);

    // Multiply 1: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask1 = _MM_SHUFFLE(2,3,0,1); // it's reversed because of being big endian
    atmp = _mm256_shuffle_epi32(amat[i], mask1);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[1]);

    // Multiply 2: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask2 = _MM_SHUFFLE(1,0,2,3);
    atmp = _mm256_shuffle_epi32(amat[i], mask2);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[2]);

    // Multiply 3: //Shuffle A in the same way as B was permuted and the multiply
    auto static const constexpr mask3 = _MM_SHUFFLE(0,1,3,2); // it's reversed because of being big endian
    atmp = _mm256_shuffle_epi32(amat[i], mask3);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[3]);

    //Lane swap amat[i] here:
    laneSwappedA = _mm256_permute2x128_si256(amat[i], amat[i], 0b0101);

    // Multiply 4
    res[i] = _mm256_dpbusds_epi32 (res[i], laneSwappedA, breord[4]);

    // Multiply 5: //Shuffle A in the same way as B was permuted and the multiply
    // auto static const constexpr mask1 = _MM_SHUFFLE(2,3,0,1); // it's reversed because of being big endian
    atmp = _mm256_shuffle_epi32(laneSwappedA, mask1);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[5]);

    // Multiply 6: //Shuffle A in the same way as B was permuted and the multiply
    // auto static const constexpr mask2 = _MM_SHUFFLE(1,0,2,3);
    atmp = _mm256_shuffle_epi32(laneSwappedA, mask2);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[6]);

    // Multiply 7: //Shuffle A in the same way as B was permuted and the multiply
    // auto static const constexpr mask3 = _MM_SHUFFLE(0,1,3,2); // it's reversed because of being big endian
    atmp = _mm256_shuffle_epi32(laneSwappedA, mask3);
    res[i] = _mm256_dpbusds_epi32 (res[i], atmp, breord[7]);
  }
}

/************************************************************************************ Test code ************************************************************************************/
void mm128Example() {
  std::cerr << "mm128 example" << std::endl;
  // Let's get some _mm example going
  __m128i * amat = reinterpret_cast<__m128i*>(aligned_alloc(16*4*sizeof(int8_t), 1024));
  __m128i * bmat = reinterpret_cast<__m128i*>(aligned_alloc(16*4*sizeof(int8_t), 1024));
  __m128i * cres = reinterpret_cast<__m128i*>(aligned_alloc(4*4*sizeof(int32_t), 1024));
  __m128i * breord = reinterpret_cast<__m128i*>(aligned_alloc(16*4*sizeof(int8_t), 1024));

  // Populate
  for (int i = 0; i<64; i++) {
    reinterpret_cast<int8_t*>(amat)[i] = (int8_t)i;
    reinterpret_cast<int8_t*>(bmat)[i] = (int8_t)i;
  }

  // We need cres to be zero. We can do this inside the hotloop out beforehand or do some bias preprocessing beforehand.
  // std::memset(cres, 0, 4*4*sizeof(int32_t));

  // This is just to print the two matrices so that we see what we are multiplying
  __m128i * bmatcolm = reinterpret_cast<__m128i*>(aligned_alloc(16*4*sizeof(int8_t), 1024));
  toColMajor(reinterpret_cast<int8_t *>(bmat), reinterpret_cast<int8_t *>(bmatcolm), 4, 16);
  // Our matrices to multiply rowM *colM
  printMat(reinterpret_cast<int8_t *>(amat), 4, 16, "A int8", 2);
  printMat(reinterpret_cast<int8_t *>(bmatcolm), 16, 4, "B int8 colM", 2);

  __m128i * breordcolm = reinterpret_cast<__m128i*>(aligned_alloc(16*4*sizeof(int8_t), 1024)); // For easier visualisation

  //Sanity check
  gemmRowMColM(reinterpret_cast<int8_t *>(amat), reinterpret_cast<int8_t *>(bmat), 4, 16, 4, reinterpret_cast<int *>(cres));
  printMat(reinterpret_cast<int *>(cres), 4, 4, "A * BcolM SlowMult", 5);

  // FUll pipeline test
  prepareBtile(bmat, breord);

  // For visualisation of the reordered format
  toColMajor(reinterpret_cast<int8_t *>(breord), reinterpret_cast<int8_t *>(breordcolm), 4, 16);
  printMat(reinterpret_cast<int8_t *>(breordcolm), 16, 4, "B int8 colMreord", 2);

  multiplyTile(amat, breord, cres);
  printMat(reinterpret_cast<int32_t *>(&cres[0]), 4, 4, "A * Breord efficient", 5);
}

void mm256Example() {
  std::cerr << "mm256 example" << std::endl;
  // Let's get some _mm example going
  __m256i * amat = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024));
  __m256i * bmat = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024));
  __m256i * cres = reinterpret_cast<__m256i*>(aligned_alloc(8*8*sizeof(int32_t), 1024));
  __m256i * breord = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024));

  // Populate
  for (int i = 0; i<256; i++) {
    reinterpret_cast<int8_t*>(amat)[i] = (int8_t)(i%127); // A needs to be unsigned for the UBS operation, so in order to get the same results as slow gemm, make sure it fits
    reinterpret_cast<int8_t*>(bmat)[i] = (int8_t)(i); // We have unsigned times signed so one will be 0-255 other will be -128-127
  }

  // We need cres to be zero. We can do this inside the hotloop out beforehand or do some bias preprocessing beforehand.
  // std::memset(cres, 0, 4*4*sizeof(int32_t));

  // This is just to print the two matrices so that we see what we are multiplying
  __m256i * bmatcolm = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024));
  toColMajor(reinterpret_cast<int8_t *>(bmat), reinterpret_cast<int8_t *>(bmatcolm), 8, 32);
  // Our matrices to multiply rowM *colM
  printMat(reinterpret_cast<int8_t *>(amat), 8, 32, "A int8", 3);
  printMat(reinterpret_cast<int8_t *>(bmatcolm), 32, 8, "B int8 colM", 4);

  __m256i * breordcolm = reinterpret_cast<__m256i*>(aligned_alloc(32*8*sizeof(int8_t), 1024)); // For easier visualisation

  //Sanity check
  gemmRowMColM(reinterpret_cast<int8_t *>(amat), reinterpret_cast<int8_t *>(bmat), 8, 32, 8, reinterpret_cast<int *>(cres));
  printMat(reinterpret_cast<int *>(cres), 8, 8, "A * BcolM SlowMult", 7);

  // FUll pipeline test
  prepareBtile(bmat, breord);

  // For visualisation of the reordered format
  toColMajor(reinterpret_cast<int8_t *>(breord), reinterpret_cast<int8_t *>(breordcolm), 8, 32);
  printMat(reinterpret_cast<int8_t *>(breordcolm), 32, 8, "B int8 colMreord", 4);

  multiplyTile(amat, breord, cres);
  printMat(reinterpret_cast<int32_t *>(&cres[0]), 8, 8, "A * Breord efficient", 7);
}

int main() {
  mm128Example();
  mm256Example();
}
