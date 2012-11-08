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


static SRes SzDecodeLzmaToFile(CSzCoderInfo *coder, const CSzArEx *db, ILookInStream *inStream, SizeT outSize, ISzAlloc *allocMain)
{
    CLzmaDec state;
    SRes res = SZ_OK;
    Byte *myOwnBufBitch = NULL;
    size_t myOwnBufSize = 0;
    UInt64 _outSizeProcessed = 0, _inSizeProcessed = 0;
    UInt64 _inPos = 0;
    size_t _inSize = 0;
    size_t writtenBytes = 0;

    LzmaDec_Construct(&state);
    RINOK(LzmaDec_AllocateProbs(&state, coder->Props.data, (unsigned)coder->Props.size, allocMain));
    LzmaDec_Init(&state);
    
    if (myOwnBufBitch == NULL)
    { 
        myOwnBufSize = state.prop.dicSize;
        myOwnBufBitch = (Byte *)IAlloc_Alloc(allocMain, myOwnBufSize);
        if (myOwnBufBitch == NULL)
            return SZ_ERROR_MEM;
        state.dic = myOwnBufBitch;
        state.dicBufSize = myOwnBufSize;
    }

    //SizeT next = (state.dicBufSize - state.dicPos < myOwnBufSize) ? state.dicBufSize : (state.dicPos + myOwnBufSize); 
    for (;;)
    {
        Byte *inBuf = NULL;
        //size_t lookahead = (1 << 18);
        //if (lookahead > inSize)
        //    lookahead = (size_t)inSize;
        if (_inPos == _inSize)
        {
            _inPos = _inSize = 0;
            //RINOK(inStream->Read(_inBuf, kInBufSize, &_inSize));      // _inSize = 1 048 576
            _inSize = LookToRead_BUF_SIZE;
            RINOK(inStream->Look((void *)inStream, (const void **)&inBuf, &_inSize));
        }
        if (res != SZ_OK || _inSize == 0)
            break;

        SizeT dicPos = state.dicPos;
        SizeT curSize = state.dicBufSize - dicPos;  
        const UInt32 kStepSize = ((UInt32)1 << 22);
        if (curSize > kStepSize)
            curSize = (SizeT)kStepSize;
        
        ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
        const UInt64 rem = outSize - _outSizeProcessed;      // _outSize = 6 086 757 (UnpackedSize)
        if (rem < curSize)
        {
            curSize = (SizeT)rem;
            /*
            // finishMode = LZMA_FINISH_END;
            we can't use LZMA_FINISH_END here to allow partial decoding
            */
        }

        SizeT inSizeProcessed = _inSize - _inPos;
        ELzmaStatus status;
        res = LzmaDec_DecodeToDic(&state, dicPos + curSize, inBuf, &inSizeProcessed, finishMode, &status);
        _inPos += (UInt32)inSizeProcessed;
        _inSizeProcessed += inSizeProcessed;
        SizeT outSizeProcessed = state.dicPos - dicPos;
        _outSizeProcessed += outSizeProcessed;

        bool finished = (inSizeProcessed == 0 && outSizeProcessed == 0);
        bool stopDecoding = (_outSizeProcessed >= outSize);

        if (res != 0 || state.dicPos == state.dicBufSize || finished || stopDecoding)
        {
            //HRESULT res2 = WriteStream(db, state.dic, state.dicPos, &writtenBytes);           // made new functions
            //if (res != 0)
            //    return S_FALSE;
            //RINOK(res2);
            //if (stopDecoding)
            //    return S_OK;
            //if (finished)
            //    return (status == LZMA_STATUS_FINISHED_WITH_MARK ? S_OK : S_FALSE);
        }
        if (state.dicPos == state.dicBufSize)
        {
            state.dicPos = 0;
            state.dic = myOwnBufBitch;
        }

        res = inStream->Skip((void *)inStream, inSizeProcessed);
        if (res != SZ_OK)
            break;
    }
    IAlloc_Free(allocMain, myOwnBufBitch);
    LzmaDec_FreeProbs(&state, allocMain);
    return res;
}







// ===================================== defines and macroses ========================================

#define IN_BUF_SIZE     (1 << 19)
#define OUT_BUF_SIZE    (1 << 20)
#define COPY_BUF_SIZE   (1 << 21)       // lzma_copy method


#define DECODING        0
#define ENCODING        1

#define ALLOCATE_BUFS(in_buf, out_buf)   {                                                              \
                                            out_buf = (Byte *)IAlloc_Alloc(allocMain, OUT_BUF_SIZE);    \
                                            in_buf = (Byte *)IAlloc_Alloc(allocMain, IN_BUF_SIZE);      \
                                            if (in_buf == NULL || out_buf == NULL)                      \
                                                return SZ_ERROR_MEM;                                    \
                                         }                                                              

