
typedef struct write_state
{
    SizeT outSize;
    UInt64 bytesWritten;
    UInt32 fileToWriteIndex;          // index in CSzArEx db
    CSzFile out_file;
    Bool fileOpened;
    Bool FitsToOneFile;

};

static void write_state_init(struct write_state_t * s)
{
    s->outSize = 0;
    s->bytesWritten = 0;
    s->fileToWriteIndex = 0;
    s->FitsToOneFile = False;
    File_Construct(&(s->out_file));
    s->fileOpened = False;
}

struct read_state_t
{
    SizeT inSize;
    SizeT bytesRead;
    UInt32 fileToReadIndex;
    CSzFile in_file;
    Bool fileOpened;
    Bool FitsToOneFile;
};



SRes WriteStream(IFileStream  *IFile, const UInt32 folderIndex, const CSzArEx *db, Byte *buf, size_t size, pwr_st_t st);
SRes WriteTempStream(IFileStream  *IFile, Byte *buf, size_t buf_size, Bool StopWriting, pwr_st_t st);
SRes ReadTempStream(IFileStream  *IFile, Byte *buf, size_t *buf_size, pr_st_t st);