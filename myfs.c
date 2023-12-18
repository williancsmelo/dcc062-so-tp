/*
 *  myfs.c - Implementacao do sistema de arquivos MyFS
 *
 *  Autores: SUPER_PROGRAMADORES_C
 *  Projeto: Trabalho Pratico II - Sistemas Operacionais
 *  Organizacao: Universidade Federal de Juiz de Fora
 *  Departamento: Dep. Ciencia da Computacao
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

// Declaracoes globais
#define MAX_PATH_LENGTH 1000
#define NUMBLOCKS_PERINODE 8

#define ROOT_INODE_NUMBER 1
Inode *inodeRoot = NULL;
int loadRootInode(Disk *d)
{
	if (inodeRoot != NULL)
		return 0;
	inodeRoot = inodeLoad(ROOT_INODE_NUMBER, d);
	if (inodeRoot == NULL)
		return -1;
	return 0;
}

// superbloco
#define SUPERBLOCK_SECTOR 0
#define SUPERBLOCK_SIZE 4 // Tamanho do superbloco em numero de unsigned ints
#define SUPERBLOCK_ITEM_BLOCKSIZE 0
#define SUPERBLOCK_ITEM_NUMBLOCKS 1
#define SUPERBLOCK_ITEM_NUMINODES 2
#define SUPERBLOCK_ITEM_BITMAPBLOCK 3
unsigned int *superblock = NULL;

// bitmap
#define BITMAP_SECTOR 1
unsigned char *bitmap = NULL; // tamanho = numero de blocos | 1 = ocupado, 0 = livre

#define MAX_OPEN_FILES MAX_FDS
typedef struct fileDescriptor
{
	unsigned int fd;
	Inode *inode;
	Disk *disk;
	unsigned int cursor;
} FileDescriptor;
FileDescriptor *openFiles[MAX_OPEN_FILES];
unsigned int numOpenFiles = 0;
unsigned int lastFd = 0;

unsigned int inodeGetLastBlockAddr(Inode *inode)
{
	unsigned int totalSize = inodeGetFileSize(inode);
	unsigned int lastBlock = totalSize / superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	if (totalSize % superblock[SUPERBLOCK_ITEM_BLOCKSIZE] == 0)
		lastBlock--;
	return inodeGetBlockAddr(inode, lastBlock);
}

int writeBlock(Disk *d, unsigned int block, const char *buf, unsigned int size)
{
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned int firstSector = block * sectorPerBlock;
	if (buf == NULL || firstSector < inodeAreaBeginSector())
		return -1;
	if (size > superblock[SUPERBLOCK_ITEM_BLOCKSIZE])
		size = superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	for (unsigned int i = 0; i < sectorPerBlock; i++)
	{
		unsigned int sizeToWrite = size > DISK_SECTORDATASIZE ? DISK_SECTORDATASIZE : size;
		unsigned char sector[DISK_SECTORDATASIZE];
		memcpy(sector, &buf[i * DISK_SECTORDATASIZE], sizeToWrite);
		if (diskWriteSector(d, firstSector + i, sector) == -1)
			return -1;
		size -= sizeToWrite;
	}
	return 0;
}

int readBlock(Disk *d, unsigned int block, char *buf)
{
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned int firstSector = block * sectorPerBlock;
	if (buf == NULL || firstSector < inodeAreaBeginSector())
		return -1;
	for (unsigned int i = 0; i < sectorPerBlock; i++)
	{
		unsigned char sector[DISK_SECTORDATASIZE];
		if (diskReadSector(d, firstSector + i, sector) == -1)
			return -1;
		memcpy(&buf[i * DISK_SECTORDATASIZE], sector, DISK_SECTORDATASIZE);
	}
	return 0;
}

// Funções do superbloco
int saveSuperblock(Disk *d)
{
	unsigned char sector[DISK_SECTORDATASIZE];
	for (int a = 0; a < SUPERBLOCK_SIZE; a++)
		ul2char(superblock[a], &sector[a * sizeof(unsigned int)]);
	return diskWriteSector(d, SUPERBLOCK_SECTOR, sector);
}

int loadSuperblock(Disk *d)
{
	if (superblock != NULL)
		return 0;
	unsigned char sector[DISK_SECTORDATASIZE];
	if (diskReadSector(d, SUPERBLOCK_SECTOR, sector) == -1)
		return -1;
	superblock = malloc(SUPERBLOCK_SIZE * sizeof(unsigned int));
	if (superblock == NULL)
		return -1;
	for (int a = 0; a < SUPERBLOCK_SIZE; a++)
		char2ul(&sector[a * sizeof(unsigned int)], &(superblock[a]));
	return 0;
}

// funções do bitmap
int loadBitmap(Disk *d)
{
	if (bitmap != NULL)
		return 0;
	bitmap = calloc(superblock[SUPERBLOCK_ITEM_NUMBLOCKS], sizeof(unsigned char));
	unsigned char *buffer = malloc(superblock[SUPERBLOCK_ITEM_BLOCKSIZE] * sizeof(unsigned char));
	if (bitmap == NULL || buffer == NULL)
		return -1;
	int response = readBlock(d, superblock[SUPERBLOCK_ITEM_BITMAPBLOCK], buffer);
	memcpy(bitmap, buffer, superblock[SUPERBLOCK_ITEM_NUMBLOCKS] * sizeof(unsigned char));
	free(buffer);
	return response;
}

int saveBitmap(Disk *d)
{
	if (bitmap == NULL)
		return -1;
	return writeBlock(d, superblock[SUPERBLOCK_ITEM_BITMAPBLOCK], bitmap, superblock[SUPERBLOCK_ITEM_NUMBLOCKS] * sizeof(unsigned char));
}

int findFreeBlocks(unsigned int numBlocks, unsigned int *blocks)
{
	if (numBlocks <= 0)
		return 0;
	unsigned int freeBlocks = 0;
	for (unsigned int i = 0; i < superblock[SUPERBLOCK_ITEM_NUMBLOCKS]; i++)
	{
		if (bitmap[i] == 0)
		{
			blocks[freeBlocks] = i;
			freeBlocks++;
			if (freeBlocks == numBlocks)
				return 0;
		}
	}
	return -1;
}

int setBlocksStatus(unsigned int numBlocks, unsigned int *blocks, char status)
{
	if (status != 0 && status != 1)
		return -1;
	for (unsigned int i = 0; i < numBlocks; i++)
		bitmap[blocks[i]] = status;
	return 0;
}

// funções do diretório
typedef struct directoryEntry
{
	char name[MAX_FILENAME_LENGTH];
	unsigned int inodeNumber;
} DirectoryEntry;

typedef struct directory
{
	DirectoryEntry **entries;
	unsigned int numEntries;
} Directory;

void freeDirectory(Directory *dir)
{
	if (dir == NULL)
		return;
	if (dir->entries != NULL)
		for (unsigned int i = 0; i < dir->numEntries; i++)
			free(dir->entries[i]);
	free(dir->entries);
	free(dir);
}

Directory *loadDirectory(Disk *d, Inode *inode)
{
	if (inodeGetFileType(inode) != FILETYPE_DIR)
		return NULL;
	Directory *responseDir;
	Directory *dir = malloc(sizeof(Directory));
	if (dir == NULL)
		return NULL;
	unsigned int numBlocks = ceil((float)inodeGetFileSize(inode) / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned char *buffer = malloc(numBlocks * superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int bufferOffset = 0;
	for (unsigned int i = 0; i < numBlocks; i++)
	{
		if (readBlock(d, inodeGetBlockAddr(inode, i), &buffer[bufferOffset]) == -1)
			break;
		bufferOffset += superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	}
	if (bufferOffset == numBlocks * superblock[SUPERBLOCK_ITEM_BLOCKSIZE])
	{
		bufferOffset = 0;
		char2ul(buffer, &(dir->numEntries));
		bufferOffset += sizeof(unsigned int);
		if (dir->numEntries > 0)
			dir->entries = malloc(dir->numEntries * sizeof(DirectoryEntry *));
		else
		{
			dir->entries = NULL;
			responseDir = dir; // response dir != NULL > SUCCESS
		}
		if (dir->entries != NULL)
		{
			for (unsigned int i = 0; i < dir->numEntries; i++)
			{
				dir->entries[i] = malloc(sizeof(DirectoryEntry));
				if (dir->entries[i] == NULL)
					break;
				char2ul(&buffer[bufferOffset], &(dir->entries[i]->inodeNumber));
				bufferOffset += sizeof(unsigned int);
				memcpy(dir->entries[i]->name, &buffer[bufferOffset], MAX_FILENAME_LENGTH * sizeof(char));
				bufferOffset += MAX_FILENAME_LENGTH * sizeof(char);
			}

			if (bufferOffset == inodeGetFileSize(inode))
				responseDir = dir;
		}
	}

	free(buffer);
	if (responseDir == NULL)
		freeDirectory(dir);
	return responseDir;
}

int setDirNumEntries(Disk *d, unsigned int firstBlock, unsigned int numEntries)
{
	unsigned int firstSector = firstBlock * superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned char sector[DISK_SECTORDATASIZE];
	if (diskReadSector(d, firstSector, sector) == -1)
		return -1;
	ul2char(numEntries, sector);
	if (diskWriteSector(d, firstSector, sector) == -1)
		return -1;
}

int addDirectoryEntry(Disk *d, Inode *inodeDir, Inode *inodeEntry, const char *entryName)
{
	Directory *dir = loadDirectory(d, inodeDir);
	if (dir == NULL)
		return -1;
	for (unsigned int i = 0; i < dir->numEntries; i++)
	{
		if (strcmp(dir->entries[i]->name, entryName) == 0)
		{
			freeDirectory(dir);
			return -1;
		}
	}

	DirectoryEntry *entry = malloc(sizeof(DirectoryEntry));
	if (entry == NULL)
	{
		freeDirectory(dir);
		return -1;
	}
	entry->inodeNumber = inodeGetNumber(inodeEntry);
	strncpy(entry->name, entryName, MAX_FILENAME_LENGTH);

	const size_t entrySize = sizeof(unsigned int) + MAX_FILENAME_LENGTH * sizeof(char);
	unsigned char toSaveBuffer[entrySize];
	ul2char(entry->inodeNumber, toSaveBuffer);
	memcpy(&toSaveBuffer[sizeof(unsigned int)], entry->name, MAX_FILENAME_LENGTH * sizeof(char));
	free(entry);

	unsigned int finalBlock = inodeGetLastBlockAddr(inodeDir);
	unsigned int finalBlockOffset = inodeGetFileSize(inodeDir) % superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	unsigned int finalBlockFreeSpace = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] - finalBlockOffset;
	unsigned char *finalBlockBuffer = malloc(superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);

	if (readBlock(d, finalBlock, finalBlockBuffer) == -1)
	{
		freeDirectory(dir);
		free(finalBlockBuffer);
		return -1;
	}

	unsigned int newBlock = 0;
	unsigned char *newBlockBuffer = NULL;
	if (finalBlockFreeSpace >= entrySize)
		memcpy(&finalBlockBuffer[finalBlockOffset], toSaveBuffer, entrySize);
	else
	{
		unsigned int blocks[1];
		if (findFreeBlocks(1, blocks) == -1)
		{
			freeDirectory(dir);
			free(finalBlockBuffer);
			return -1;
		}
		newBlock = blocks[0];
		newBlockBuffer = malloc(superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
		for (unsigned int i = 0; i < entrySize; i++)
		{
			if (finalBlockOffset >= superblock[SUPERBLOCK_ITEM_BLOCKSIZE])
				newBlockBuffer[finalBlockOffset - superblock[SUPERBLOCK_ITEM_BLOCKSIZE]] = toSaveBuffer[i];
			else
				finalBlockBuffer[finalBlockOffset] = toSaveBuffer[i];
			finalBlockOffset++;
		}
	}

	if (writeBlock(d, finalBlock, finalBlockBuffer, superblock[SUPERBLOCK_ITEM_BLOCKSIZE]) == -1)
	{
		freeDirectory(dir);
		free(finalBlockBuffer);
		free(newBlockBuffer);
		return -1;
	}
	if (newBlockBuffer != NULL && writeBlock(d, newBlock, newBlockBuffer, superblock[SUPERBLOCK_ITEM_BLOCKSIZE]) == -1)
	{
		freeDirectory(dir);
		free(finalBlockBuffer);
		free(newBlockBuffer);
		return -1;
	}

	if (newBlock != 0)
		inodeAddBlock(inodeDir, newBlock);
	inodeSetFileSize(inodeDir, inodeGetFileSize(inodeDir) + entrySize);
	inodeSetRefCount(inodeEntry, inodeGetRefCount(inodeEntry) + 1);

	free(newBlockBuffer);
	free(finalBlockBuffer);
	unsigned int newNumEntries = dir->numEntries + 1;
	freeDirectory(dir);

	if (inodeSave(inodeEntry) == -1 || inodeSave(inodeDir) == -1 || saveBitmap(d) == -1)
		return -1;
	return setDirNumEntries(d, inodeGetBlockAddr(inodeDir, 0), newNumEntries);
}

int createDirectory(Disk *d, Inode *inode)
{
	unsigned int numEntries = 0;
	unsigned int blocks[1];
	if (findFreeBlocks(1, blocks) == 0)
	{
		unsigned char buf[sizeof(unsigned int)];
		ul2char(numEntries, buf);
		if (writeBlock(d, blocks[0], buf, sizeof(unsigned int)) == 0)
		{
			inodeSetFileSize(inode, sizeof(unsigned int));
			inodeSetFileType(inode, FILETYPE_DIR);
			inodeSetGroupOwner(inode, 0);
			inodeSetOwner(inode, 0);
			inodeSetPermission(inode, 0);
			inodeAddBlock(inode, blocks[0]);
			setBlocksStatus(1, blocks, 1);
			saveBitmap(d);
			return addDirectoryEntry(d, inode, inode, ".");
		}
	}
	return -1;
}

unsigned int findInodeNumber(Directory *dir, const char *name)
{
	for (unsigned int i = 0; i < dir->numEntries; i++)
	{
		if (strcmp(dir->entries[i]->name, name) == 0)
			return dir->entries[i]->inodeNumber;
	}

	return 0;
}

// funções open file
FileDescriptor *createFileDescriptor(Disk *d, Inode *inode)
{
	if (numOpenFiles >= MAX_OPEN_FILES)
		return NULL;
	FileDescriptor *openFile = malloc(sizeof(FileDescriptor));
	if (openFile == NULL)
		return NULL;
	openFile->fd = ++lastFd;
	openFile->inode = inode;
	openFile->disk = d;
	openFile->cursor = 0;
	openFiles[numOpenFiles++] = openFile;
	return openFile;
}

FileDescriptor *getFileDescriptor(unsigned int fd)
{
	if (numOpenFiles == 0)
		return NULL;
	for (unsigned int i = 0; i < numOpenFiles; i++)
	{
		if (openFiles[i]->fd == fd)
			return openFiles[i];
	}

	return NULL;
}

int loadFSData(Disk *d)
{
	if (d == NULL)
		return -1;
	if (loadSuperblock(d) == -1)
		return -1;
	if (loadBitmap(d) == -1)
		return -1;
	if (loadRootInode(d) == -1)
		return -1;
	return 0;
}

// Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
// se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
// um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle(Disk *d)
{
	if (numOpenFiles > 0)
		return 0;
	return 1;
}

// Funcao para formatacao de um disco com o novo sistema de arquivos
// com tamanho de blocos igual a blockSize. Retorna o numero total de
// blocos disponiveis no disco, se formatado com sucesso. Caso contrario,
// retorna -1.
int myFSFormat(Disk *d, unsigned int blockSize)
{
	// Verificação de parâmetros inválidos
	if (d == NULL || blockSize == 0)
		return -1;

	// criar superbloco
	superblock = malloc(SUPERBLOCK_SIZE * sizeof(unsigned int));
	superblock[SUPERBLOCK_ITEM_BLOCKSIZE] = blockSize;
	superblock[SUPERBLOCK_ITEM_NUMBLOCKS] = diskGetSize(d) / blockSize;
	superblock[SUPERBLOCK_ITEM_NUMINODES] = superblock[SUPERBLOCK_ITEM_NUMBLOCKS] / NUMBLOCKS_PERINODE;

	// Inicializar i-nodes
	for (int i = 1; i < superblock[SUPERBLOCK_ITEM_NUMINODES] + 1; i++)
	{
		Inode *inode = inodeCreate(i, d);
		if (inode == NULL)
			return -1;

		if (i == ROOT_INODE_NUMBER)
			inodeRoot = inode;
		else
			free(inode);
	}

	// criar bitmap
	bitmap = calloc(superblock[SUPERBLOCK_ITEM_NUMBLOCKS], sizeof(unsigned char));
	unsigned int inodesSectors = superblock[SUPERBLOCK_ITEM_NUMINODES] / inodeNumInodesPerSector();
	unsigned int inodesBlocks = ceil((float)(inodesSectors + inodeAreaBeginSector()) * DISK_SECTORDATASIZE / blockSize);
	for (int i = 0; i < inodesBlocks + 1; i++)
		bitmap[i] = 1;
	superblock[SUPERBLOCK_ITEM_BITMAPBLOCK] = inodesBlocks;
	if (saveSuperblock(d) == -1)
		return -1;
	if (saveBitmap(d) == -1)
		return -1;

	// create root
	if (createDirectory(d, inodeRoot) == -1)
		return -1;
	if (addDirectoryEntry(d, inodeRoot, inodeRoot, "..") == -1)
		return -1;
	return superblock[SUPERBLOCK_ITEM_NUMBLOCKS];
}

int splitPath(const char *path, char ***entries)
{
	if (path == NULL || path[0] == '\0')
	{
		*entries = NULL;
		return 0;
	}

	char **result = malloc(MAX_PATH_LENGTH * sizeof(char *));
	char *pathCopy = strdup(path);
	if (result == NULL || pathCopy == NULL)
		return -1;

	int count = 0;
	char *token = strtok(pathCopy, "/");
	while (token != NULL)
	{
		if (strcmp(token, ".") != 0 && strcmp(token, "..") != 0)
			result[count++] = strdup(token);
		token = strtok(NULL, "/");
	}
	free(pathCopy);
	result = (char **)realloc(result, count * sizeof(char *));
	if (result == NULL)
		return -1;
	*entries = result;
	return count;
}

// Funcao para abertura de um arquivo, a partir do caminho especificado
// em path, no disco montado especificado em d, no modo Read/Write,
// criando o arquivo se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen(Disk *d, const char *path)
{
	if (loadFSData(d) == -1)
		return -1;
	char **entries = NULL;
	int numEntries = splitPath(path, &entries);

	if (numEntries == -1)
		return -1;

	Inode *inodeDir = inodeRoot;
	Directory *dir = loadDirectory(d, inodeRoot);
	for (int i = 0; i < numEntries - 1; i++)
	{
		unsigned int inodeNumber = findInodeNumber(dir, entries[i]);
		if (inodeNumber == 0)
			break;
		if (inodeDir != inodeRoot)
			free(inodeDir);
		inodeDir = inodeLoad(inodeNumber, d);
		if (inodeDir == NULL || inodeGetFileType(inodeDir) != FILETYPE_DIR)
			break;
		freeDirectory(dir);
		dir = loadDirectory(d, inodeDir);
		if (dir == NULL)
			break;
	}
	Inode *inodeFile;

	if (dir != NULL)
	{
		unsigned int inodeNumber = findInodeNumber(dir, entries[numEntries - 1]);
		freeDirectory(dir);
		if (inodeNumber != 0)
			inodeFile = inodeLoad(inodeNumber, d);
		else
		{
			inodeNumber = inodeFindFreeInode(ROOT_INODE_NUMBER + 1, d);
			if (inodeNumber != 0)
			{
				inodeFile = inodeLoad(inodeNumber, d);
				if (inodeFile != NULL)
				{
					inodeSetFileType(inodeFile, FILETYPE_REGULAR);
					inodeSetFileSize(inodeFile, 0);
					inodeSetGroupOwner(inodeFile, 0);
					inodeSetOwner(inodeFile, 0);
					inodeSetPermission(inodeFile, 0);
					unsigned int blocks[1];
					if (findFreeBlocks(1, blocks) == -1)
					{
						free(inodeFile);
						inodeFile = NULL;
					}
					else
					{
						inodeAddBlock(inodeFile, blocks[0]);
						setBlocksStatus(1, blocks, 1);
						saveBitmap(d);
					}
					if (inodeFile != NULL && addDirectoryEntry(d, inodeDir, inodeFile, entries[numEntries - 1]) == -1)
					{
						free(inodeFile);
						inodeFile = NULL;
					}
				}
			}
		}
	}

	unsigned int fd = 0;
	if (inodeFile != NULL)
	{
		for (unsigned int i = 0; i < numOpenFiles; i++)
		{
			if (openFiles[i]->inode == inodeFile)
			{
				fd = openFiles[i]->fd;
				break;
			}
		}
		if (fd == 0)
		{
			FileDescriptor *newFileDescriptor = createFileDescriptor(d, inodeFile);
			if (newFileDescriptor != NULL)
				fd = newFileDescriptor->fd;
		}
	}

	if (entries != NULL)
	{
		for (int i = 0; i < numEntries; i++)
			free(entries[i]);
		free(entries);
	}
	if (inodeDir != NULL && inodeDir != inodeRoot)
		free(inodeDir);

	return fd;
}

// Funcao para a leitura de um arquivo, a partir de um descritor de
// arquivo existente. Os dados lidos sao copiados para buf e terao
// tamanho maximo de nbytes. Retorna o numero de bytes efetivamente
// lidos em caso de sucesso ou -1, caso contrario.
int myFSRead(int fd, char *buf, unsigned int nbytes)
{
	FileDescriptor *openFile = getFileDescriptor(fd);
	if (openFile == NULL || loadFSData(openFile->disk) == -1)
		return -1;
	unsigned int sizeToRead = nbytes > inodeGetFileSize(openFile->inode) ? inodeGetFileSize(openFile->inode) : nbytes;
	unsigned int numBlocks = ceil((float)sizeToRead / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int offset = 0;
	unsigned char *blockBuffer = malloc(superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	for (unsigned int i = 0; i < numBlocks; i++)
	{
		if (i + 1 == numBlocks)
		{
			if (readBlock(openFile->disk, inodeGetBlockAddr(openFile->inode, i), blockBuffer) == -1)
				break;
			memcpy(&buf[offset], blockBuffer, sizeToRead - offset);
			offset += sizeToRead - offset;
		}
		else
		{
			if (readBlock(openFile->disk, inodeGetBlockAddr(openFile->inode, i), &buf[offset]) == -1)
				break;
			offset += superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
		}
	}
	free(blockBuffer);
	if (offset != sizeToRead)
		return -1;
	return offset;
}

// Funcao para a escrita de um arquivo, a partir de um descritor de
// arquivo existente. Os dados de buf serao copiados para o disco e
// terao tamanho maximo de nbytes. Retorna o numero de bytes
// efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite(int fd, const char *buf, unsigned int nbytes)
{
	FileDescriptor *openFile = getFileDescriptor(fd);
	if (openFile == NULL || loadFSData(openFile->disk) == -1)
		return -1;

	unsigned int bufferOffset = 0;
	unsigned int cursorBlock = openFile->cursor / superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	unsigned int cursorBlockOffset = openFile->cursor % superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
	unsigned char *blockBuffer = malloc(superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int blockAddr = inodeGetBlockAddr(openFile->inode, cursorBlock);
	unsigned int sizeToWriteInCursorBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] - cursorBlockOffset;
	if (sizeToWriteInCursorBlock > nbytes)
		sizeToWriteInCursorBlock = nbytes;
	if (blockAddr != 0)
	{
		if (readBlock(openFile->disk, blockAddr, blockBuffer) == -1)
		{
			free(blockBuffer);
			return -1;
		}
		for (unsigned int i = 0; i < sizeToWriteInCursorBlock; i++)
		{
			blockBuffer[cursorBlockOffset] = buf[bufferOffset];
			bufferOffset++;
			cursorBlockOffset++;
		}
		if (writeBlock(openFile->disk, blockAddr, blockBuffer, superblock[SUPERBLOCK_ITEM_BLOCKSIZE]) == -1)
		{
			free(blockBuffer);
			return -1;
		}
		cursorBlock++;

		unsigned int numBlocks = ceil((float)(nbytes - bufferOffset) / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
		unsigned int *newBlocks = NULL;
		unsigned int newBlocksOffset = 0;
		for (unsigned int i = 0; i < numBlocks; i++)
		{
			unsigned int sizeToWrite = nbytes - bufferOffset;
			if (sizeToWrite > superblock[SUPERBLOCK_ITEM_BLOCKSIZE])
				sizeToWrite = superblock[SUPERBLOCK_ITEM_BLOCKSIZE];
			blockAddr = inodeGetBlockAddr(openFile->inode, cursorBlock);
			if (blockAddr != 0)
			{
				if (writeBlock(openFile->disk, blockAddr, &buf[bufferOffset], sizeToWrite) == -1)
					break;
				bufferOffset += sizeToWrite;
			}
			else
			{
				if (newBlocks == NULL)
				{
					unsigned int numNewBlocks = ceil((float)(nbytes - bufferOffset) / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
					newBlocks = malloc(numNewBlocks * sizeof(unsigned int));
					if (newBlocks == NULL || findFreeBlocks(numNewBlocks, newBlocks) == -1)
						break;
				}
				if (writeBlock(openFile->disk, newBlocks[newBlocksOffset], &buf[bufferOffset], sizeToWrite) == -1)
					break;
				inodeAddBlock(openFile->inode, newBlocks[newBlocksOffset]);
				newBlocksOffset++;
				bufferOffset += sizeToWrite;
			}
		}
		free(blockBuffer);
		if (newBlocks != NULL)
		{
			setBlocksStatus(newBlocksOffset, newBlocks, 1);
			saveBitmap(openFile->disk);
			free(newBlocks);
		}
		inodeSetFileSize(openFile->inode, inodeGetFileSize(openFile->inode) + bufferOffset);
		inodeSave(openFile->inode);
		openFile->cursor += bufferOffset;
		return bufferOffset;
	}
}

// Funcao para fechar um arquivo, a partir de um descritor de arquivo
// existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose(int fd)
{
	FileDescriptor *fileToRemove = NULL;
	for (int i = 0; i < numOpenFiles; i++)
	{
		if (openFiles[i]->fd == fd)
			fileToRemove = openFiles[i];
		if (fileToRemove != NULL)
			openFiles[i] = openFiles[i + 1];
	}
	if (fileToRemove == NULL)
		return -1;
	free(fileToRemove->inode);
	free(fileToRemove);
	numOpenFiles--;
	return 0;
}

// Funcao para abertura de um diretorio, a partir do caminho
// especificado em path, no disco indicado por d, no modo Read/Write,
// criando o diretorio se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpenDir(Disk *d, const char *path)
{
	// skip
	return -1;
}

// Funcao para a leitura de um diretorio, identificado por um descritor
// de arquivo existente. Os dados lidos correspondem a uma entrada de
// diretorio na posicao atual do cursor no diretorio. O nome da entrada
// e' copiado para filename, como uma string terminada em \0 (max 255+1).
// O numero do inode correspondente 'a entrada e' copiado para inumber.
// Retorna 1 se uma entrada foi lida, 0 se fim de diretorio ou -1 caso
// mal sucedido
int myFSReadDir(int fd, char *filename, unsigned int *inumber)
{
	// skip
	return -1;
}

// Funcao para adicionar uma entrada a um diretorio, identificado por um
// descritor de arquivo existente. A nova entrada tera' o nome indicado
// por filename e apontara' para o numero de i-node indicado por inumber.
// Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSLink(int fd, const char *filename, unsigned int inumber)
{
	// skip
	return -1;
}

// Funcao para remover uma entrada existente em um diretorio,
// identificado por um descritor de arquivo existente. A entrada e'
// identificada pelo nome indicado em filename. Retorna 0 caso bem
// sucedido, ou -1 caso contrario.
int myFSUnlink(int fd, const char *filename)
{
	// skip
	return -1;
}

// Funcao para fechar um diretorio, identificado por um descritor de
// arquivo existente. Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSCloseDir(int fd)
{
	// skip
	return -1;
}

// Funcao para instalar seu sistema de arquivos no S.O., registrando-o junto
// ao virtual FS (vfs). Retorna um identificador unico (slot), caso
// o sistema de arquivos tenha sido registrado com sucesso.
// Caso contrario, retorna -1
int installMyFS(void)
{
	FSInfo *myfs = malloc(sizeof(FSInfo));
	myfs->fsid = 1;
	myfs->fsname = "WillianFS";
	myfs->isidleFn = myFSIsIdle;
	myfs->formatFn = myFSFormat;
	myfs->openFn = myFSOpen;
	myfs->readFn = myFSRead;
	myfs->writeFn = myFSWrite;
	myfs->closeFn = myFSClose;
	myfs->opendirFn = myFSOpenDir;
	myfs->readdirFn = myFSReadDir;
	myfs->linkFn = myFSLink;
	myfs->unlinkFn = myFSUnlink;
	myfs->closedirFn = myFSCloseDir;

	return vfsRegisterFS(myfs);
}