/* 7zStream.c -- 7z Stream functions
2010-03-11 : Igor Pavlov : Public domain */

#include <string.h>

#include "Types.h"
#include "stdio.h"
#include "7z.h"
#include "7zStream.h"

SRes SeqInStream_Read2(ISeqInStream *stream, void *buf, size_t size, SRes errorType)
{
  while (size != 0)
  {
    size_t processed = size;
    RINOK(stream->Read(stream, buf, &processed));
    if (processed == 0)
      return errorType;
    buf = (void *)((Byte *)buf + processed);
    size -= processed;
  }
  return SZ_OK;
}

SRes SeqInStream_Read(ISeqInStream *stream, void *buf, size_t size)
{
  return SeqInStream_Read2(stream, buf, size, SZ_ERROR_INPUT_EOF);
}

SRes SeqInStream_ReadByte(ISeqInStream *stream, Byte *buf)
{
  size_t processed = 1;
  RINOK(stream->Read(stream, buf, &processed));
  return (processed == 1) ? SZ_OK : SZ_ERROR_INPUT_EOF;
}

SRes LookInStream_SeekTo(ILookInStream *stream, UInt64 offset)
{
  Int64 t = offset;
  return stream->Seek(stream, &t, SZ_SEEK_SET);
}

SRes LookInStream_LookRead(ILookInStream *stream, void *buf, size_t *size)
{
  const void *lookBuf;
  if (*size == 0)
    return SZ_OK;
  RINOK(stream->Look(stream, &lookBuf, size));
  memcpy(buf, lookBuf, *size);
  return stream->Skip(stream, *size);
}

SRes LookInStream_Read2(ILookInStream *stream, void *buf, size_t size, SRes errorType)
{
  while (size != 0)
  {
    size_t processed = size;
    RINOK(stream->Read(stream, buf, &processed));
    if (processed == 0)
      return errorType;
    buf = (void *)((Byte *)buf + processed);
    size -= processed;
  }
  return SZ_OK;
}

SRes LookInStream_Read(ILookInStream *stream, void *buf, size_t size)
{
  return LookInStream_Read2(stream, buf, size, SZ_ERROR_INPUT_EOF);
}

static SRes LookToRead_Look_Lookahead(void *pp, const void **buf, size_t *size)
{
  SRes res = SZ_OK;
  CLookToRead *p = (CLookToRead *)pp;
  size_t size2 = p->size - p->pos;
  if (size2 == 0 && *size > 0)
  {
    p->pos = 0;
    size2 = LookToRead_BUF_SIZE;
    res = p->realStream->Read(p->realStream, p->buf, &size2);
    p->size = size2;
  }
  if (size2 < *size)
    *size = size2;
  *buf = p->buf + p->pos;
  return res;
}

static SRes LookToRead_Look_Exact(void *stream, const void **buf, size_t *size)
{
  SRes res = SZ_OK;
  CLookToRead *pStream = (CLookToRead *)stream;
  size_t size2 = pStream->size - pStream->pos;
  if (size2 == 0 && *size > 0)
  {
    pStream->pos = 0;
    if (*size > LookToRead_BUF_SIZE)
      *size = LookToRead_BUF_SIZE;
    res = pStream->realStream->Read(pStream->realStream, pStream->buf, size);
    size2 = pStream->size = *size;
  }
  if (size2 < *size)
    *size = size2;
  *buf = pStream->buf + pStream->pos;
  return res;
}

static SRes LookToRead_Skip(void *pp, size_t offset)
{
  CLookToRead *p = (CLookToRead *)pp;
  p->pos += offset;
  return SZ_OK;
}

static SRes LookToRead_Read(void *pp, void *buf, size_t *size)
{
  CLookToRead *p = (CLookToRead *)pp;
  size_t rem = p->size - p->pos;
  if (rem == 0)
    return p->realStream->Read(p->realStream, buf, size);
  if (rem > *size)
    rem = *size;
  memcpy(buf, p->buf + p->pos, rem);
  p->pos += rem;
  *size = rem;
  return SZ_OK;
}

static SRes LookToRead_Seek(void *pp, Int64 *pos, ESzSeek origin)
{
  CLookToRead *p = (CLookToRead *)pp;
  p->pos = p->size = 0;
  return p->realStream->Seek(p->realStream, pos, origin);
}

void LookToRead_CreateVTable(CLookToRead *p, int lookahead)
{
  p->s.Look = lookahead ?
      LookToRead_Look_Lookahead :
      LookToRead_Look_Exact;
  p->s.Skip = LookToRead_Skip;
  p->s.Read = LookToRead_Read;
  p->s.Seek = LookToRead_Seek;
}

void LookToRead_Init(CLookToRead *p)
{
  p->pos = p->size = 0;
}

static SRes SecToLook_Read(void *pp, void *buf, size_t *size)
{
  CSecToLook *p = (CSecToLook *)pp;
  return LookInStream_LookRead(p->realStream, buf, size);
}

void SecToLook_CreateVTable(CSecToLook *p)
{
  p->s.Read = SecToLook_Read;
}

