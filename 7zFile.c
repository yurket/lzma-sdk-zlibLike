/* 7zFile.c -- File IO
2009-11-24 : Igor Pavlov : Public domain */

#include "7zFile.h"

#ifndef USE_WINDOWS_FILE

#ifndef UNDER_CE
#include <errno.h>
#endif

#else

/*
   ReadFile and WriteFile functions in Windows have BUG:
   If you Read or Write 64MB or more (probably min_failure_size = 64MB - 32KB + 1)
   from/to Network file, it returns ERROR_NO_SYSTEM_RESOURCES
   (Insufficient system resources exist to complete the requested service).
   Probably in some version of Windows there are problems with other sizes:
   for 32 MB (maybe also for 16 MB).
   And message can be "Network connection was lost"
*/

#define kChunkSizeMax (1 << 22)

#endif

void File_Construct(CSzFile *p)
{
  #ifdef USE_WINDOWS_FILE
  p->handle = INVALID_HANDLE_VALUE;
  #else
  p->file = NULL;
  #endif
}

#if !defined(UNDER_CE) || !defined(USE_WINDOWS_FILE)
static WRes File_Open(CSzFile *p, const char *name, int writeMode)
{
  #ifdef USE_WINDOWS_FILE
  p->handle = CreateFileA(name,
      writeMode ? GENERIC_WRITE : GENERIC_READ,
      FILE_SHARE_READ, NULL,
      writeMode ? CREATE_ALWAYS : OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, NULL);
  return (p->handle != INVALID_HANDLE_VALUE) ? 0 : GetLastError();
  #else
  p->file = fopen(name, writeMode ? "wb+" : "rb");
  return (p->file != 0) ? 0 :
    #ifdef UNDER_CE
    2; /* ENOENT */
    #else
    errno;
    #endif
  #endif
}

WRes InFile_Open(CSzFile *p, const char *name) { return File_Open(p, name, 0); }
WRes OutFile_Open(CSzFile *p, const char *name) { return File_Open(p, name, 1); }
#endif

#ifdef USE_WINDOWS_FILE
WRes File_OpenW(CSzFile *p, const WCHAR *name, int writeMode, int isTemp)
{
    if (name == NULL && !isTemp)
        return 1;

    p->handle = CreateFileW(isTemp? L"temp.dat" : name,                     // it would be better to generate some pseudorandom name
      writeMode ? GENERIC_WRITE : GENERIC_READ,
      FILE_SHARE_READ, NULL,
      writeMode ? CREATE_ALWAYS : OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, NULL);
  return (p->handle != INVALID_HANDLE_VALUE) ? 0 : GetLastError();
}
WRes InFile_OpenW(CSzFile *p, const WCHAR *name, int isTemp) { return File_OpenW(p, name, 0, isTemp); }
WRes OutFile_OpenW(CSzFile *p, const WCHAR *name, int isTemp) { return File_OpenW(p, name, 1, isTemp); }
#endif

WRes File_Close(CSzFile *p)
{
  #ifdef USE_WINDOWS_FILE
  if (p && (p->handle != INVALID_HANDLE_VALUE))
  {
    if (!CloseHandle(p->handle))
      return GetLastError();
    p->handle = INVALID_HANDLE_VALUE;
  }
  #else
  if (p->file != NULL)
  {
    int res = fclose(p->file);
    if (res != 0)
      return res;
    p->file = NULL;
  }
  #endif
  return 0;
}

WRes File_Read(CSzFile *p, void *data, size_t *size)
{
  size_t originalSize = *size;
  if (originalSize == 0)
    return 0;

  #ifdef USE_WINDOWS_FILE

  *size = 0;
  do
  {
    DWORD curSize = (originalSize > kChunkSizeMax) ? kChunkSizeMax : (DWORD)originalSize;
    DWORD processed = 0;
    BOOL res = ReadFile(p->handle, data, curSize, &processed, NULL);
    data = (void *)((Byte *)data + processed);
    originalSize -= processed;
    *size += processed;
    if (!res)
      return GetLastError();
    if (processed == 0)
      break;
  }
  while (originalSize > 0);
  return 0;

  #else
  
  *size = fread(data, 1, originalSize, p->file);
  if (*size == originalSize)
    return 0;
  return ferror(p->file);
  
  #endif
}