#define FREE_BUFS(in_buf, out_buf)      {                                         \
                                            IAlloc_Free(allocMain, in_buf);       \
                                            in_buf = NULL;                        \
                                            IAlloc_Free(allocMain, out_buf);      \
                                            out_buf = NULL;                       \
                                        }                                         

// ===================================================================================================
#define BCJ_BUF_SIZE    5             // 1(x86 alignment) + 4(LookAhead??)
#define BCJ_state_init(state)           {                                                                                       \
                                            state.ip = 0;                                                                       \
                                            state.x86_state = 0;                                                                \
                                            state.decode_buf = (Byte *)IAlloc_Alloc(allocMain, OUT_BUF_SIZE + BCJ_BUF_SIZE);    \
                                            state.buf_size = 0;                                                      \
                                        }

#define BCJ_free_state(state)           IAlloc_Free(allocMain, state.decode_buf);  \
                                        state.decode_buf = NULL;


struct BCJ_state
{
    UInt64 ip;
    UInt32 x86_state;
    char buf[BCJ_BUF_SIZE];
    UInt32 buf_size;
    Byte *decode_buf;
};

static SizeT ApplyFilter(Byte *data, SizeT *size, const UInt32 filter_type, BCJ_state *st, bool last_time)
{
    if ( filter_type == k_BCJ)
    {
        UInt32 state = 0;
        UInt32 processed = 0, retain_bytes;
        if (st->buf_size)
            memcpy(st->decode_buf, st->buf, st->buf_size);
        memcpy(st->decode_buf + st->buf_size, data, *size);
        UInt32 decode_buf_size = *size + st->buf_size;
        processed = x86_Convert(st->decode_buf, decode_buf_size, st->ip, &st->x86_state, DECODING);
        st->ip += processed;
        retain_bytes = decode_buf_size - processed;
        if (retain_bytes > BCJ_BUF_SIZE)
        {
            printf("emm.. tut kak by bufera ne hvataet, naprimer...\n");
            return SZ_ERROR_MEM;
        }
        memcpy(st->buf, st->decode_buf + processed, retain_bytes);
        st->buf_size = retain_bytes;
        if (last_time)
            processed += retain_bytes;
        memcpy(data, st->decode_buf, processed);
        *size = processed;

        return processed;
    }

    return 0;
}

static SRes SzDecodeLzmaToFileWithBuf(const UInt32 folderIndex, CSzCoderInfo *coder, const CSzArEx *db, ILookInStream *inStream, SizeT outSize, 
                                      ISzAlloc *allocMain, const UInt32 FILTER_TYPE)
{
    Byte *myInBufBitch = NULL;
    Byte *myOutBufBitch = NULL;
    SRes res = 0;
    CLzmaDec state;
    LzmaDec_Construct(&state);
    LzmaDec_Allocate(&state, coder->Props.data,coder->Props.size, allocMain);
    LzmaDec_Init(&state);
    
    if (myInBufBitch == NULL)
        ALLOCATE_BUFS(myInBufBitch, myOutBufBitch);

    wr_st_t st;
    write_state_init(st);
    BCJ_state bcj_st;
    BCJ_state_init(bcj_st);
    size_t in_buf_size = 0, in_offset = 0;
    size_t out_size = 0;
    size_t bytes_read, bytes_left;
    bool StopDecoding = false, to_flush = false;
    SizeT out_buf_size = OUT_BUF_SIZE;
    bool to_read = true;
    while(1)                                    // decompressing cycle 
    {
        ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
        ELzmaStatus status;
        
        if (to_read)                            // if more than 1 folder in db, it can be read part of next folder stream ->
        {                                       // it's not a problem, because before unpacking new db folder it seeks to right position
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
            StopDecoding = true;
            printf("something bad happened =( \n");
            return SZ_ERROR_FAIL;
        }
        out_size += out_buf_size;
        bytes_left -= in_buf_size;

        if (in_buf_size < bytes_read && in_buf_size)           // not whole in_buf was decompressed
        {
            to_read = false;
            in_offset += in_buf_size;
            in_buf_size = bytes_left;
        }
        
        StopDecoding = (out_size >= outSize)? true : false;
        if (bytes_left == 0 || out_buf_size == OUT_BUF_SIZE || StopDecoding)   // whole in_buf was decompressed
        {
            bool last_time = StopDecoding ? true : false;
            ApplyFilter(myOutBufBitch, &out_buf_size, FILTER_TYPE, &bcj_st, last_time);
            if (FILTER_TYPE == k_BCJ2)
                WriteTempStream(myOutBufBitch, out_buf_size, StopDecoding, &st);
            else
                WriteStream(folderIndex, db, myOutBufBitch, out_buf_size, &st);

            if (bytes_left == 0)
            {
                to_read = true;
                in_offset = 0;
            }
            out_buf_size = OUT_BUF_SIZE;
            if (StopDecoding)
                break;
        }   
        
     }
    printf("I'm ALIVE! =) I unpacked %d(from %d) bytes and %d bytes left in input buffer\n", out_size, outSize, bytes_left);

    FREE_BUFS(myInBufBitch, myOutBufBitch);
    BCJ_free_state(bcj_st);
    LzmaDec_Free(&state, allocMain);
    return SZ_OK;
}

