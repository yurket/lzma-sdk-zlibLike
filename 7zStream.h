
typedef struct write_state
{
    SizeT outSize;
    SizeT bytesWritten;
    UInt32 fileToWriteIndex;          // index in CSzArEx db
    CSzFile out_file;
    bool fileOpened;
    bool FitsToOneFile;
} wr_st_t, *pwr_st_t;

#define write_state_init(st) {  st.outSize = 0;              \
                                st.bytesWritten = 0;         \
                                st.fileToWriteIndex = 0;     \
                                st.FitsToOneFile = false;    \
                                File_Construct(&st.out_file);\
                                st.fileOpened = false;       \
                             };                              \

#define OPEN_FILE(outFile, fileName)            if (OutFile_OpenW(outFile, fileName))                           \
                                                {                                                               \
                                                    wprintf(L"can not open output file \"%s\"\n", fileName);    \
                                                    res = SZ_ERROR_FAIL;                                        \
                                                    continue;                                                   \
                                                }                                                               \

#define F_WRITE(outFile, buf, size)             if (File_Write(outFile, buf, size))                     \
                                                {                                                       \
                                                    wprintf(L"can not write to output file");           \
                                                    res = SZ_ERROR_FAIL;                                \
                                                    continue;                                           \
                                                }                                                       \

SRes WriteStream(const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t size, pwr_st_t st);