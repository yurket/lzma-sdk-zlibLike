/* 7zDec.c -- Decoding from 7z folder
2010-11-02 : Igor Pavlov : Public domain */

#include <string.h>
#include <stdio.h>
/* #define _7ZIP_PPMD_SUPPPORT */

#include "7z.h"
#include "Bcj2.h"
#include "Bra.h"
#include "CpuArch.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"

#include "7zStream.h"
#ifdef _7ZIP_PPMD_SUPPPORT
#include "Ppmd7.h"
#endif

#define k_Copy 0
#define k_LZMA2 0x21
#define k_LZMA  0x30101
#define k_BCJ   0x03030103
#define k_PPC   0x03030205
#define k_ARM   0x03030501
#define k_ARMT  0x03030701
#define k_SPARC 0x03030805
#define k_BCJ2  0x0303011B

#ifdef _7ZIP_PPMD_SUPPPORT

#define k_PPMD 0x30401

typedef struct
{
  IByteIn p;
  const Byte *cur;
  const Byte *end;
  const Byte *begin;
  UInt64 processed;
  Bool extra;
  SRes res;
  ILookInStream *inStream;
} CByteInToLook;

static Byte ReadByte(void *pp)
{
  CByteInToLook *p = (CByteInToLook *)pp;
  if (p->cur != p->end)
    return *p->cur++;
  if (p->res == SZ_OK)
  {
    size_t size = p->cur - p->begin;
    p->processed += size;
    p->res = p->inStream->Skip(p->inStream, size);
    size = (1 << 25);
    p->res = p->inStream->Look(p->inStream, (const void **)&p->begin, &size);
    p->cur = p->begin;
    p->end = p->begin + size;
    if (size != 0)
      return *p->cur++;;
  }
  p->extra = True;
  return 0;
}

static SRes SzDecodePpmd(CSzCoderInfo *coder, UInt64 inSize, ILookInStream *inStream,
    Byte *outBuffer, SizeT outSize, ISzAlloc *allocMain)
{
  CPpmd7 ppmd;
  CByteInToLook s;
  SRes res = SZ_OK;

  s.p.Read = ReadByte;
  s.inStream = inStream;
  s.begin = s.end = s.cur = NULL;
  s.extra = False;
  s.res = SZ_OK;
  s.processed = 0;

  if (coder->Props.size != 5)
    return SZ_ERROR_UNSUPPORTED;

  {
    unsigned order = coder->Props.data[0];
    UInt32 memSize = GetUi32(coder->Props.data + 1);
    if (order < PPMD7_MIN_ORDER ||
        order > PPMD7_MAX_ORDER ||
        memSize < PPMD7_MIN_MEM_SIZE ||
        memSize > PPMD7_MAX_MEM_SIZE)
      return SZ_ERROR_UNSUPPORTED;
    Ppmd7_Construct(&ppmd);
    if (!Ppmd7_Alloc(&ppmd, memSize, allocMain))
      return SZ_ERROR_MEM;
    Ppmd7_Init(&ppmd, order);
  }
  {
    CPpmd7z_RangeDec rc;
    Ppmd7z_RangeDec_CreateVTable(&rc);
    rc.Stream = &s.p;
    if (!Ppmd7z_RangeDec_Init(&rc))
      res = SZ_ERROR_DATA;
    else if (s.extra)
      res = (s.res != SZ_OK ? s.res : SZ_ERROR_DATA);
    else
    {
      SizeT i;
      for (i = 0; i < outSize; i++)
      {
        int sym = Ppmd7_DecodeSymbol(&ppmd, &rc.p);
        if (s.extra || sym < 0)
          break;
        outBuffer[i] = (Byte)sym;
      }
      if (i != outSize)
        res = (s.res != SZ_OK ? s.res : SZ_ERROR_DATA);
      else if (s.processed + (s.cur - s.begin) != inSize || !Ppmd7z_RangeDec_IsFinishedOK(&rc))
        res = SZ_ERROR_DATA;
    }
  }
  Ppmd7_Free(&ppmd, allocMain);
  return res;
}

#endif


