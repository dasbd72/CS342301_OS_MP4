// filesys.cc
//	Routines to manage the overall operation of the file system.
//	Implements routines to map from textual file names to files.
//
//	Each file in the file system has:
//	   A file header, stored in a sector on disk
//		(the size of the file header data structure is arranged
//		to be precisely the size of 1 disk sector)
//	   A number of data blocks
//	   An entry in the file system directory
//
// 	The file system consists of several data structures:
//	   A bitmap of free disk sectors (cf. bitmap.h)
//	   A directory of file names and file headers
//
//      Both the bitmap and the directory are represented as normal
//	files.  Their file headers are located in specific sectors
//	(sector 0 and sector 1), so that the file system can find them
//	on bootup.
//
//	The file system assumes that the bitmap and directory files are
//	kept "open" continuously while Nachos is running.
//
//	For those operations (such as Create, Remove) that modify the
//	directory and/or bitmap, if the operation succeeds, the changes
//	are written immediately back to disk (the two files are kept
//	open during all this time).  If the operation fails, and we have
//	modified part of the directory and/or bitmap, we simply discard
//	the changed version, without writing it back to disk.
//
// 	Our implementation at this point has the following restrictions:
//
//	   there is no synchronization for concurrent accesses
//	   files have a fixed size, set when the file is created
//	   files cannot be bigger than about 3KB in size
//	   there is no hierarchical directory structure, and only a limited
//	     number of files can be added to the system
//	   there is no attempt to make the system robust to failures
//	    (if Nachos exits in the middle of an operation that modifies
//	    the file system, it may corrupt the disk)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.
#ifndef FILESYS_STUB

#include "filesys.h"

#include "copyright.h"
#include "debug.h"
#include "directory.h"
#include "disk.h"
#include "filehdr.h"
#include "pbitmap.h"

// Sectors containing the file headers for the bitmap of free sectors,
// and the directory of files.  These file headers are placed in well-known
// sectors, so that they can be located on boot-up.
#define FreeMapSector 0
#define DirectorySector 1

// Initial file sizes for the bitmap and directory; until the file system
// supports extensible files, the directory size sets the maximum number
// of files that can be loaded onto the disk.
#define FreeMapFileSize (NumSectors / BitsInByte)
// MP4 start
#define NumDirEntries 64
// MP4 end
#define DirectoryFileSize (sizeof(DirectoryEntry) * NumDirEntries)

//----------------------------------------------------------------------
// FileSystem::FileSystem
// 	Initialize the file system.  If format = TRUE, the disk has
//	nothing on it, and we need to initialize the disk to contain
//	an empty directory, and a bitmap of free sectors (with almost but
//	not all of the sectors marked as free).
//
//	If format = FALSE, we just have to open the files
//	representing the bitmap and the directory.
//
//	"format" -- should we initialize the disk?
//----------------------------------------------------------------------

FileSystem::FileSystem(bool format) {
    DEBUG(dbgFile, "Initializing the file system.");
    if (format) {
        PersistentBitmap *freeMap = new PersistentBitmap(NumSectors);
        Directory *directory = new Directory(NumDirEntries);
        FileHeader *mapHdr = new FileHeader;
        FileHeader *dirHdr = new FileHeader;

        DEBUG(dbgFile, "Formatting the file system.");

        // First, allocate space for FileHeaders for the directory and bitmap
        // (make sure no one else grabs these!)
        freeMap->Mark(FreeMapSector);
        freeMap->Mark(DirectorySector);

        // Second, allocate space for the data blocks containing the contents
        // of the directory and bitmap files.  There better be enough space!

        ASSERT(mapHdr->Allocate(freeMap, FreeMapFileSize));
        ASSERT(dirHdr->Allocate(freeMap, DirectoryFileSize));

        // Flush the bitmap and directory FileHeaders back to disk
        // We need to do this before we can "Open" the file, since open
        // reads the file header off of disk (and currently the disk has garbage
        // on it!).

        DEBUG(dbgFile, "Writing headers back to disk.");
        mapHdr->WriteBack(FreeMapSector);
        dirHdr->WriteBack(DirectorySector);

        // OK to open the bitmap and directory files now
        // The file system operations assume these two files are left open
        // while Nachos is running.

        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);

        // Once we have the files "open", we can write the initial version
        // of each file back to disk.  The directory at this point is completely
        // empty; but the bitmap has been changed to reflect the fact that
        // sectors on the disk have been allocated for the file headers and
        // to hold the file data for the directory and bitmap.

        DEBUG(dbgFile, "Writing bitmap and directory back to disk.");
        freeMap->WriteBack(freeMapFile);  // flush changes to disk
        directory->WriteBack(directoryFile);

        if (debug->IsEnabled('f')) {
            freeMap->Print();
            directory->Print();
        }
        delete freeMap;
        delete directory;
        delete mapHdr;
        delete dirHdr;
    } else {
        // if we are not formatting the disk, just open the files representing
        // the bitmap and directory; these are left open while Nachos is running
        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);
    }
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileSystem::~FileSystem
//----------------------------------------------------------------------
FileSystem::~FileSystem() {
    delete freeMapFile;
    delete directoryFile;
}