WRes File_Write(CSzFile *p, const void *data, size_t *size)
{
  size_t originalSize = *size;
  if (originalSize == 0)
    return 0;
  
  #ifdef USE_WINDOWS_FILE

  *size = 0;
  do
  {
    DWORD curSize = (originalSize > kChunkSizeMax) ? kChunkSizeMax : (DWORD)originalSize;
    DWORD processed = 0;
    BOOL res = WriteFile(p->handle, data, curSize, &processed, NULL);
    data = (void *)((Byte *)data + processed);
    originalSize -= processed;
    *size += processed;
    if (!res)
      return GetLastError();
    if (processed == 0)
      break;
  }
  while (originalSize > 0);
  return 0;

  #else

  *size = fwrite(data, 1, originalSize, p->file);
  if (*size == originalSize)
    return 0;
  return ferror(p->file);
  
  #endif
}

WRes File_Seek(CSzFile *p, Int64 *pos, ESzSeek origin)
{
  #ifdef USE_WINDOWS_FILE

  LARGE_INTEGER value;
  DWORD moveMethod;
  value.LowPart = (DWORD)*pos;
  value.HighPart = (LONG)((UInt64)*pos >> 16 >> 16); /* for case when UInt64 is 32-bit only */
  switch (origin)
  {
    case SZ_SEEK_SET: moveMethod = FILE_BEGIN; break;
    case SZ_SEEK_CUR: moveMethod = FILE_CURRENT; break;
    case SZ_SEEK_END: moveMethod = FILE_END; break;
    default: return ERROR_INVALID_PARAMETER;
  }
  value.LowPart = SetFilePointer(p->handle, value.LowPart, &value.HighPart, moveMethod);
  if (value.LowPart == 0xFFFFFFFF)
  {
    WRes res = GetLastError();
    if (res != NO_ERROR)
      return res;
  }
  *pos = ((Int64)value.HighPart << 32) | value.LowPart;
  return 0;

  #else
  
  int moveMethod;
  int res;
  switch (origin)
  {
    case SZ_SEEK_SET: moveMethod = SEEK_SET; break;
    case SZ_SEEK_CUR: moveMethod = SEEK_CUR; break;
    case SZ_SEEK_END: moveMethod = SEEK_END; break;
    default: return 1;
  }
  res = fseek(p->file, (long)*pos, moveMethod);
  *pos = ftell(p->file);
  return res;
  
  #endif
}

WRes File_GetLength(CSzFile *p, UInt64 *length)
{
  #ifdef USE_WINDOWS_FILE
  
  DWORD sizeHigh;
  DWORD sizeLow = GetFileSize(p->handle, &sizeHigh);
  if (sizeLow == 0xFFFFFFFF)
  {
    DWORD res = GetLastError();
    if (res != NO_ERROR)
      return res;
  }
  *length = (((UInt64)sizeHigh) << 32) + sizeLow;
  return 0;
  
  #else
  
  long pos = ftell(p->file);
  int res = fseek(p->file, 0, SEEK_END);
  *length = ftell(p->file);
  fseek(p->file, pos, SEEK_SET);
  return res;
  
  #endif
}


/* ---------- FileSeqInStream ---------- */

static SRes FileSeqInStream_Read(void *pp, void *buf, size_t *size)
{
  CFileSeqInStream *p = (CFileSeqInStream *)pp;
  return File_Read(&p->file, buf, size) == 0 ? SZ_OK : SZ_ERROR_READ;
}

void FileSeqInStream_CreateVTable(CFileSeqInStream *p)
{
  p->s.Read = FileSeqInStream_Read;
}


/* ---------- FileInStream ---------- */

static SRes FileInStream_Read(void *pp, void *buf, size_t *size)
{
  CFileInStream *p = (CFileInStream *)pp;
  return (File_Read(&p->file, buf, size) == 0) ? SZ_OK : SZ_ERROR_READ;
}

