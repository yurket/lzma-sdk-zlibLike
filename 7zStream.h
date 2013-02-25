#ifndef __7Z_STREAM_H
#define __7Z_STREAM_H

#include "7z.h"
#include "7zFile.h"
#include "Types.h"

struct write_state_t
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

static void read_state_init(struct read_state_t * s)
{
    s->inSize = 0;
    s->bytesRead = 0;
    s->fileToReadIndex = 0;
    s->FitsToOneFile = False;
    File_Construct(&(s->in_file));
    s->fileOpened = False;
}

SRes WriteStream(IFileStream  *IFile, const UInt32 folderIndex, const CSzArEx *db, Byte *buf, SizeT size, struct write_state_t * st);
SRes WriteTempStream(IFileStream  *IFile, Byte *buf, SizeT buf_size, Bool StopWriting, struct write_state_t * st);
SRes ReadTempStream(IFileStream  *IFile, Byte *buf, SizeT *buf_size, struct read_state_t * st);

#endif /* __7Z_STREAM_H */
