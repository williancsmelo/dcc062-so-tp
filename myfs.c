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
	if (inodeRoot == NULL)
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
char *bitmap = NULL; // tamanho = numero de blocos | 1 = ocupado, 0 = livre

#define MAX_OPEN_FILES 20
Inode *openFiles[MAX_OPEN_FILES];
unsigned int numOpenFiles = 0;

int writeBlock(Disk *d, unsigned int block, const char *buf)
{
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned int firstSector = block * sectorPerBlock;
	for (unsigned int i = 0; i < sectorPerBlock; i++)
	{
		unsigned char sector[DISK_SECTORDATASIZE];
		memcpy(sector, &buf[i * DISK_SECTORDATASIZE], DISK_SECTORDATASIZE);
		if (diskWriteSector(d, firstSector + i, sector) == -1)
			return -1;
	}
	return 0;
}

int readBlock(Disk *d, unsigned int block, char *buf)
{
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned int firstSector = block * sectorPerBlock;
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
	diskWriteSector(d, SUPERBLOCK_SECTOR, sector);
	return 0;
}

int loadSuperblock(Disk *d)
{
	free(superblock);
	unsigned char sector[DISK_SECTORDATASIZE];
	if (diskReadSector(d, SUPERBLOCK_SECTOR, sector) == -1)
		return -1;
	superblock = malloc(SUPERBLOCK_SIZE * sizeof(unsigned int));
	for (int a = 0; a < SUPERBLOCK_SIZE; a++)
		char2ul(&sector[a * sizeof(unsigned int)], &(superblock[a]));
	return 0;
}

// funções do bitmap
int saveBitmap(Disk *d)
{
	const unsigned int firstSector = superblock[SUPERBLOCK_ITEM_BITMAPBLOCK] * superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	const unsigned int numSectors = sizeof(unsigned char) * superblock[SUPERBLOCK_ITEM_NUMBLOCKS] / DISK_SECTORDATASIZE;
	for (int i = 0; i < numSectors; i++)
	{
		unsigned char sector[DISK_SECTORDATASIZE];
		memcpy(sector, &bitmap[i * DISK_SECTORDATASIZE], DISK_SECTORDATASIZE);
		if (diskWriteSector(d, firstSector + i, sector) == -1)
			return -1;
	}
}

