#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "7zVersion.h"

static int TestSignatureCandidate(Byte *testBytes)
{
    size_t i;
    for (i = 0; i < k7zSignatureSize; i++)
        if (testBytes[i] != k7zSignature[i])
            return 0;
    return 1;
}
#define BUF_SIZE (1 << 20)

static char *LetsFind7z(char *fileName)
{
    int status = 0;
    FILE *fin = fopen(fileName, "rb");
    long in_file_size = 0, cur_pos;
    FILE *fout = fopen("7zpart.7z", "wb");
    Byte buf[k7zSignatureSize];
    Byte *write_buf = (Byte *) new Byte[BUF_SIZE];
    if (!fin || !fout)
    {
        printf("Can't open file %s\n", (!fin) ? fileName: "7zpart.7z");
        return NULL;
    }

    fseek(fin, 0, SEEK_END);
    in_file_size = ftell(fin);
    rewind(fin);

    while (True)
    {
        fread(buf, sizeof(Byte), k7zSignatureSize, fin);
        if (TestSignatureCandidate(buf))
        {
            fseek(fin, -((int)k7zSignatureSize), SEEK_CUR);
            break;
        }
        status = fseek(fin, -((int)k7zSignatureSize) + 1, SEEK_CUR);
        cur_pos = ftell(fin);
        if (status || cur_pos >= (in_file_size - k7zSignatureSize))
        {
            printf("Can't find s7 signature -> shutting down\n");
            return NULL;
        }
    }
    UInt32 bytes_to_write = BUF_SIZE, read;
    while(True)
    {
        read = fread(write_buf, sizeof(Byte), BUF_SIZE, fin);
        if (read == 0)
            break;
        if (read != BUF_SIZE)
            bytes_to_write = read;
        fwrite(write_buf, sizeof(Byte), bytes_to_write, fout);
    }
    fclose(fin);
    fclose(fout);
    delete [] write_buf;
    write_buf = NULL;
    return "7zpart.7z";
}

static void Cleanup(IFileStream *IFile)
{
    IFile->FileRemove(IFile, L"temp.dat");
    IFile->FileRemove(IFile, L"7zpart.7z");
}

int main(int argc, char *argv[])
{
    char *FileName = NULL;
    CLookToRead lookStream;
    CFileInStream archiveStream;
    IFileStream IFile;
    CSzArEx db;              /* 7z archive database structure */
    ISzAlloc allocImp;       /* memory functions for main pool */
    ISzAlloc allocTempImp;
    SRes res; 
    unsigned int i = 0;
    size_t *pOffsets = NULL;

    allocImp.Alloc = SzAlloc;
    allocImp.Free = SzFree;
    allocTempImp.Alloc = SzAllocTemp;
    allocTempImp.Free = SzFreeTemp;

    if (argc == 2)  FileName = argv[1];
    else if (argc == 3) FileName = argv[2];
    else
    {
        printf("to much args!\n");
        return 1;
    }
    char *SzFileName = LetsFind7z(FileName);
    if (SzFileName == NULL)
        return -1;

    if (InFile_Open(&archiveStream.file, SzFileName))
    {
        printf("can not open input file %s\n", SzFileName);
        return 1;
    }
    FileInStream_CreateVTable(&archiveStream);
    LookToRead_CreateVTable(&lookStream, False);

    lookStream.realStream = &archiveStream.s;
    LookToRead_Init(&lookStream);
    
    IFileStream_CreateVTable(&IFile, &allocImp);
    CrcGenerateTable();

    printf("Unpacking...\n");
    SzArEx_Init(&db);
    res = SzArEx_Open(&db, &lookStream.s, &allocImp, &allocTempImp);
    switch (res)
    {
    case SZ_OK:
        wprintf(L"[+] SzArEx_Open: Ok!\n");
        break;
    case SZ_ERROR_UNSUPPORTED:
        wprintf(L"[-] SzArEx_Open: Error unsupported!\n");
        break;
    case SZ_ERROR_FAIL:
        wprintf(L"[-] SzArEx_Open: Some error occured! (Fail)\n");
        break;
    case SZ_ERROR_CRC:
        wprintf(L"[-] SzArEx_Open: CRC error! (Fail)\n");
        break;
    default:
        wprintf(L"[-] SzArEx_Open: Unknown error!\n");
        break;
    }
    RINOK(res);

    printf("file count: %d, NumPackSterams: %d, \n", db.db.NumFiles, db.db.NumPackStreams);
    printf("====================================================\n");

    for (i = 0, pOffsets = db.FileNameOffsets; i < db.db.NumFiles; i++)
    {
        CSzFileItem *file = db.db.Files + i;
        wprintf(L"%-2d: %-37s - %s, unpacked: %ld\n",
            i,
            (db.FileNames.data != NULL) ? db.FileNames.data + (*pOffsets++)*sizeof(wchar_t) : (Byte *)L"[stream]",
            file->IsDir ? L"dir" : L"file",
            file->Size);
    }
    printf("====================================================\n");

    long packed = 0, unpacked = 0;
    for (int folderIndex = 0; folderIndex < db.db.NumFolders; folderIndex++)
    {
        CSzFolder *folder = db.db.Folders + folderIndex;
        unpacked += SzFolder_GetUnpackSize(folder);
        UInt64 foler_packed = 0;
        SzArEx_GetFolderFullPackSize(&db, folderIndex, &foler_packed);
        packed += foler_packed;
    }
    printf("unpacked: %ld, packed: %ld\n", unpacked, packed);
    res = ExtractAllFiles(&db, &lookStream.s, &IFile, &allocImp);
    switch (res)
    {
    case SZ_OK:
        wprintf(L"[+] ExtractAllFiles: Ok! No errors detected \n");
        break;
    case SZ_ERROR_UNSUPPORTED:
        wprintf(L"[-] ExtractAllFiles: Error unsupported!\n");
        break;
    default:
        wprintf(L"[-] ExtractAllFiles: Some error occured!\n");
        break;
    }

    RINOK(res);

    ExtractZeroSizeFiles(&db, &IFile);

    File_Close(&archiveStream.file);
    SzArEx_Free(&db, &allocImp);
    Cleanup(&IFile);

    system("pause");
    return 0;
}