static SRes SzDecodeLzma(CSzCoderInfo *coder, UInt64 inSize, ILookInStream *inStream,
    Byte *outBuffer, SizeT outSize, ISzAlloc *allocMain)
{
  CLzmaDec state;
  SRes res = SZ_OK;

  LzmaDec_Construct(&state);
  RINOK(LzmaDec_AllocateProbs(&state, coder->Props.data, (unsigned)coder->Props.size, allocMain));
  state.dic = outBuffer;
  state.dicBufSize = outSize;
  LzmaDec_Init(&state);

  for (;;)
  {
    Byte *inBuf = NULL;
    size_t lookahead = (1 << 18);
    if (lookahead > inSize)
      lookahead = (size_t)inSize;
    res = inStream->Look((void *)inStream, (const void **)&inBuf, &lookahead);
    if (res != SZ_OK)
      break;

    {
      SizeT inProcessed = (SizeT)lookahead, dicPos = state.dicPos;
      ELzmaStatus status;
      res = LzmaDec_DecodeToDic(&state, outSize, inBuf, &inProcessed, LZMA_FINISH_END, &status);
      lookahead -= inProcessed;
      inSize -= inProcessed;
      if (res != SZ_OK)
        break;
      if (state.dicPos == state.dicBufSize || (inProcessed == 0 && dicPos == state.dicPos))
      {
        if (state.dicBufSize != outSize || lookahead != 0 ||
            (status != LZMA_STATUS_FINISHED_WITH_MARK &&
             status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
          res = SZ_ERROR_DATA;
        break;
      }
      res = inStream->Skip((void *)inStream, inProcessed);
      if (res != SZ_OK)
        break;
    }
  }

  LzmaDec_FreeProbs(&state, allocMain);
  return res;
}



// ===================================== defines and macroses ========================================

#define IN_BUF_SIZE     (1 << 19)
#define OUT_BUF_SIZE    (1 << 20)
#define COPY_BUF_SIZE   (1 << 21)       // lzma_copy method


#define DECODING        0
#define ENCODING        1

#define ALLOCATE_BUF(buf, size)          {                                                      \
                                             buf = (Byte *)IAlloc_Alloc(allocMain, size);        \
                                             if (buf == NULL)  return SZ_ERROR_MEM;              \
                                         }                                                       

#define ALLOCATE_BUFS(in_buf, in_size, out_buf, out_size)   ALLOCATE_BUF(in_buf, in_size);              \
                                                            ALLOCATE_BUF(out_buf, out_size);

#define FREE_BUF(buf)                    IAlloc_Free(allocMain, buf);             \
                                         buf = NULL;        

#define FREE_BUFS(in_buf, out_buf)      FREE_BUF(in_buf);           \
                                        FREE_BUF(out_buf);

// ===================================================================================================
#define RETAIN_BUF_SIZE            4     //  4 is LookAhead in x86_Convert(0
#define BCJ_state_init(state)           {                                                                                       \
                                            state.ip = 0;                                                                       \
                                            state.x86_state = 0;                                                                \
                                            state.retain_buf = (Byte *)IAlloc_Alloc(allocMain, RETAIN_BUF_SIZE);            \
                                            state.retain_buf_size = 0;                                                          \
                                            state.FirstBuffer = True;                                                           \
                                        }
#define BCJ_state_free(state)           FREE_BUF(state.retain_buf);

//#define BCJ_free_state(state)           IAlloc_Free(allocMain, state.decode_buf);  \
//                                        state.decode_buf = NULL;


struct BCJ_state
{
    UInt32 ip;
    UInt32 x86_state;
    Byte *retain_buf;
    UInt32 retain_buf_size;
    Bool FirstBuffer;
};

static SizeT DecodeBCJ(Byte *data, SizeT *size, BCJ_state *st, Bool last_time)
{
    SizeT offset;
    SizeT processed = 0, retain_bytes;
    if (st->retain_buf_size)
        memcpy(data, st->retain_buf, st->retain_buf_size);        // copy 4 bytes in the beginning of buffer
    *size += st->retain_buf_size;
    offset = (st->FirstBuffer) ? RETAIN_BUF_SIZE : 0;
    processed = x86_Convert(data + offset, *size, st->ip, &st->x86_state, DECODING);
    st->ip += processed;
    retain_bytes = *size - processed;
    if (retain_bytes > RETAIN_BUF_SIZE)
    {
        printf("emm.. tut kak by bufera ne hvataet, naprimer...\n");
        return SZ_ERROR_MEM;
    }
    memcpy(st->retain_buf, (data + offset) + processed, retain_bytes);
    st->retain_buf_size = retain_bytes;
    if (last_time)
        processed += retain_bytes;
    *size = processed;
    return processed;
}