void Parser(OpenFile *directoryFile, Directory *&directory, OpenFile *&dirFile, char *&token, int &sector, char *name) {
    DEBUG(dbgFile, "Parser(" << name << ")");
    char *next;

    dirFile = directoryFile;

    directory = new Directory(NumDirEntries);
    directory->FetchFrom(dirFile);

    token = strtok(name, "/");
    if (token != NULL) {
        next = strtok(NULL, "/");
        sector = directory->Find(token);
        while (next != NULL && sector != -1) {
            if (dirFile != directoryFile)
                delete dirFile;
            dirFile = new OpenFile(sector);
            directory->FetchFrom(dirFile);

            token = next;
            next = strtok(NULL, "/");
            sector = directory->Find(token);
        }
    }
}

//----------------------------------------------------------------------
// FileSystem::Create
// 	Create a file in the Nachos file system (similar to UNIX create).
//	Since we can't increase the size of files dynamically, we have
//	to give Create the initial size of the file.
//
//	The steps to create a file are:
//	  Make sure the file doesn't already exist
//        Allocate a sector for the file header
// 	  Allocate space on disk for the data blocks for the file
//	  Add the name to the directory
//	  Store the new file header on disk
//	  Flush the changes to the bitmap and the directory back to disk
//
//	Return TRUE if everything goes ok, otherwise, return FALSE.
//
// 	Create fails if:
//   		file is already in directory
//	 	no free space for file header
//	 	no free entry for file in directory
//	 	no free space for data blocks for the file
//
// 	Note that this implementation assumes there is no concurrent access
//	to the file system!
//
//	"name" -- name of file to be created
//	"initialSize" -- size of file to be created
//----------------------------------------------------------------------

