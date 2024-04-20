// filename ************** eFile.c *****************************
// High-level routines to implement a solid-state disk
// Students implement these functions in Lab 4
// Jonathan W. Valvano 1/12/20
#include <stdint.h>
#include <string.h>
#include "../common/OS.h"
#include "../common/eDisk.h"
#include "../common/eFile.h"
#include <stdio.h>

// Synchronize filesystem with display since they both use the same port
extern Sema4Type LCDFree;
#define ACQUIRE_SEMA()      \
    {                       \
        OS_bWait(&LCDFree); \
    }
#define RELEASE_SEMA_AND_RETURN(x) \
    {                              \
        OS_bSignal(&LCDFree);      \
        return x;                  \
    }

#define TABLE_BLOCKS 8

enum file_identifier
{
    UNUSED,
    IS_FILE,
    IS_DIR
};

// im not sure if we need to * by 8 - rv
// also need to set entry to some "busy value" - rv
//  (256*8) blocks * 512 bytes/block = 1MiB
struct file_alloc_table
{
    uint16_t next[256 * TABLE_BLOCKS];
};
typedef struct file_alloc_table file_alloc_table;

file_alloc_table allocation_table;

// 16 bytes
struct dir_entry
{
    uint32_t bytes; // persistent storage of file size
    uint16_t start;
    uint8_t type; // @ref enum file_identifier
    char name[9];
};
typedef struct dir_entry dir_entry;

// 512 bytes = 1 block
struct directory
{
    dir_entry self;
    dir_entry parent;
    dir_entry entries[29];
    uint8_t size;
    uint8_t unused[15];
};
typedef struct directory directory_t;

struct file
{
    uint32_t file_idx;
    uint32_t bytes;
    uint16_t start_block;
    uint16_t curr_block;
    uint8_t buffer[512];
};
typedef struct file file_t;

file_t read_file, write_file;

const uint16_t allocation_table_block = 0;
const uint16_t root_block = TABLE_BLOCKS;

uint8_t dir_open = 0;
uint8_t dir_idx = 0;
directory_t opened_dir;
uint32_t curr_dir_block;
directory_t dir;

uint16_t get_free_block()
{
    uint16_t block;
    // if we change the size of FAT table then we may need to adjust the loop bounds
    for (block = (TABLE_BLOCKS + 1); block < (256 * TABLE_BLOCKS); block++)
    {
        if (allocation_table.next[block] == 0)
        {
            return block;
        }
    }
    return 0xFFFF; // invalid pointer since it is out of the range of blocks
}