static SRes SzDecodeLzma2ToFileWithBuf(const UInt32 folderIndex, CSzCoderInfo *coder, const CSzArEx *db, ILookInStream *inStream, SizeT outSize, 
                                      ISzAlloc *allocMain, const UInt32 FILTER_TYPE)
{
    Byte *myInBufBitch = NULL;
    Byte *myOutBufBitch = NULL;

    CLzma2Dec state;
    Lzma2Dec_Construct(&state);
    Lzma2Dec_Allocate(&state, coder->Props.data[0], allocMain);
    Lzma2Dec_Init(&state);

    if (myInBufBitch == NULL)
        ALLOCATE_BUFS(myInBufBitch, myOutBufBitch);

    wr_st_t st;
    write_state_init(st);
    BCJ_state bcj_st;
    BCJ_state_init(bcj_st);
    size_t in_buf_size = 0, in_offset = 0;
    size_t out_size = 0;
    size_t bytes_read, bytes_left;
    bool StopDecoding = false, to_flush = false;
    SizeT out_buf_size = OUT_BUF_SIZE;
    bool to_read = true;
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
            StopDecoding = true;
            printf("lzma2 decode: something bad happened =( \n");
            return SZ_ERROR_FAIL;
        }
        out_size += out_buf_size;
        bytes_left -= in_buf_size;

        if (in_buf_size < bytes_read && in_buf_size)           // not whole in_buf was decompressed
        {
            to_read = false;
            in_offset += in_buf_size;
            in_buf_size = bytes_left;
        }

        StopDecoding = (out_size >= outSize)? true : false;
        if (bytes_left == 0 || out_buf_size == OUT_BUF_SIZE || StopDecoding)   // whole in_buf was decompressed
        {
            bool last_time = StopDecoding ? true : false;
            ApplyFilter(myOutBufBitch, &out_buf_size, FILTER_TYPE, &bcj_st, last_time);
            if (FILTER_TYPE == k_BCJ2)
                WriteTempStream(myOutBufBitch, out_buf_size, StopDecoding, &st);
            else
                WriteStream(folderIndex, db, myOutBufBitch, out_buf_size, &st);
            if (bytes_left == 0)
            {
                to_read = true;
                in_offset = 0;
            }
            out_buf_size = OUT_BUF_SIZE;
            if (StopDecoding)
                break;
        }   
    }
    printf("lzma2: I'm ALIVE! =) I unpacked %d(from %d) bytes and %d bytes left in input buffer\n", out_size, outSize, bytes_left);
    FREE_BUFS(myInBufBitch, myOutBufBitch);
    BCJ_free_state(bcj_st);
    Lzma2Dec_Free(&state, allocMain);
    return SZ_OK;
}