static SRes SzDecodeLzmaToFileWithBuf(const UInt32 folderIndex, CSzCoderInfo *coder, const CSzArEx *db, 
                                      ILookInStream *inStream, IFileStream  *IFile, SizeT outSize, 
                                      ISzAlloc *allocMain, Bool filterPresent)
{
    Byte *myInBufBitch = NULL;
    Byte *myOutBufBitch = NULL;
    SRes res = 0;
    CLzmaDec state;
    wr_st_t st;
    
    size_t in_buf_size = 0, in_offset = 0;
    size_t out_size = 0;
    size_t bytes_read, bytes_left;
    Bool StopDecoding = False;
    SizeT out_buf_size = OUT_BUF_SIZE;
    Bool to_read = True;
    write_state_init(st);
    LzmaDec_Construct(&state);
    LzmaDec_Allocate(&state, coder->Props.data,coder->Props.size, allocMain);
    LzmaDec_Init(&state);
    
    if (myInBufBitch == NULL)
        ALLOCATE_BUFS(myInBufBitch, IN_BUF_SIZE, myOutBufBitch, OUT_BUF_SIZE);


    while(1)                                    // decompressing cycle 
    {
        ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
        ELzmaStatus status;
        
        if (to_read)                            // if more than 1 folder in db, a part of the next folder stream can be read ->
        {                                       // it's not a problem, because before unpacking new db folder it first seeks to right position
            bytes_read = IN_BUF_SIZE;   
            RINOK(inStream->Read(inStream, myInBufBitch, &bytes_read));
            in_buf_size = bytes_left = bytes_read;
        }
        if (outSize - out_size < OUT_BUF_SIZE)
        {
            out_buf_size = outSize - out_size;
            finishMode = LZMA_FINISH_END;
            
        }
        res = LzmaDec_DecodeToBuf(&state, myOutBufBitch, &out_buf_size, myInBufBitch + in_offset, &in_buf_size, finishMode, &status);
        if (in_buf_size == 0 || res != SZ_OK)
        {
            StopDecoding = True;
            printf("something bad happened =( \n");
            return SZ_ERROR_FAIL;
        }
        out_size += out_buf_size;
        bytes_left -= in_buf_size;

        if (in_buf_size < bytes_read && in_buf_size)           // not whole in_buf was decompressed
        {
            to_read = False;
            in_offset += in_buf_size;
            in_buf_size = bytes_left;
        }
        
        StopDecoding = (out_size >= outSize)? True : False;
        if (bytes_left == 0 || out_buf_size == OUT_BUF_SIZE || StopDecoding)   // whole in_buf was decompressed
        {
            if (filterPresent)
                WriteTempStream(IFile, myOutBufBitch, out_buf_size, StopDecoding, &st);
            else
                WriteStream(IFile, folderIndex, db, myOutBufBitch, out_buf_size, &st);

            if (bytes_left == 0)
            {
                to_read = True;
                in_offset = 0;
            }
            out_buf_size = OUT_BUF_SIZE;
            if (StopDecoding)
                break;
        }   
        
     }
    printf("I'm ALIVE! =) I unpacked %d(from %d) bytes and %d bytes left in input buffer\n", out_size, outSize, bytes_left);

    FREE_BUFS(myInBufBitch, myOutBufBitch);
    LzmaDec_Free(&state, allocMain);
    return SZ_OK;
}

static SRes SzDecodeLzma2ToFileWithBuf(const UInt32 folderIndex, CSzCoderInfo *coder, const CSzArEx *db, 
                                       ILookInStream *inStream, IFileStream  *IFile, SizeT outSize, 
                                      ISzAlloc *allocMain, Bool filterPresent)
{
    Byte *myInBufBitch = NULL;
    Byte *myOutBufBitch = NULL;

    CLzma2Dec state;
    Lzma2Dec_Construct(&state);
    Lzma2Dec_Allocate(&state, coder->Props.data[0], allocMain);
    Lzma2Dec_Init(&state);

    if (myInBufBitch == NULL)
        ALLOCATE_BUFS(myInBufBitch, IN_BUF_SIZE, myOutBufBitch, OUT_BUF_SIZE);

    wr_st_t st;
    write_state_init(st);
    size_t in_buf_size = 0, in_offset = 0;
    size_t out_size = 0;
    size_t bytes_read, bytes_left;
    Bool StopDecoding = False;
    SizeT out_buf_size = OUT_BUF_SIZE;
    Bool to_read = True;
    while(1)                                    // decompressing cycle 
    {
        ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
        ELzmaStatus status;

        if (to_read)                            // if more than 1 folder in db, part of next folder stream can be read ->
        {                                       // it's not a problem, because before unpacking new db folder it seeks to right position
            bytes_read = IN_BUF_SIZE;   
            RINOK(inStream->Read(inStream, myInBufBitch, &bytes_read));
            in_buf_size = bytes_left = bytes_read;
        }
        if ((outSize - out_size < OUT_BUF_SIZE) && outSize)
        {
            out_buf_size = outSize - out_size;
            finishMode = LZMA_FINISH_END;

        }
        Lzma2Dec_DecodeToBuf(&state, myOutBufBitch, &out_buf_size, myInBufBitch + in_offset, &in_buf_size, finishMode, &status);
        if (in_buf_size == 0)
        {
            StopDecoding = True;
            printf("lzma2 decode: something bad happened =( \n");
            return SZ_ERROR_FAIL;
        }
        out_size += out_buf_size;
        bytes_left -= in_buf_size;

        if (in_buf_size < bytes_read && in_buf_size)           // not whole in_buf was decompressed
        {
            to_read = False;
            in_offset += in_buf_size;
            in_buf_size = bytes_left;
        }

        StopDecoding = (out_size >= outSize)? True : False;
        if (bytes_left == 0 || out_buf_size == OUT_BUF_SIZE || StopDecoding)   // whole in_buf was decompressed
        {
            if (filterPresent)
                WriteTempStream(IFile, myOutBufBitch, out_buf_size, StopDecoding, &st);
            else
                WriteStream(IFile, folderIndex, db, myOutBufBitch, out_buf_size, &st);
            if (bytes_left == 0)
            {
                to_read = True;
                in_offset = 0;
            }
            out_buf_size = OUT_BUF_SIZE;
            if (StopDecoding)
                break;
        }   
    }
    printf("lzma2: I'm ALIVE! =) I unpacked %d(from %d) bytes and %d bytes left in input buffer\n", out_size, outSize, bytes_left);
    FREE_BUFS(myInBufBitch, myOutBufBitch);
    Lzma2Dec_Free(&state, allocMain);
    return SZ_OK;
}

