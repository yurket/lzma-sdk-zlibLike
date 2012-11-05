
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

typedef struct read_state
{
    SizeT inSize;
    SizeT bytesRead;
    UInt32 fileToReadIndex;
    CSzFile in_file;
    bool fileOpened;
    bool FitsToOneFile;
} r_st_t, *pr_st_t;

#define read_state_init(st) {  st.inSize = 0;               \
                               st.bytesRead = 0;            \
                               st.fileToReadIndex = 0;      \
                               st.FitsToOneFile = false;    \
                               File_Construct(&st.in_file); \
                               st.fileOpened = false;       \
                            };                              \

#define OPEN_FILE_OUT(File, fileName)           if (OutFile_OpenW(File, fileName))                              \
                                                {                                                               \
                                                    wprintf(L"can not open output file \"%s\"\n", fileName);    \
                                                    res = SZ_ERROR_FAIL;                                        \
                                                    return res;                                                   \
                                                }        

#define OPEN_FILE_IN(File, fileName)           if (InFile_OpenW(File, fileName))                              \
                                               {                                                               \
                                                   wprintf(L"can not open output file \"%s\"\n", fileName);    \
                                                   res = SZ_ERROR_FAIL;                                        \
                                                   return res;                                                   \
                                               }  \

#define F_WRITE(outFile, buf, size)             if (File_Write(outFile, buf, size))                     \
                                                {                                                       \
                                                    wprintf(L"can not write to output file");           \
                                                    res = SZ_ERROR_FAIL;                                \
                                                    return res;                                           \
                                                }                                                       \

#define F_READ(inFile, buf, size)               if (File_Read(inFile, buf, size))                       \
                                                {                                                       \
                                                    wprintf(L"can not read file");                      \
                                                    res = SZ_ERROR_FAIL;                                \
                                                }                                                       \


SRes WriteStream(const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t size, pwr_st_t st);
SRes WriteTempStream(Byte *buf, size_t buf_size, bool StopWriting, pwr_st_t st);
SRes ReadStream(const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t *size, pr_st_t st);
SRes ReadTempStream(Byte *buf, size_t *buf_size, pr_st_t st);