int findFreeBlocks(unsigned int numBlocks, unsigned int *blocks)
{
	unsigned int freeBlocks = 0;
	for (unsigned int i = 0; i < superblock[SUPERBLOCK_ITEM_NUMBLOCKS]; i++)
	{
		if (!bitmap[i])
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

Directory *loadDirectory(Disk *d, Inode *inode)
{
	if (inodeGetFileType(inode) != FILETYPE_DIR)
		return NULL;
	Directory *dir = malloc(sizeof(Directory));
	if (dir == NULL)
		return NULL;
	unsigned int numBlocks = ceil((float)inodeGetFileSize(inode) / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	unsigned char *buffer = malloc(numBlocks * superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	for (unsigned int i = 0; i < numBlocks; i++)
	{
		unsigned int block = inodeGetBlockAddr(inode, i);
		unsigned int firstSector = block * sectorPerBlock;
		for (unsigned int j = 0; j < sectorPerBlock; j++)
		{
			if (diskReadSector(d, firstSector + j, &buffer[j * DISK_SECTORDATASIZE + i * superblock[SUPERBLOCK_ITEM_BLOCKSIZE]]) == -1)
				return NULL;
		}
	}
	char2ul(&buffer[0], &(dir->numEntries));
	unsigned int offset = sizeof(unsigned int);
	if (dir->numEntries > 0)
	{
		dir->entries = malloc(dir->numEntries * sizeof(DirectoryEntry *));
		for (unsigned int i = 0; i < dir->numEntries; i++)
		{
			dir->entries[i] = malloc(sizeof(DirectoryEntry));
			char2ul(&buffer[offset], &(dir->entries[i]->inodeNumber));
			offset += sizeof(unsigned int);
			memcpy(&buffer[offset], dir->entries[i]->name, MAX_FILENAME_LENGTH);
			offset += MAX_FILENAME_LENGTH;
		}
	}
	else
	{
		dir->entries = NULL;
	}
	free(buffer);
	return dir;
}

void freeDirectory(Directory *dir)
{
	if (dir == NULL)
		return;
	for (unsigned int i = 0; i < dir->numEntries; i++)
		free(dir->entries[i]);
	free(dir->entries);
	free(dir);
}

int addDirectoryEntry(Disk *d, Inode *inodeDir, Inode *inodeEntry, const char *entryName)
{
	Directory *dir = loadDirectory(d, inodeDir);

	if (dir == NULL)
		return -1;
	for (unsigned int i = 0; i < dir->numEntries; i++)
		if (strcmp(dir->entries[i]->name, entryName) == 0)
		{
			freeDirectory(dir);
			return -1;
		}

	DirectoryEntry *entry = malloc(sizeof(DirectoryEntry));
	if (entry == NULL)
		return -1;
	entry->inodeNumber = inodeGetNumber(inodeEntry);
	strncpy(entry->name, entryName, MAX_FILENAME_LENGTH);
	// save entry

	unsigned char toSaveBuffer[sizeof(DirectoryEntry)];
	ul2char(entry->inodeNumber, toSaveBuffer);
	memcpy(&toSaveBuffer[sizeof(unsigned int)], entry->name, MAX_FILENAME_LENGTH);
	free(entry);

	unsigned int finalBlock = inodeGetBlockAddr(inodeDir, inodeGetFileSize(inodeDir) / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned char finalBlockBuffer[superblock[SUPERBLOCK_ITEM_BLOCKSIZE]];

	if (readBlock(d, finalBlock, finalBlockBuffer) == -1)
		return -1;

	unsigned int finalBlockFreeSpace = inodeGetFileSize(inodeDir) % superblock[SUPERBLOCK_ITEM_BLOCKSIZE];

	unsigned int offset = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] - finalBlockFreeSpace;

	if (finalBlockFreeSpace > sizeof(DirectoryEntry))
		memcpy(&finalBlockBuffer[offset], toSaveBuffer, sizeof(DirectoryEntry));
	else
	{
		unsigned int blocks[1];
		if (findFreeBlocks(1, blocks) == -1)
			return -1;
		unsigned char newBlockBuffer[superblock[SUPERBLOCK_ITEM_BLOCKSIZE]];
		for (unsigned int i = 0; i < sizeof(DirectoryEntry); i++)
		{
			if (offset >= superblock[SUPERBLOCK_ITEM_BLOCKSIZE])
				newBlockBuffer[offset - superblock[SUPERBLOCK_ITEM_BLOCKSIZE]] = toSaveBuffer[i];
			else
				finalBlockBuffer[offset] = toSaveBuffer[i];
			offset++;
		}
		if (writeBlock(d, blocks[0], newBlockBuffer) == -1)
			return -1;
		inodeAddBlock(inodeDir, blocks[0]);
		setBlocksStatus(1, blocks, 1);
	}

	if (writeBlock(d, finalBlock, finalBlockBuffer) == -1)
		return -1;

	inodeSetFileSize(inodeDir, inodeGetFileSize(inodeDir) + sizeof(DirectoryEntry));
	inodeSetRefCount(inodeEntry, inodeGetRefCount(inodeEntry) + 1);

	if (inodeSave(inodeEntry) == -1 || inodeSave(inodeDir) == -1)
		return -1;
	return saveBitmap(d);
}

int createDirectory(Disk *d, Inode *inode)
{
	Directory *dir = malloc(sizeof(Directory));
	if (dir == NULL)
		return -1;
	dir->numEntries = 0;
	dir->entries = NULL;
	unsigned int blocks[1];
	if (findFreeBlocks(1, blocks) == -1)
		return -1;
	unsigned char sector[DISK_SECTORDATASIZE];
	ul2char(dir->numEntries, sector);
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	if (diskWriteSector(d, blocks[0] * sectorPerBlock, sector) == -1)
		return -1;
	inodeSetFileSize(inode, sizeof(unsigned int));
	inodeSetFileType(inode, FILETYPE_DIR);
	inodeSetRefCount(inode, 1);
	inodeAddBlock(inode, blocks[0]);
	setBlocksStatus(1, blocks, 1);
	saveBitmap(d);
	free(dir);
	return addDirectoryEntry(d, inode, inode, ".");
}

unsigned int findInodeNumber(Directory *dir, const char *name)
{
	for (unsigned int i = 0; i < dir->numEntries; i++)
		if (strcmp(dir->entries[i]->name, name) == 0)
			return dir->entries[i]->inodeNumber;
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

	Inode *inodeRoot;
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
	unsigned int inodesBlocks = (inodesSectors + inodeAreaBeginSector()) * DISK_SECTORDATASIZE / blockSize;
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
		// Ignorar entradas "." (atual) e ".." (anterior)
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
	if (numOpenFiles >= MAX_OPEN_FILES)
		return -1;
	char **entries;
	int numEntries = splitPath(path, &entries);
	if (numEntries == -1)
		return -1;

	if (loadRootInode(d) == -1)
		return -1;

	Inode *inode = NULL;
	Directory *dir = NULL;
	char foundDir = 0;
	for (int i = 0; i < numEntries - 1; i++)
	{
		dir = loadDirectory(d, i == 0 ? inodeRoot : inode);
		if (dir == NULL)
			break;
		unsigned int inodeNumber = findInodeNumber(dir, entries[i]);
		freeDirectory(dir);
		dir = NULL;
		if (inodeNumber == 0)
			break;
		free(inode);
		inode = inodeLoad(inodeNumber, d);
		if (inode == NULL)
			break;
		if (i == numEntries - 2)
			foundDir = 1;
	}
	char foundFile = 0;
	if (foundDir)
		dir = loadDirectory(d, inode);
	Inode *inodeFile = NULL;
	if (dir != NULL)
	{
		unsigned int inodeNumber = findInodeNumber(dir, entries[numEntries - 1]);
		if (inodeNumber != 0)
		{
			inodeFile = inodeLoad(inodeNumber, d);
			if (inodeFile != NULL)
				foundFile = 1;
		}
		else
		{
			inodeNumber = inodeFindFreeInode(ROOT_INODE_NUMBER + 1, d);
			if (inodeNumber != 0)
			{
				inodeFile = inodeCreate(inodeNumber, d);
				if (inode != NULL)
				{
					inodeSetFileType(inode, FILETYPE_REGULAR);
					addDirectoryEntry(d, inode, inodeFile, entries[numEntries - 1]);
					foundFile = 1;
				}
			}
		}
	}

	int fd = -1;
	if (foundFile)
	{
		for (unsigned int i = 0; i < numOpenFiles; i++)
		{
			if (openFiles[i] == inodeFile)
			{
				// File is already open, return its file descriptor
				free(entries);
				return i;
			}
		}
	}

	// Check if the file is already open

	// Add the file to the open files array
	openFiles[numOpenFiles++] = inode;

	// Free the memory allocated for the path entries
	free(entries);

	// Return the file descriptor (index in the open files array)
	return numOpenFiles - 1;
}

// Funcao para a leitura de um arquivo, a partir de um descritor de
// arquivo existente. Os dados lidos sao copiados para buf e terao
// tamanho maximo de nbytes. Retorna o numero de bytes efetivamente
// lidos em caso de sucesso ou -1, caso contrario.
int myFSRead(int fd, char *buf, unsigned int nbytes)
{
	return -1;
}

// Funcao para a escrita de um arquivo, a partir de um descritor de
// arquivo existente. Os dados de buf serao copiados para o disco e
// terao tamanho maximo de nbytes. Retorna o numero de bytes
// efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite(int fd, const char *buf, unsigned int nbytes)
{
	return -1;
}

// Funcao para fechar um arquivo, a partir de um descritor de arquivo
// existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose(int fd)
{

	return -1;
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