static SRes SzDecodeCopyToFileWithBuf(const UInt32 folderIndex, const CSzArEx *db, ILookInStream *inStream, 
                                      IFileStream  *IFile, SizeT outSize, ISzAlloc *allocMain)
{
    if (outSize <= 0 || !inStream )
        return SZ_ERROR_FAIL;
    Byte *buf;
    ALLOCATE_BUF(buf, COPY_BUF_SIZE);
    SizeT out_size = 0, bytes_read = 0;
    wr_st_t st;
    write_state_init(st);

    while (out_size < outSize)
    {
        SizeT rem = outSize - out_size;
        bytes_read = (rem < COPY_BUF_SIZE) ? rem : COPY_BUF_SIZE;
        RINOK(inStream->Read(inStream, buf, &bytes_read));
        RINOK(WriteStream(IFile, folderIndex, db, buf, bytes_read, &st));
        out_size += bytes_read;
    }
    printf("I'm ALIVE! =) I unpacked %d(from %d) bytes\n", out_size, outSize);
    FREE_BUF(buf);
    return SZ_OK;
}


static SRes SzDecodeLzma2(CSzCoderInfo *coder, UInt64 inSize, ILookInStream *inStream,
    Byte *outBuffer, SizeT outSize, ISzAlloc *allocMain)
{
  CLzma2Dec state;
  SRes res = SZ_OK;

  Lzma2Dec_Construct(&state);
  if (coder->Props.size != 1)
    return SZ_ERROR_DATA;
  RINOK(Lzma2Dec_AllocateProbs(&state, coder->Props.data[0], allocMain));
  state.decoder.dic = outBuffer;
  state.decoder.dicBufSize = outSize;
  Lzma2Dec_Init(&state);

  for (;;)
  {
    Byte *inBuf = NULL;
    size_t lookahead = (1 << 18);
    if (lookahead > inSize)         // inSize = packed size 1 322 532
      lookahead = (size_t)inSize;
    res = inStream->Look((void *)inStream, (const void **)&inBuf, &lookahead);      // lookahead = 16384
    if (res != SZ_OK)
      break;

    {
      SizeT inProcessed = (SizeT)lookahead, dicPos = state.decoder.dicPos;
      ELzmaStatus status;
      res = Lzma2Dec_DecodeToDic(&state, outSize, inBuf, &inProcessed, LZMA_FINISH_END, &status);
      lookahead -= inProcessed;
      inSize -= inProcessed;
      if (res != SZ_OK)
        break;
      if (state.decoder.dicPos == state.decoder.dicBufSize || (inProcessed == 0 && dicPos == state.decoder.dicPos))
      {
        if (state.decoder.dicBufSize != outSize || lookahead != 0 ||
            (status != LZMA_STATUS_FINISHED_WITH_MARK))
          res = SZ_ERROR_DATA;
        break;
      }
      res = inStream->Skip((void *)inStream, inProcessed);
      if (res != SZ_OK)
        break;
    }
  }

  Lzma2Dec_FreeProbs(&state, allocMain);
  return res;
}