static SRes SecToRead_Read(void *pp, void *buf, size_t *size)
{
  CSecToRead *p = (CSecToRead *)pp;
  return p->realStream->Read(p->realStream, buf, size);
}

void SecToRead_CreateVTable(CSecToRead *p)
{
  p->s.Read = SecToRead_Read;
}


// ===========================================================================

static wchar_t * BaseName(wchar_t *path)
{
    bool dir_in_path = false;
    wchar_t *p = path;
    while (*p++)
        if ( *p == (wchar_t)'\\' || *p == (wchar_t)'/')
            dir_in_path = true;
    if (dir_in_path)
    {
        while ((*--p != (wchar_t)'\\') && (*p != (wchar_t)'/'));
        return ++p;
    }
    return path;
}
//static wchar_t * TempName(wchar_t *baseName)
//{
//    wchar_t *tempName = (wchar_t *) new wchar_t(255);
//
//    wcscpy(tempName, L"tempDir/");
//    wcscat(tempName, baseName);
//    return tempName;
//}
SizeT CountBytesToWrite(const UInt32 folderIndex, const CSzArEx *db, size_t buf_size, pwr_st_t st)
{
    const SizeT startOffset = st->bytesWritten;
    SizeT filesOffsetSums = 0;
    SizeT bytesToWriteInCurFile = 0;
    for (UInt32 i = 0; i < db->db.NumFiles; i++)
    {
        if (db->FileIndexToFolderIndexMap[i] != folderIndex)        // skip files from others folders
            continue;

        CSzFileItem &curFile = db->db.Files[i];
        if (curFile.IsDir)
            continue;

        filesOffsetSums += curFile.Size;
        if (startOffset < filesOffsetSums)        // need to flush data in file with index i
        {
            st->fileToWriteIndex = i;
            Int64 remForCurFile = (Int64)filesOffsetSums - (startOffset + buf_size);    // must be signed
            if ( remForCurFile > 0 )                                             // whole buf fits to current file
            {
                st->FitsToOneFile = true;
                return buf_size;
            }
            else                                                                // need to split writing into several files
            {
                st->FitsToOneFile = false;
                return filesOffsetSums - startOffset;
            }
        }
    }
    return 0;
}

SRes WriteStream(IFileStream  *IFile, const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t buf_size, pwr_st_t st)
{
    SRes res = SZ_OK;
    SizeT offset = 0;
    if (buf == NULL)
        return SZ_ERROR_DATA;
    if (buf_size == 0)
        return SZ_OK;

    while (buf_size)
    {
        wchar_t *fileName = NULL;
        SizeT bytesToWrite = CountBytesToWrite(folderIndex, db, buf_size, st);
        for (UInt32 i = 0; i < db->db.NumFiles; i++)
        {
            CSzFileItem &curFile = db->db.Files[i];
            if (i == st->fileToWriteIndex)
            {
                fileName = (wchar_t *)db->FileNames.data + db->FileNameOffsets[i];
                //fileName = BaseName(fileName);
                break;
            }
        }

        if (!st->fileOpened)
        {
            OPEN_FILE_OUT(&st->out_file, fileName);
            st->fileOpened = true;
        }

        while(bytesToWrite)
        {
            size_t bytesWritten = bytesToWrite;
            F_WRITE(&st->out_file, buf + offset, bytesWritten);
            bytesToWrite -= bytesWritten;
            buf_size -= bytesWritten;
            offset += bytesWritten;
            st->bytesWritten += bytesWritten;
        }
        if (!st->FitsToOneFile)
        {
            File_Close(&st->out_file);
            st->fileOpened = false;
        }
    }

    return res;
}


SRes WriteTempStream(IFileStream  *IFile, Byte *buf, size_t buf_size, bool StopWriting, pwr_st_t st)
{
    SRes res = SZ_OK;
    wchar_t *fileName = L"temp.dat";
    if (buf == NULL)
        return SZ_ERROR_DATA;
    if (buf_size == 0)
        return SZ_OK;
    if (!st->fileOpened)
    {
        OPEN_FILE_OUT(&st->out_file, fileName);
        st->fileOpened = true;
    }

    while (buf_size)
    {
        SizeT bytes_to_write = buf_size;
        F_WRITE(&st->out_file, buf, bytes_to_write);
        buf_size -= bytes_to_write;
    }

    if (StopWriting)
    {
        File_Close(&st->out_file);
        st->fileOpened = false;
    }

    return res;
}
SRes ReadTempStream(IFileStream  *IFile, Byte *buf, size_t *buf_size, pr_st_t st)
{
    SRes res = SZ_OK;
    wchar_t *fileName = L"temp.dat";
    if (buf == NULL)
        return SZ_ERROR_DATA;
    if (*buf_size == 0)
        return SZ_OK;
    if (!st->fileOpened)
    {
        OPEN_FILE_IN(&st->in_file, fileName);
        st->fileOpened = true;
    }

    SizeT bytes_to_read = *buf_size;
    F_READ(&st->in_file, buf, &bytes_to_read);
    *buf_size = bytes_to_read;

    if (bytes_to_read < *buf_size || res )
    {
        File_Close(&st->in_file);
        st->fileOpened = false;
    }
    return res;
}