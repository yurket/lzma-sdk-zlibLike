#include <stdio.h>
#include <string.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "7zVersion.h"

int main(int argc, char *argv[])
{
    char *FileName = argv[1];
    CLookToRead lookStream;
    CFileInStream archiveStream;
    CSzArEx db;              /* 7z archive database structure */
    ISzAlloc allocImp;       /* memory functions for main pool */
    ISzAlloc allocTempImp;
    SRes res; 
    int i = 0, j = 0;
    size_t *pOffsets = NULL;

    allocImp.Alloc = SzAlloc;
    allocImp.Free = SzFree;
    allocTempImp.Alloc = SzAllocTemp;
    allocTempImp.Free = SzFreeTemp;

    if (argc > 2)
    {
        printf("to much args!\n");
        return 1;
    }
    if (InFile_Open(&archiveStream.file, FileName))
    {
        printf("can not open input file\n");
        return 1;
    }

    FileInStream_CreateVTable(&archiveStream);
    LookToRead_CreateVTable(&lookStream, False);

    lookStream.realStream = &archiveStream.s;
    LookToRead_Init(&lookStream);

    CrcGenerateTable();

    SzArEx_Init(&db);
    res = SzArEx_Open(&db, &lookStream.s, &allocImp, &allocTempImp);

    res == SZ_OK ? printf("open archive ok\n") : printf("not ok\n");
    printf("file count: %d, NumPackSterams: %d, \n", db.db.NumFiles, db.db.NumPackStreams);
    printf("====================================================\n");
    //printf("\n======= NAMES =========\n");
    //{
    //    wchar_t *start_pos = db.FileNames.data;
    //    for (i = 0; i < db.db.NumFiles; )
    //    {
    //        if (*start_pos == (wchar_t *)0)
    //        {
    //            i++;
    //            wprintf(L"%c\n", *start_pos++);
    //            continue;
    //        }
    //        wprintf(L"%c", *start_pos++);
    //    }
    //}

    for (i = 0, pOffsets = db.FileNameOffsets; i < db.db.NumFiles; i++)
    {
        CSzFileItem *file = db.db.Files + i;
        wprintf(L"%-2d: %-37s - %s, unpacked: %ld\n", 
            i, db.FileNames.data + (*pOffsets++)*sizeof(wchar_t) ,file->IsDir?L"dir":L"file", file->Size);
    }


    {
        HANDLE hOutFile = NULL;
        unsigned int blockIndex = -1;
        unsigned char *outBuffer = (char *)0; 
        unsigned int outBufferSize = 0;  
        unsigned int offset = 0;
        unsigned int outSizeProcessed = 0;

        res = SzArEx_Extract(&db, &lookStream.s, 0,&blockIndex, &outBuffer, &outBufferSize, \
            &offset, &outSizeProcessed, &allocImp, &allocTempImp);
        res == SZ_OK ? printf("extr ok\n") : printf("extr not ok\n");
        if (outSizeProcessed != outBufferSize)
            printf("[!] buf size %d bytes, while processed %d bytes!", outBufferSize, outSizeProcessed);

        hOutFile = CreateFileW(db.FileNames.data, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        WriteFile(hOutFile, outBuffer, outBufferSize, &outSizeProcessed, NULL);
        if (outSizeProcessed != outBufferSize)
            printf("[!] buf size %d bytes, while written %d bytes!", outBufferSize, outSizeProcessed);
        CloseHandle(hOutFile);
    }
    system("pause");
    return 0;
}