static SRes SzDecodeCopy(UInt64 inSize, ILookInStream *inStream, Byte *outBuffer)
{
  while (inSize > 0)
  {
    void *inBuf;
    size_t curSize = (1 << 18);
    if (curSize > inSize)
      curSize = (size_t)inSize;
    RINOK(inStream->Look((void *)inStream, (const void **)&inBuf, &curSize));
    if (curSize == 0)
      return SZ_ERROR_INPUT_EOF;
    memcpy(outBuffer, inBuf, curSize);
    outBuffer += curSize;
    inSize -= curSize;
    RINOK(inStream->Skip((void *)inStream, curSize));
  }
  return SZ_OK;
}



static Bool IS_MAIN_METHOD(UInt32 m)
{
  switch(m)
  {
    case k_Copy:
    case k_LZMA:
    case k_LZMA2:
    #ifdef _7ZIP_PPMD_SUPPPORT
    case k_PPMD:
    #endif
      return True;
  }
  return False;
}

static Bool IS_SUPPORTED_CODER(const CSzCoderInfo *c)
{
  return
      c->NumInStreams == 1 &&
      c->NumOutStreams == 1 &&
      c->MethodID <= (UInt32)0xFFFFFFFF &&
      IS_MAIN_METHOD((UInt32)c->MethodID);
}

#define IS_BCJ2(c) ((c)->MethodID == k_BCJ2 && (c)->NumInStreams == 4 && (c)->NumOutStreams == 1)

static SRes CheckSupportedFolder(const CSzFolder *f)
{
  if (f->NumCoders < 1 || f->NumCoders > 4)
    return SZ_ERROR_UNSUPPORTED;
  if (!IS_SUPPORTED_CODER(&f->Coders[0]))
    return SZ_ERROR_UNSUPPORTED;
  if (f->NumCoders == 1)
  {
    if (f->NumPackStreams != 1 || f->PackStreams[0] != 0 || f->NumBindPairs != 0)
      return SZ_ERROR_UNSUPPORTED;
    return SZ_OK;
  }
  if (f->NumCoders == 2)
  {
    CSzCoderInfo *c = &f->Coders[1];
    if (c->MethodID > (UInt32)0xFFFFFFFF ||
        c->NumInStreams != 1 ||
        c->NumOutStreams != 1 ||
        f->NumPackStreams != 1 ||
        f->PackStreams[0] != 0 ||
        f->NumBindPairs != 1 ||
        f->BindPairs[0].InIndex != 1 ||
        f->BindPairs[0].OutIndex != 0)
      return SZ_ERROR_UNSUPPORTED;
    switch ((UInt32)c->MethodID)
    {
      case k_BCJ:
      case k_ARM:
        break;
      default:
        return SZ_ERROR_UNSUPPORTED;
    }
    return SZ_OK;
  }
  if (f->NumCoders == 4)
  {
    if (!IS_SUPPORTED_CODER(&f->Coders[1]) ||
        !IS_SUPPORTED_CODER(&f->Coders[2]) ||
        !IS_BCJ2(&f->Coders[3]))
      return SZ_ERROR_UNSUPPORTED;
    if (f->NumPackStreams != 4 ||
        f->PackStreams[0] != 2 ||
        f->PackStreams[1] != 6 ||
        f->PackStreams[2] != 1 ||
        f->PackStreams[3] != 0 ||
        f->NumBindPairs != 3 ||
        f->BindPairs[0].InIndex != 5 || f->BindPairs[0].OutIndex != 0 ||
        f->BindPairs[1].InIndex != 4 || f->BindPairs[1].OutIndex != 1 ||
        f->BindPairs[2].InIndex != 3 || f->BindPairs[2].OutIndex != 2)
      return SZ_ERROR_UNSUPPORTED;
    return SZ_OK;
  }
  return SZ_ERROR_UNSUPPORTED;
}

static UInt64 GetSum(const UInt64 *values, UInt32 index)
{
  UInt64 sum = 0;
  UInt32 i;
  for (i = 0; i < index; i++)
    sum += values[i];
  return sum;
}

#define CASE_BRA_CONV(isa) case k_ ## isa: isa ## _Convert(outBuffer, outSize, 0, 0); break;