//---------- eFile_Init-----------------
// Activate the file system, without formating
// Input: none
// Output: 0 if successful and 1 on failure (already initialized)
int initstatus = 0;
int eFile_Init(void)
{ // initialize file system
    // TODO:
    // 1) Check if already initialized
    // 2) Read file_alloc_table from disk
    // 3) Initialize vars using information from (2)
    // Note: in this and other comments we need to handle synchronization
    // (probably we should use same semaphore as LCD)
    ACQUIRE_SEMA();

    if (initstatus)
        RELEASE_SEMA_AND_RETURN(1);

    int result = eDisk_Init(0);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    result = eDisk_Read(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS); // read the file alloc table
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    memset(&read_file, 0, sizeof(file_t));
    memset(&write_file, 0, sizeof(file_t));
    curr_dir_block = root_block;

    initstatus = 1;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_Format-----------------
// Erase all files, create blank directory, initialize free space manager
// Input: none
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Format(void)
{ // erase disk, add format
    // TODO:
    // 1) Clear local file_alloc_table and write it to disk
    // 2) Create new root directory and write it to disk
    // Note: all the write backs could be done in eFile_UnMount
    ACQUIRE_SEMA();

    int result;

    // Clear file_alloc_table and write to disk
    memset(&allocation_table, 0, sizeof(file_alloc_table));
    result = eDisk_Write(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    // Create new root directory
    memset(&dir, 0, sizeof(directory_t));
    strcpy(dir.self.name, ".");
    strcpy(dir.parent.name, "..");
    dir.self.start = root_block;
    dir.self.type = IS_DIR;
    dir.parent.start = root_block;
    dir.parent.type = IS_DIR;

    result = eDisk_WriteBlock((uint8_t *)&dir, root_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    curr_dir_block = root_block;
    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_Mount-----------------
// Mount the file system, without formating
// Input: none
// Output: 0 if successful and 1 on failure
int eFile_Mount(void)
{ // initialize file system
    // Not sure whether this is needed, shouldn't this already be done in eFile_Init?
    // In Lab4.c, it is called in the following order: eFile_Init->eFile_Format->eFile_Mount
    // I would assume that to format the disk we would need to have mounted it (read it in already),
    // otherwise the format would do nothing since we would just be reading in from the disk again
    return 0;
}

//---------- eFile_Create-----------------
// Create a new, empty file with one allocated block
// Input: file name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Create(const char name[])
{ // create new file, make it empty
    // TODO:
    // 1) Read directory from disk (if not read before -- we should cache the current directory)
    // 2) Check if file name exists -- no duplicate files
    // 3) Find next available block in directory (can be done while doing (2))
    // 4) Initialize empty file in the available block
    // 5) Write back changes to disk
    ACQUIRE_SEMA();

    int result;
    // read current directory from disk
    result = eDisk_ReadBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    if (dir.size == 29)
        RELEASE_SEMA_AND_RETURN(1);

    for (uint8_t i = 0; i < dir.size; i++)
    {
        if (dir.entries[i].type == IS_FILE)
        {
            if (strcmp(dir.entries[i].name, name) == 0)
            { // don't allow duplicate names
                RELEASE_SEMA_AND_RETURN(1);
            }
        }
    }

    uint16_t block = get_free_block();
    if (block == 0xFFFF)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }
    // Update next pointer to be invalid / EOF
    allocation_table.next[block] = 0xFFFF;

    dir.entries[dir.size].type = IS_FILE;
    dir.entries[dir.size].start = block;
    dir.entries[dir.size].bytes = 0;
    strcpy(dir.entries[dir.size].name, name);
    dir.size++;

    result = eDisk_WriteBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    result = eDisk_Write(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_DirCreate-----------------
// Create a new, empty directory with one allocated block
// Input: directory name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_DirCreate(const char name[])
{ // create new directory, make it empty
    // if we get to implementing sub-directories (similar to eFile_Create)
    return 1; // replace
}

//---------- eFile_WOpen-----------------
// Open the file, read into RAM last block
// Input: file name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_WOpen(const char name[])
{ // open a file for writing
    // Only allow one file opened at a time for writing, need to store currently opened writing file
    // 1) Check if another file that is being written to is opened
    // 2) Search for file name in directory
    // 3) Store write file into some cached structure to be used in eFile_Write (load latest block in file)
    ACQUIRE_SEMA();

    int result;

    if (write_file.start_block != 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    result = eDisk_ReadBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    uint8_t i;
    for (i = 0; i < dir.size; i++)
    {
        if (dir.entries[i].type == IS_FILE)
        {
            if (strcmp(dir.entries[i].name, name) == 0)
            { // compare name
                break;
            }
        }
    }

    if (i == dir.size)
    {
        // file not found
        RELEASE_SEMA_AND_RETURN(1);
    }

    uint16_t block = dir.entries[i].start;
    while (allocation_table.next[block] != 0xFFFF)
    {
        block = allocation_table.next[block];
    }

    result = eDisk_ReadBlock(write_file.buffer, block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    write_file.start_block = dir.entries[i].start;
    write_file.curr_block = block;
    write_file.file_idx = dir.entries[i].bytes;
    write_file.bytes = dir.entries[i].bytes;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_Write-----------------
// save at end of the open file
// Input: data to be saved
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Write(const char data)
{
    // 1) Try to write data to latest block
    // 2) If we are at the end of the block we need to write it back and get a new block
    ACQUIRE_SEMA();

    int result;

    if (write_file.start_block == 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    if (write_file.file_idx != 0 && write_file.file_idx % 512 == 0)
    {
        uint16_t block = get_free_block();

        if (block == 0xFFFF)
        {
            RELEASE_SEMA_AND_RETURN(1);
        }

        allocation_table.next[write_file.curr_block] = block;
        allocation_table.next[block] = 0xFFFF;

        result = eDisk_WriteBlock(write_file.buffer, write_file.curr_block);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);

        // can probably optimize the following by only writing the modified block(s)
        result = eDisk_Write(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);

        write_file.curr_block = block;
    }

    write_file.buffer[(write_file.file_idx++) % 512] = data;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_WClose-----------------
// close the file, left disk in a state power can be removed
// Input: none
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_WClose(void)
{ // close the file for writing
    // 1) Write back the latest block of the file that is opened
    // 2) Allow future calls to eFile_WOpen to open a new file
    ACQUIRE_SEMA();

    int result;

    if (write_file.start_block == 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    result = eDisk_ReadBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    uint8_t i;
    for (i = 0; i < dir.size; i++)
    {
        if (dir.entries[i].type == IS_FILE)
        {
            if (dir.entries[i].start == write_file.start_block)
            { // find file
                break;
            }
        }
    }

    if (i == dir.size)
    {
        // file not found
        RELEASE_SEMA_AND_RETURN(1);
    }

    dir.entries[i].bytes = write_file.file_idx;

    result = eDisk_WriteBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    result = eDisk_WriteBlock(write_file.buffer, write_file.curr_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    write_file.start_block = 0;
    write_file.curr_block = 0;
    write_file.file_idx = 0;
    write_file.bytes = 0;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_ROpen-----------------
// Open the file, read first block into RAM
// Input: file name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble read to flash)
int eFile_ROpen(const char name[])
{ // open a file for reading
    // Similar to eFile_WOpen, however we need to make sure the block we
    // are reading from does not have the same index as a block we are writing to
    ACQUIRE_SEMA();

    int result;

    if (read_file.start_block != 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    result = eDisk_ReadBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    uint8_t i;
    for (i = 0; i < dir.size; i++)
    {
        if (dir.entries[i].type == IS_FILE)
        {
            if (strcmp(dir.entries[i].name, name) == 0)
            { // compare name
                break;
            }
        }
    }

    if (i == dir.size)
    {
        // file not found
        RELEASE_SEMA_AND_RETURN(1);
    }

    result = eDisk_ReadBlock(read_file.buffer, dir.entries[i].start);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    read_file.start_block = dir.entries[i].start;
    read_file.curr_block = dir.entries[i].start;
    read_file.file_idx = 0;
    read_file.bytes = dir.entries[i].bytes;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_ReadNext-----------------
// retreive data from open file
// Input: none
// Output: return by reference data
//         0 if successful and 1 on failure (e.g., end of file)
int eFile_ReadNext(char *pt)
{ // get next byte
    // 1) Read byte from open file and increment pointer
    // 2) If we are at the end of the block we need read the next block
    // 3) Need to handle EOF -- if next block has an invalid index
    ACQUIRE_SEMA();

    int result;

    if (read_file.start_block == 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    if (read_file.file_idx == read_file.bytes)
    {
        RELEASE_SEMA_AND_RETURN(1); // EOF
    }

    if (read_file.file_idx != 0 && read_file.file_idx % 512 == 0)
    {
        uint16_t block = allocation_table.next[read_file.curr_block];
        if (block == 0xFFFF)
        {
            RELEASE_SEMA_AND_RETURN(1); // EOF
        }

        result = eDisk_ReadBlock(read_file.buffer, block);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);

        read_file.curr_block = block;
    }

    *pt = read_file.buffer[(read_file.file_idx++) % 512];

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_RClose-----------------
// close the reading file
// Input: none
// Output: 0 if successful and 1 on failure (e.g., wasn't open)
int eFile_RClose(void)
{ // close the file for writing
    // Similar to eFile_WClose but no write back
    ACQUIRE_SEMA();

    if (read_file.start_block == 0)
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    read_file.start_block = 0;
    read_file.curr_block = 0;
    read_file.file_idx = 0;
    read_file.bytes = 0;

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_Delete-----------------
// delete this file
// Input: file name is a single ASCII letter
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Delete(const char name[])
{ // remove this file
    // 1) Find the file with the name in the current directory
    // 2) Remove it from the directory and write directory back
    // 3) Remove it from the file_alloc_table and write table back
    ACQUIRE_SEMA();

    int result;

    result = eDisk_ReadBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    uint8_t i;
    for (i = 0; i < dir.size; i++)
    {
        if (dir.entries[i].type == IS_FILE)
        {
            if (strcmp(dir.entries[i].name, name) == 0)
            { // compare name
                break;
            }
        }
    }

    if (i == dir.size)
    {
        // file not found
        RELEASE_SEMA_AND_RETURN(1);
    }

    uint16_t block = dir.entries[i].start;
    dir.entries[i].type = UNUSED;
    uint16_t next;
    while (block != 0xFFFF)
    {
        next = allocation_table.next[block];
        allocation_table.next[block] = 0;
        block = next;
    }

    // moves last element in directory to fill the removed space
    dir.entries[i] = dir.entries[--dir.size];

    result = eDisk_WriteBlock((uint8_t *)&dir, curr_dir_block);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    result = eDisk_Write(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_DOpen-----------------
// Open a (sub)directory, read into RAM
// Input: directory name is an ASCII string up to seven characters
//        (empty/NULL for root directory)
// Output: 0 if successful and 1 on failure (e.g., trouble reading from flash)
int eFile_DOpen(const char name[])
{ // open directory
    // Switch current directory (cached) to the opened dir if it is found
    // All future file operations should be on that directory
    ACQUIRE_SEMA();

    int result;

    if (dir_open)
        RELEASE_SEMA_AND_RETURN(1);

    if (strcmp(name, "") == 0)
    {
        result = eDisk_ReadBlock((uint8_t *)&opened_dir, root_block);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);
    }
    else
    {
        result = eDisk_ReadBlock((uint8_t *)&opened_dir, curr_dir_block);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);

        uint8_t i;
        for (i = 0; i < opened_dir.size; i++)
        {
            if (opened_dir.entries[i].type == IS_DIR)
            {
                if (strcmp(opened_dir.entries[i].name, name) == 0)
                { // compare name
                    break;
                }
            }
        }

        if (i == opened_dir.size)
        {
            // dir not found
            RELEASE_SEMA_AND_RETURN(1);
        }

        result = eDisk_ReadBlock((uint8_t *)&opened_dir, opened_dir.entries[i].start);
        if (result)
            RELEASE_SEMA_AND_RETURN(result);
    }

    dir_idx = 0;
    dir_open = 1;
    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_DirNext-----------------
// Retreive directory entry from open directory
// Input: none
// Output: return file name and size by reference
//         0 if successful and 1 on failure (e.g., end of directory)
int eFile_DirNext(char *name[], unsigned long *size)
{ // get next entry
    // Need to store a pointer for the current entry, increment and return the next entry information
    ACQUIRE_SEMA();

    if (!dir_open)
        RELEASE_SEMA_AND_RETURN(1);

    if (dir_idx < opened_dir.size)
    {
        *name = opened_dir.entries[dir_idx].name;
        *size = opened_dir.entries[dir_idx].bytes;

        dir_idx++;
    }
    else
    {
        RELEASE_SEMA_AND_RETURN(1);
    }

    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_DClose-----------------
// Close the directory
// Input: none
// Output: 0 if successful and 1 on failure (e.g., wasn't open)
int eFile_DClose(void)
{ // close the directory
    ACQUIRE_SEMA();

    if (!dir_open)
        RELEASE_SEMA_AND_RETURN(1);
    dir_open = 0;
    RELEASE_SEMA_AND_RETURN(0);
}

//---------- eFile_Unmount-----------------
// Unmount and deactivate the file system
// Input: none
// Output: 0 if successful and 1 on failure (not currently mounted)
int eFile_Unmount(void)
{
    // Write back any potentially unwritten data
    ACQUIRE_SEMA();

    int result = eDisk_Write(0, (uint8_t *)&allocation_table, allocation_table_block, TABLE_BLOCKS);
    if (result)
        RELEASE_SEMA_AND_RETURN(result);
    initstatus = 0;

    RELEASE_SEMA_AND_RETURN(0);
}