bool FileSystem::Create(char *name, int initialSize) {
    DEBUG(dbgFile, "Create(" << name << ", " << initialSize << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    PersistentBitmap *freeMap;
    FileHeader *hdr;
    OpenFile *dirFile;
    char *token;
    int sector;
    bool success;

    DEBUG(dbgFile, "Creating file " << name << " size " << initialSize);

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (sector != -1)
        success = FALSE;  // file is already in directory
    else {
        freeMap = new PersistentBitmap(freeMapFile, NumSectors);
        sector = freeMap->FindAndSet();  // find a sector to hold the file header
        if (sector == -1)
            success = FALSE;  // no free block for file header
        else if (!directory->Add(token, sector))
            success = FALSE;  // no space in directory
        else {
            hdr = new FileHeader;
            if (!hdr->Allocate(freeMap, initialSize))
                success = FALSE;  // no space on disk for data
            else {
                success = TRUE;
                // everthing worked, flush all changes back to disk
                hdr->WriteBack(sector);
                directory->WriteBack(dirFile);
                freeMap->WriteBack(freeMapFile);
            }
            delete hdr;
        }
        delete freeMap;
    }

    if (dirFile != directoryFile)
        delete dirFile;
    delete directory;
    delete duplicate;
    return success;
}

// MP4 start
bool FileSystem::CreateDirectory(char *name) {
    DEBUG(dbgFile, "CreateDirectory(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    PersistentBitmap *freeMap;
    FileHeader *hdr;
    OpenFile *dirFile;
    char *token;
    int sector;
    bool success;

    DEBUG(dbgFile, "Creating Directory " << name);

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (sector != -1)
        success = FALSE;
    else {
        freeMap = new PersistentBitmap(freeMapFile, NumSectors);
        sector = freeMap->FindAndSet();
        if (sector == -1)
            success = FALSE;
        else if (!directory->AddDirectory(token, sector))
            success = FALSE;
        else {
            hdr = new FileHeader;
            if (!hdr->Allocate(freeMap, DirectoryFileSize))
                success = FALSE;
            else {
                success = TRUE;
                hdr->WriteBack(sector);

                OpenFile *newDirFile = new OpenFile(sector);
                Directory *newDir = new Directory(NumDirEntries);
                newDir->WriteBack(newDirFile);

                directory->WriteBack(dirFile);
                freeMap->WriteBack(freeMapFile);

                delete newDirFile;
                delete newDir;
            }
            delete hdr;
        }
        delete freeMap;
    }

    if (dirFile != directoryFile)
        delete dirFile;
    delete directory;
    delete duplicate;
    return success;
}
// MP4 end

//----------------------------------------------------------------------
// FileSystem::Open
// 	Open a file for reading and writing.
//	To open a file:
//	  Find the location of the file's header, using the directory
//	  Bring the header into memory
//
//	"name" -- the text name of the file to be opened
//----------------------------------------------------------------------

OpenFile *FileSystem::Open(char *name) {
    DEBUG(dbgFile, "Open(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    OpenFile *openFile = NULL;
    OpenFile *dirFile;
    char *token;
    int sector;

    DEBUG(dbgFile, "Opening file" << name);

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (sector >= 0)
        openFile = new OpenFile(sector);  // name was found in directory

    if (dirFile != directoryFile)
        delete dirFile;
    delete directory;
    delete duplicate;
    return openFile;  // return NULL if not found
}

// MP4 Start
OpenFileId FileSystem::OpenAFile(char *name) {
    OpenFileId id = 0;
    fileDescriptorTable[id] = Open(name);
    return id;
}

int FileSystem::WriteFile(char *buffer, int size, OpenFileId id) {
    if (id != 0)
        return -1;
    int retVal = fileDescriptorTable[id]->Write(buffer, size);
    if (retVal < 0)
        return -1;
    return retVal;
}

int FileSystem::ReadFile(char *buffer, int size, OpenFileId id) {
    if (id != 0)
        return -1;
    int retVal = fileDescriptorTable[id]->Read(buffer, size);
    if (retVal < 0)
        return -1;
    return retVal;
}

int FileSystem::CloseFile(OpenFileId id) {
    if (id != 0 || fileDescriptorTable[id] == NULL)
        return -1;
    delete fileDescriptorTable[id];
    fileDescriptorTable[id] = NULL;
    return 1;
}
// MP4 end

//----------------------------------------------------------------------
// FileSystem::Remove
// 	Delete a file from the file system.  This requires:
//	    Remove it from the directory
//	    Delete the space for its header
//	    Delete the space for its data blocks
//	    Write changes to directory, bitmap back to disk
//
//	Return TRUE if the file was deleted, FALSE if the file wasn't
//	in the file system.
//
//	"name" -- the text name of the file to be removed
//----------------------------------------------------------------------

// MP4 start
bool FileSystem::Remove(char *name) {
    DEBUG(dbgFile, "Remove(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    PersistentBitmap *freeMap;
    FileHeader *fileHdr;
    OpenFile *dirFile;
    char *token;
    int sector;

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (sector == -1) {
        delete directory;
        return FALSE;  // file not found
    }
    fileHdr = new FileHeader;
    fileHdr->FetchFrom(sector);

    freeMap = new PersistentBitmap(freeMapFile, NumSectors);

    fileHdr->Deallocate(freeMap);  // remove data blocks
    freeMap->Clear(sector);        // remove header block
    directory->Remove(token);

    freeMap->WriteBack(freeMapFile);  // flush to disk
    directory->WriteBack(dirFile);    // flush to disk

    if (dirFile != directoryFile)
        delete dirFile;
    delete fileHdr;
    delete directory;
    delete freeMap;
    delete duplicate;
    return TRUE;
}
// MP4 end

// MP4 start
bool FileSystem::RecursiveRemove(char *name) {
    DEBUG(dbgFile, "RecursiveRemove(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    PersistentBitmap *freeMap;
    FileHeader *fileHdr;
    OpenFile *dirFile;
    char *token;
    int sector;

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (sector == -1) {
        delete directory;
        return FALSE;  // file not found
    }

    freeMap = new PersistentBitmap(freeMapFile, NumSectors);

    if (directory->IsDir(token)) {
        OpenFile *subDirFile = new OpenFile(sector);
        Directory *subDir = new Directory(NumDirEntries);
        subDir->FetchFrom(subDirFile);

        subDir->RecursiveRemove(freeMap);
    }

    fileHdr = new FileHeader;
    fileHdr->FetchFrom(sector);

    fileHdr->Deallocate(freeMap);
    freeMap->Clear(sector);
    directory->Remove(token);

    freeMap->WriteBack(freeMapFile);
    directory->WriteBack(dirFile);

    if (dirFile != directoryFile)
        delete dirFile;
    delete fileHdr;
    delete freeMap;
    delete directory;
    delete duplicate;
    return TRUE;
}
// MP4 end

//----------------------------------------------------------------------
// FileSystem::List
// 	List all the files in the file system directory.
//----------------------------------------------------------------------

void FileSystem::List(char *name) {
    DEBUG(dbgFile, "List(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    OpenFile *dirFile;
    char *token;
    int sector;

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (token != NULL) {
        if (dirFile != directoryFile)
            delete dirFile;
        dirFile = new OpenFile(sector);
        directory->FetchFrom(dirFile);
    }

    directory->List();

    if (dirFile != directoryFile)
        delete dirFile;
    delete directory;
    delete duplicate;
}

// MP4 start
void FileSystem::RecursiveList(char *name) {
    DEBUG(dbgFile, "RecursiveList(" << name << ")");
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    OpenFile *dirFile;
    char *token;
    int sector;

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    if (token != NULL) {
        if (dirFile != directoryFile)
            delete dirFile;
        dirFile = new OpenFile(sector);
        directory->FetchFrom(dirFile);
    }

    directory->RecursiveList(0);

    if (dirFile != directoryFile)
        delete dirFile;
    delete directory;
    delete duplicate;
}
// MP4 end

//----------------------------------------------------------------------
// FileSystem::Print
// 	Print everything about the file system:
//	  the contents of the bitmap
//	  the contents of the directory
//	  for each file in the directory,
//	      the contents of the file header
//	      the data in the file
//----------------------------------------------------------------------

void FileSystem::Print() {
    FileHeader *bitHdr = new FileHeader;
    FileHeader *dirHdr = new FileHeader;
    PersistentBitmap *freeMap = new PersistentBitmap(freeMapFile, NumSectors);
    Directory *directory = new Directory(NumDirEntries);

    printf("Bit map file header:\n");
    bitHdr->FetchFrom(FreeMapSector);
    bitHdr->Print();

    printf("Directory file header:\n");
    dirHdr->FetchFrom(DirectorySector);
    dirHdr->Print();

    freeMap->Print();

    directory->FetchFrom(directoryFile);
    directory->Print();

    delete bitHdr;
    delete dirHdr;
    delete freeMap;
    delete directory;
}

// int FileSystem::GetHeaderSize() {
//     Directory *directory = new Directory(NumDirEntries);
//     directory->FetchFrom(directoryFile);
//     directory->();

//     delete directory;
// }

void FileSystem::PrintFileHdrSize(char *name) {
    char *duplicate = new char[256];
    strcpy(duplicate, name);

    Directory *directory;
    OpenFile *dirFile;
    FileHeader *hdr = new FileHeader;
    char *token;
    int sector;

    Parser(directoryFile, directory, dirFile, token, sector, duplicate);

    hdr->FetchFrom(sector);
    printf("Header Size: %d\n", hdr->GetHeaderSize());

    if (dirFile != directoryFile)
        delete dirFile;
    delete hdr;
    delete directory;
    delete duplicate;
}

#endif  // FILESYS_STUB