static SRes SzDecodeCopyToFileWithBuf(const UInt32 folderIndex, const CSzArEx *db, ILookInStream *inStream, SizeT outSize, ISzAlloc *allocMain)
{
    if (outSize <= 0 || !inStream )
        return SZ_ERROR_FAIL;
    Byte *buf = (Byte *)IAlloc_Alloc(allocMain, COPY_BUF_SIZE);
    SizeT out_size = 0, bytes_read = 0;
    wr_st_t st;
    write_state_init(st);

    while (out_size < outSize)
    {
        SizeT rem = outSize - out_size;
        bytes_read = (rem < COPY_BUF_SIZE) ? rem : COPY_BUF_SIZE;
        RINOK(inStream->Read(inStream, buf, &bytes_read));
        RINOK(WriteStream(folderIndex, db, buf, bytes_read, &st));
        out_size += bytes_read;
    }
    printf("I'm ALIVE! =) I unpacked %d(from %d) bytes\n", out_size, outSize);
    IAlloc_Free(allocMain, buf);
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



static UInt32 DetectFilter(const CSzFolder *folder)
{
    if (folder->NumCoders == 2 && folder->Coders[1].MethodID == k_BCJ )
        return k_BCJ;
    else if (folder->NumCoders == 4 && folder->Coders[3].MethodID == k_BCJ2)
        return k_BCJ2;
    else
        return 0;
}

static SRes SzFolder_Decode2ToFile(const CSzFolder *folder, const UInt32 folderIndex, const UInt64 *packSizes,
                             ILookInStream *inStream, const CSzArEx *db, UInt64 startPos,
                             SizeT outSize, ISzAlloc *allocMain, Byte *tempBuf[])
{
    UInt32 ci;
    SizeT total_out_size = outSize;
    SizeT tempSizes[3] = { 0, 0, 0};
    //SizeT tempSize3 = 0;
    //Byte *tempBuf3 = 0;

    RINOK(CheckSupportedFolder(folder));

    for (ci = 0; ci < folder->NumCoders; ci++)
    {
        CSzCoderInfo *coder = &folder->Coders[ci];
        const UInt32 FilterType = DetectFilter(folder);
        if (IS_MAIN_METHOD((UInt32)coder->MethodID))
        {
            UInt32 si = 0;
            UInt64 offset;
            UInt64 inSize;
            Byte *outBufCur = NULL;
            
            SizeT outSizeCur = outSize;
            bool unpackInMem = false;                               // for small buffers within unpacking BCJ2 we can use 
                                                                    // unpacking in memory
            if (folder->NumCoders == 4)
            {
                UInt32 indices[] = { 3, 2, 0 };
                UInt64 unpackSize = folder->UnpackSizes[ci];        // 4176, 9k, 10.610.492,
                si = indices[ci];                                   // 3, 2, 0
                if (ci < 2)
                {
                    Byte *temp;
                    unpackInMem = true;
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
                    outSize = outSizeCur = (SizeT)unpackSize;
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
                    RINOK(SzDecodeCopyToFileWithBuf(folderIndex, db, inStream, outSize, allocMain));
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
                    RINOK(SzDecodeLzmaToFileWithBuf(folderIndex, coder, db, inStream, outSize, allocMain, FilterType));
//                    RINOK(SzDecodeLzmaToFile(coder, db, inStream, outSize, allocMain));
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
                    RINOK(SzDecodeLzma2ToFileWithBuf(folderIndex, coder, db, inStream, outSize, allocMain, FilterType)); 
                }
                //RINOK(SzDecodeLzma2(coder, inSize, inStream, outBufCur, outSizeCur, allocMain));
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
            {
                Byte *myInBufBitch = NULL, *myOutBufBitch = NULL;
                Byte bcj2PrevByte = 0;
                wr_st_t wr_st;
                write_state_init(wr_st);
                r_st_t r_st;
                read_state_init(r_st);
                Bcj2_dec_state bcj2_st;
                Bcj2_dec_state_init(bcj2_st);
                bcj2_st.buf1 = tempBuf[0];
                bcj2_st.buf2 = tempBuf[1];
                bcj2_st.buf3 = tempBuf[2];
                UInt64 _total_size = 0;
                if (myInBufBitch == NULL)
                    ALLOCATE_BUFS(myInBufBitch, myOutBufBitch);

                while (total_out_size)
                {
                    SizeT myOutBufSize = OUT_BUF_SIZE;
                    SizeT processed = 0, bytes_read = IN_BUF_SIZE;
                    RINOK(ReadTempStream(myInBufBitch, &bytes_read, &r_st));

                    processed = Bcj2_DecodeToFileWithBufs(
                        myInBufBitch, bytes_read,
                        bcj2_st.buf1, &tempSizes[0],
                        bcj2_st.buf2, &tempSizes[1],
                        bcj2_st.buf3, &tempSizes[2],
                        myOutBufBitch, myOutBufSize,
                        &bcj2_st);
                    if (processed == 0)
                        break;
                    RINOK(WriteStream(folderIndex, db, myOutBufBitch, processed, &wr_st));
                    total_out_size -= processed;
                }

                FREE_BUFS(myInBufBitch, myOutBufBitch);
            }
            RINOK(res)
        }
        else    // BCJ
        {
            printf("BCJ filter in coder # %d\n", ci+1);
 /*           if (ci != 1)
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
            }*/
        }
    }
    return SZ_OK;
}


SRes SzFolder_DecodeToFile(const CSzFolder *folder, const UInt32 folderIndex, const UInt64 *packSizes,
                           ILookInStream *inStream, const CSzArEx *db, UInt64 startPos,
                           size_t outSize, ISzAlloc *allocMain)
{
    Byte *tempBuf[3] = { 0, 0, 0};
    int i;
    SRes res = SzFolder_Decode2ToFile(folder, folderIndex, packSizes, inStream, db, 
                      startPos,(SizeT)outSize, allocMain, tempBuf);
    for (i = 0; i < 3; i++)
        IAlloc_Free(allocMain, tempBuf[i]);
    return res;
}