static SRes FileInStream_Seek(void *pp, Int64 *pos, ESzSeek origin)
{
  CFileInStream *p = (CFileInStream *)pp;
  return File_Seek(&p->file, pos, origin);
}

void FileInStream_CreateVTable(CFileInStream *p)
{
  p->s.Read = FileInStream_Read;
  p->s.Seek = FileInStream_Seek;
}


/* ---------- FileOutStream ---------- */

static size_t FileOutStream_Write(void *pp, const void *data, size_t size)
{
  CFileOutStream *p = (CFileOutStream *)pp;
  File_Write(&p->file, data, &size);
  return size;
}

void FileOutStream_CreateVTable(CFileOutStream *p)
{
  p->s.Write = FileOutStream_Write;
}


/* ------------ IFileStream ------------ */
static WRes IFileStream_OpenWrite(IFileStream *pFileStream, const wchar_t *name, int isTemp)
{
    CSzFile *newFile = (CSzFile *)IAlloc_Alloc(pFileStream->mem_alctr, sizeof(CSzFile));
    if (isTemp)
        pFileStream->tempFile = (void *)newFile;
    else
        pFileStream->realFile = (void *)newFile;
    pFileStream->curFileName = name;
    return OutFile_OpenW(newFile, name, isTemp);
}

static WRes IFileStream_OpenRead(IFileStream *pFileStream, const wchar_t *name, int isTemp)
{
    CSzFile *newFile = (CSzFile *)IAlloc_Alloc(pFileStream->mem_alctr, sizeof(CSzFile));
    if (isTemp)
        pFileStream->tempFile = (void *)newFile;
    else
        pFileStream->realFile = (void *)newFile;
    pFileStream->curFileName = name;
    return InFile_OpenW(newFile, name, isTemp);
}

static size_t IFileStream_Write(IFileStream *pFileStream, const void *data, size_t size, int isTemp)
{
    CSzFile *p;
    if (isTemp)
        p = (CSzFile *)pFileStream->tempFile;
    else
        p = (CSzFile *)pFileStream->realFile;
    File_Write(p, data, &size);
    return size;
}
static SRes IFileStream_Read(IFileStream *pFileStream, void *data, size_t *size, int isTemp)
{
    CSzFile *p;
    if (isTemp)
        p = (CSzFile *)pFileStream->tempFile;
    else
        p = (CSzFile *)pFileStream->realFile;
    return (File_Read(p, data, size) == 0) ? SZ_OK : SZ_ERROR_READ;
}
static void IFileStream_CloseFile(IFileStream *pFileStream, int isTemp)
{
    CSzFile *p;
    if (isTemp)
        p = (CSzFile *)pFileStream->tempFile;
    else
        p = (CSzFile *)pFileStream->realFile;
    File_Close(p);
    IAlloc_Free(pFileStream->mem_alctr, p);                 //  ����� ����� ����������� ������ �� ���������????????? 
    p = NULL;
}

static SRes FileDelete(IFileStream *pFileStream, void *name)
{
#ifdef _WIN32
    LPCWSTR fileName = (LPCWSTR)name;
    if (name == NULL)
        return SZ_ERROR_FAIL;
    if (DeleteFileW(fileName) == 0)
        return GetLastError();
    else
        return SZ_OK;
#else
    char *fileName = (char *)name;
    if (name == NULL)
        return SZ_ERROR_FAIL;
    if( remove(fileName) != 0 )
        perror( "Error deleting file" );
    else
        return SZ_OK;
#endif
}

static void IFileStream_DeleteFile(IFileStream *pp, void *name)
{
    FileDelete(pp, name);
}
void IFileStream_CreateVTable(IFileStream *p, ISzAlloc *alctr)
{
    p->OpenInFile = IFileStream_OpenRead;
    p->OpenOutFile = IFileStream_OpenWrite;
    p->FileRead = IFileStream_Read;
    p->FileWrite = IFileStream_Write;
    p->FileClose = IFileStream_CloseFile;
    p->FileRemove = IFileStream_DeleteFile;
    p->mem_alctr = alctr;
}