static SRes SzFolder_Decode2(const CSzFolder *folder, const UInt64 *packSizes,
    ILookInStream *inStream, UInt64 startPos,
    Byte *outBuffer, SizeT outSize, ISzAlloc *allocMain,
    Byte *tempBuf[])
{
  UInt32 ci;
  SizeT tempSizes[3] = { 0, 0, 0};
  SizeT tempSize3 = 0;
  Byte *tempBuf3 = 0;

  RINOK(CheckSupportedFolder(folder));

  for (ci = 0; ci < folder->NumCoders; ci++)
  {
    CSzCoderInfo *coder = &folder->Coders[ci];

    if (IS_MAIN_METHOD((UInt32)coder->MethodID))
    {
      UInt32 si = 0;
      UInt64 offset;
      UInt64 inSize;
      Byte *outBufCur = outBuffer;
      SizeT outSizeCur = outSize;
      if (folder->NumCoders == 4)
      {
        UInt32 indices[] = { 3, 2, 0 };
        UInt64 unpackSize = folder->UnpackSizes[ci];
        si = indices[ci];
        if (ci < 2)
        {
          Byte *temp;
          outSizeCur = (SizeT)unpackSize;
          if (outSizeCur != unpackSize)
            return SZ_ERROR_MEM;
          temp = (Byte *)IAlloc_Alloc(allocMain, outSizeCur);
          if (temp == 0 && outSizeCur != 0)
            return SZ_ERROR_MEM;
          outBufCur = tempBuf[1 - ci] = temp;
          tempSizes[1 - ci] = outSizeCur;
        }
        else if (ci == 2)
        {
          if (unpackSize > outSize) /* check it */
            return SZ_ERROR_PARAM;
          tempBuf3 = outBufCur = outBuffer + (outSize - (size_t)unpackSize);
          tempSize3 = outSizeCur = (SizeT)unpackSize;
        }
        else
          return SZ_ERROR_UNSUPPORTED;
      }
      offset = GetSum(packSizes, si);
      inSize = packSizes[si];
      RINOK(LookInStream_SeekTo(inStream, startPos + offset));

      if (coder->MethodID == k_Copy)
      {
        if (inSize != outSizeCur) /* check it */
          return SZ_ERROR_DATA;
        RINOK(SzDecodeCopy(inSize, inStream, outBufCur));
      }
      else if (coder->MethodID == k_LZMA)
      {
        RINOK(SzDecodeLzma(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
      }
      else if (coder->MethodID == k_LZMA2)
      {
        RINOK(SzDecodeLzma2(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
      }
      else
      {
        #ifdef _7ZIP_PPMD_SUPPPORT
        RINOK(SzDecodePpmd(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
        #else
        return SZ_ERROR_UNSUPPORTED;
        #endif
      }
    }
    else if (coder->MethodID == k_BCJ2)
    {
      UInt64 offset = GetSum(packSizes, 1);
      UInt64 s3Size = packSizes[1];
      SRes res;
      if (ci != 3)
        return SZ_ERROR_UNSUPPORTED;
      RINOK(LookInStream_SeekTo(inStream, startPos + offset));
      tempSizes[2] = (SizeT)s3Size;
      if (tempSizes[2] != s3Size)
        return SZ_ERROR_MEM;
      tempBuf[2] = (Byte *)IAlloc_Alloc(allocMain, tempSizes[2]);
      if (tempBuf[2] == 0 && tempSizes[2] != 0)
        return SZ_ERROR_MEM;
      res = SzDecodeCopy(s3Size, inStream, tempBuf[2]);
      RINOK(res)

      res = Bcj2_Decode(
          tempBuf3, tempSize3,
          tempBuf[0], tempSizes[0],
          tempBuf[1], tempSizes[1],
          tempBuf[2], tempSizes[2],
          outBuffer, outSize);
      RINOK(res)
    }
    else
    {
      if (ci != 1)
        return SZ_ERROR_UNSUPPORTED;
      switch(coder->MethodID)
      {
        case k_BCJ:
        {
          UInt32 state;
          x86_Convert_Init(state);
          x86_Convert(outBuffer, outSize, 0, &state, 0);
          break;
        }
        CASE_BRA_CONV(ARM)
        default:
          return SZ_ERROR_UNSUPPORTED;
      }
    }
  }
  return SZ_OK;
}


SRes SzFolder_Decode(const CSzFolder *folder, const UInt64 *packSizes,
    ILookInStream *inStream, UInt64 startPos,
    Byte *outBuffer, size_t outSize, ISzAlloc *allocMain)
{
  Byte *tempBuf[3] = { 0, 0, 0};
  int i;
  SRes res = SzFolder_Decode2(folder, packSizes, inStream, startPos,
      outBuffer, (SizeT)outSize, allocMain, tempBuf);
  for (i = 0; i < 3; i++)
    IAlloc_Free(allocMain, tempBuf[i]);
  return res;
}



static Bool DetectFilter(const CSzFolder *folder)
{
    if (folder->NumCoders == 2 && folder->Coders[1].MethodID == k_BCJ )
        return True;
    else if (folder->NumCoders == 4 && folder->Coders[3].MethodID == k_BCJ2)
        return True;
    else
        return 0;
}
static SRes ApplyBCJ(IFileStream  *IFile, SizeT total_unpack_size, const UInt32 folderIndex, const CSzArEx *db, ISzAlloc *allocMain)
{
    Byte *decodeBuf = NULL;
    SizeT myOutBufSize = OUT_BUF_SIZE + RETAIN_BUF_SIZE;
    wr_st_t wr_st;
    write_state_init(wr_st);
    r_st_t r_st;
    read_state_init(r_st);
    BCJ_state bcj1_st;
    BCJ_state_init(bcj1_st);
    if (decodeBuf == NULL)
        ALLOCATE_BUF(decodeBuf, myOutBufSize);

    while (total_unpack_size)
    {
        SizeT processed = 0, bytes_read = OUT_BUF_SIZE;
        SizeT retain_offset = 0;
        Bool LastBuf = False;
        RINOK(ReadTempStream(IFile, decodeBuf + RETAIN_BUF_SIZE, &bytes_read, &r_st));
        if (bytes_read < IN_BUF_SIZE)
            LastBuf = True;
        processed = DecodeBCJ(decodeBuf, &bytes_read, &bcj1_st, LastBuf);

        if (processed == 0)
            break;
        if (bcj1_st.FirstBuffer)               // first out buffer should strart from offset RETAIN_BUF_SIZE
        {
            retain_offset = RETAIN_BUF_SIZE;
            bcj1_st.FirstBuffer = False;
        }
        RINOK(WriteStream(IFile, folderIndex, db, decodeBuf + retain_offset, processed, &wr_st));
        total_unpack_size -= processed;
    }

    FREE_BUF(decodeBuf);
    BCJ_state_free(bcj1_st);
    return SZ_OK;
}


static SRes ApplyBCJ2(IFileStream  *IFile, SizeT total_out_size, const UInt32 folderIndex, 
                      const CSzArEx *db, ISzAlloc *allocMain, Byte *tempBuf[], SizeT tempSizes[])
{
    Byte *myInBufBitch = NULL, *myOutBufBitch = NULL;
    wr_st_t wr_st;
    write_state_init(wr_st);
    r_st_t r_st;
    read_state_init(r_st);
    Bcj2_dec_state bcj2_st;
    Bcj2_dec_state_init(bcj2_st);
    bcj2_st.buf1 = tempBuf[0];
    bcj2_st.buf2 = tempBuf[1];
    bcj2_st.buf3 = tempBuf[2];
    if (myInBufBitch == NULL)
        ALLOCATE_BUFS(myInBufBitch, IN_BUF_SIZE, myOutBufBitch, OUT_BUF_SIZE);

    while (total_out_size)
    {
        SizeT myOutBufSize = OUT_BUF_SIZE;
        SizeT processed = 0, bytes_read = IN_BUF_SIZE;
        RINOK(ReadTempStream(IFile, myInBufBitch, &bytes_read, &r_st));

        processed = Bcj2_DecodeToFileWithBufs(
            myInBufBitch, bytes_read,
            bcj2_st.buf1, &tempSizes[0],
            bcj2_st.buf2, &tempSizes[1],
            bcj2_st.buf3, &tempSizes[2],
            myOutBufBitch, myOutBufSize,
            &bcj2_st);
        if (processed == 0)
            break;
        RINOK(WriteStream(IFile, folderIndex, db, myOutBufBitch, processed, &wr_st));
        total_out_size -= processed;
    }

    FREE_BUFS(myInBufBitch, myOutBufBitch);
    return SZ_OK;
}
static SRes SzFolder_Decode2ToFile(const CSzFolder *folder, const UInt32 folderIndex, const UInt64 *packSizes,
                             ILookInStream *inStream, IFileStream  *IFile, const CSzArEx *db, UInt64 startPos,
                             SizeT outSize, ISzAlloc *allocMain, Byte *tempBuf[])
{
    UInt32 ci;
    SizeT total_out_size = outSize;
    SizeT tempSizes[3] = { 0, 0, 0};
    SizeT outSizeCur = outSize;
    //SizeT tempSize3 = 0;
    //Byte *tempBuf3 = 0;

    RINOK(CheckSupportedFolder(folder));

    for (ci = 0; ci < folder->NumCoders; ci++)
    {
        CSzCoderInfo *coder = &folder->Coders[ci];
        const Bool FilterPresent = DetectFilter(folder);
        if (IS_MAIN_METHOD((UInt32)coder->MethodID))
        {
            UInt32 si = 0;
            UInt64 offset;
            UInt64 inSize;
            Byte *outBufCur = NULL;
            
            Bool unpackInMem = False;                               // for small buffers within unpacking BCJ2 we can use 
                                                                    // unpacking in memory
            if (folder->NumCoders == 4)
            {
                UInt32 indices[] = { 3, 2, 0 };
                UInt64 unpackSize = folder->UnpackSizes[ci];        // 4176, 9k, 10.610.492,
                si = indices[ci];                                   // 3, 2, 0
                if (ci < 2)
                {
                    Byte *temp;
                    unpackInMem = True;
                    outSizeCur = (SizeT)unpackSize;                   // 4176, 9k,
                    if (outSizeCur != unpackSize)
                        return SZ_ERROR_MEM;
                    temp = (Byte *)IAlloc_Alloc(allocMain, outSizeCur);
                    if (temp == 0 && outSizeCur != 0)
                        return SZ_ERROR_MEM;
                    outBufCur = tempBuf[1 - ci] = temp;
                    tempSizes[1 - ci] = outSizeCur;
                }
                else if (ci == 2)
                {
                    if (unpackSize > outSize) /* check it */
                        return SZ_ERROR_PARAM;
                    //tempBuf3 = outBufCur = outBuffer + (outSize - (size_t)unpackSize);
                    outSizeCur = (SizeT)unpackSize;
                }
                else
                    return SZ_ERROR_UNSUPPORTED;
            }

            offset = GetSum(packSizes, si);
            inSize = packSizes[si];
            RINOK(LookInStream_SeekTo(inStream, startPos + offset));

            if (coder->MethodID == k_Copy)
            {
                if (unpackInMem) 
                {
                    RINOK(SzDecodeCopy(inSize, inStream, outBufCur));
                }
                else
                {
                    RINOK(SzDecodeCopyToFileWithBuf(folderIndex, db, inStream, IFile, outSizeCur, allocMain));
                }
            }
            else if (coder->MethodID == k_LZMA)
            {
                if (unpackInMem)
                {
                    RINOK(SzDecodeLzma(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
                }
                else
                {
                    RINOK(SzDecodeLzmaToFileWithBuf(folderIndex, coder, db, inStream, IFile, outSizeCur, allocMain, FilterPresent));
                }
            }
            else if (coder->MethodID == k_LZMA2)
            {
                if (unpackInMem)
                {
                    RINOK(SzDecodeLzma2(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
                }
                else
                {
                    RINOK(SzDecodeLzma2ToFileWithBuf(folderIndex, coder, db, inStream, IFile, outSizeCur, allocMain, FilterPresent)); 
                }
            }
            else
            {
#ifdef _7ZIP_PPMD_SUPPPORT
                RINOK(SzDecodePpmd(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
#else
                return SZ_ERROR_UNSUPPORTED;
#endif
            }
        }
        else if (coder->MethodID == k_BCJ2)
        {
            printf("BCJ2 filter in coder # %d\n", ci+1);
            UInt64 offset = GetSum(packSizes, 1);
            UInt64 s3Size = packSizes[1];
            SRes res;
            if (ci != 3)
                return SZ_ERROR_UNSUPPORTED;
            RINOK(LookInStream_SeekTo(inStream, startPos + offset));
            tempSizes[2] = (SizeT)s3Size;
            if (tempSizes[2] != s3Size)
                return SZ_ERROR_MEM;
            tempBuf[2] = (Byte *)IAlloc_Alloc(allocMain, tempSizes[2]);
            if (tempBuf[2] == 0 && tempSizes[2] != 0)
                return SZ_ERROR_MEM;
            res = SzDecodeCopy(s3Size, inStream, tempBuf[2]);
            RINOK(res)
            res = ApplyBCJ2(IFile, total_out_size, folderIndex, db, allocMain, tempBuf, tempSizes);
            RINOK(res)
        }
        else    // BCJ
        {
            printf("BCJ filter in coder # %d\n", ci+1);
            if (ci != 1)
                return SZ_ERROR_UNSUPPORTED;
            ApplyBCJ(IFile, outSizeCur, folderIndex, db, allocMain);
        }
    }
    return SZ_OK;
}


SRes SzFolder_DecodeToFile(const CSzFolder *folder, const UInt32 folderIndex, const UInt64 *packSizes,
                           ILookInStream *inStream, IFileStream  *IFile, const CSzArEx *db, UInt64 startPos,
                           size_t outSize, ISzAlloc *allocMain)
{
    Byte *tempBuf[3] = { 0, 0, 0};
    int i;
    SRes res = SzFolder_Decode2ToFile(folder, folderIndex, packSizes, inStream, IFile, 
                      db, startPos,(SizeT)outSize, allocMain, tempBuf);
    for (i = 0; i < 3; i++)
        IAlloc_Free(allocMain, tempBuf[i]);
    return res;
}