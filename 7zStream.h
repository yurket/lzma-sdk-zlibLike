
typedef struct write_state
{
    SizeT outSize;
    UInt64 bytesWritten;
    UInt32 fileToWriteIndex;          // index in CSzArEx db
    CSzFile out_file;
    Bool fileOpened;
    Bool FitsToOneFile;
} wr_st_t, *pwr_st_t;

#define write_state_init(st) {  st.outSize = 0;              \
                                st.bytesWritten = 0;         \
                                st.fileToWriteIndex = 0;     \
                                st.FitsToOneFile = False;    \
                                File_Construct(&st.out_file);\
                                st.fileOpened = False;       \
                             };                              \

typedef struct read_state
{
    SizeT inSize;
    SizeT bytesRead;
    UInt32 fileToReadIndex;
    CSzFile in_file;
    Bool fileOpened;
    Bool FitsToOneFile;
} r_st_t, *pr_st_t;

#define read_state_init(st) {  st.inSize = 0;               \
                               st.bytesRead = 0;            \
                               st.fileToReadIndex = 0;      \
                               st.FitsToOneFile = False;    \
                               File_Construct(&st.in_file); \
                               st.fileOpened = False;       \
                            };                              \




SRes WriteStream(IFileStream  *IFile, const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t size, pwr_st_t st);
SRes WriteTempStream(IFileStream  *IFile, Byte *buf, size_t buf_size, Bool StopWriting, pwr_st_t st);
SRes ReadTempStream(IFileStream  *IFile, Byte *buf, size_t *buf_size, pr_st_t st);