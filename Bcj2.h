/* Bcj2.h -- Converter for x86 code (BCJ2)
2009-02-07 : Igor Pavlov : Public domain */

#ifndef __BCJ2_H
#define __BCJ2_H

#include "Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _LZMA_PROB32
#define CProb UInt32
#else
#define CProb UInt16
#endif

#ifdef _LZMA_PROB32
#define CProb UInt32
#else
#define CProb UInt16
#endif


#define kNumTopBits 24
#define kTopValue ((UInt32)1 << kNumTopBits)

#define kNumBitModelTotalBits 11
#define kBitModelTotal (1 << kNumBitModelTotalBits)
#define kNumMoveBits 5
/*
Conditions:
  outSize <= FullOutputSize,
  where FullOutputSize is full size of output stream of x86_2 filter.

If buf0 overlaps outBuf, there are two required conditions:
  1) (buf0 >= outBuf)
  2) (buf0 + size0 >= outBuf + FullOutputSize).

Returns:
  SZ_OK
  SZ_ERROR_DATA - Data error
*/

int Bcj2_Decode(
    const Byte *buf0, SizeT size0,
    const Byte *buf1, SizeT size1,
    const Byte *buf2, SizeT size2,
    const Byte *buf3, SizeT size3,
    Byte *outBuf, SizeT outSize);

typedef struct Bcj2_dec_state
{
    UInt32 out_for_now;
    Byte prev_byte;
    UInt32 code;
    UInt32 range;
    Byte b;
    CProb *prob;
    UInt32 bound;
    UInt32 ttt;
    CProb p[256 + 2];
    Byte *buf1;
    Byte *buf2;
    Byte *buf3;
}   Bcj2_st, *pBcj2_st;

#define Bcj2_dec_state_init(st)         {                           \
                                            int i;                  \
                                            st.out_for_now = 0;           \
                                            st.prev_byte = 0;       \
                                            st.code = 0;            \
                                            st.range = 0xFFFFFFFF;  \
                                            st.b = 0;               \
                                            st.prob = NULL;         \
                                            st.bound = 0;           \
                                            st.ttt = 0;             \
                                            for (i = 0; i < sizeof(st.p) / sizeof(st.p[0]); i++){     \
                                                st.p[i] = kBitModelTotal >> 1;                              \
                                            }                                                               \
                                        };                          \

int Bcj2_DecodeToFileWithBufs(
                const Byte *buf0, SizeT size0,
                Byte *buf1, SizeT *size1,
                Byte *buf2, SizeT *size2,
                Byte *buf3, SizeT *size3,
                Byte *outBuf, SizeT outSize,
                pBcj2_st st);

#ifdef __cplusplus
}
#endif

#endif
