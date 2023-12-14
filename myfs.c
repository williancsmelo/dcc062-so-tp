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
#include <math.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

// Declaracoes globais
#define ROOT_INODE_NUMBER 1
#define NUMBLOCKS_PERINODE 8

// superbloco
#define SUPERBLOCK_SECTOR 0
#define SUPERBLOCK_SIZE 3 // Tamanho do superbloco em numero de unsigned ints
#define SUPERBLOCK_ITEM_BLOCKSIZE 0
#define SUPERBLOCK_ITEM_NUMBLOCKS 1
#define SUPERBLOCK_ITEM_NUMINODES 2

// bitmap
#define BITMAP_SECTOR 1

// Funções do superbloco
unsigned int superblock[SUPERBLOCK_SIZE];
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
	unsigned char sector[DISK_SECTORDATASIZE];
	if (diskReadSector(d, SUPERBLOCK_SECTOR, sector) == -1)
		return -1;
	for (int a = 0; a < SUPERBLOCK_SIZE; a++)
		char2ul(&sector[a * sizeof(unsigned int)], &(superblock[a]));
	return 0;
}

// funções do bitmap
char *bitmap; // tamanho = numero de blocos | 1 = ocupado, 0 = livre

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
	DirectoryEntry *entries;
	unsigned int numEntries;
} Directory;

void addDirectoryEntry(Directory *dir, char *name, unsigned int inodeNumber)
{
	DirectoryEntry *entry = malloc(sizeof(DirectoryEntry));
	strcpy(entry->name, name);
	entry->inodeNumber = inodeNumber;
	dir->entries[dir->numEntries] = *entry;
	dir->numEntries++;
}

Directory *createDirectory()
{
	Directory *dir = malloc(sizeof(Directory));
	dir->numEntries = 0;
	addDirectoryEntry(dir, ".", 1);
	return dir;
}

int saveDirectory(Disk *d, Directory *dir, Inode *inode)
{
	unsigned int directorySize = (dir->numEntries * sizeof(DirectoryEntry)) + sizeof(unsigned int);
	unsigned int numBlocks = ceil((double)directorySize / superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int *blocks = malloc(numBlocks * sizeof(unsigned int));
	if (findFreeBlocks(numBlocks, blocks) == -1)
		return -1;
	char *buffer = malloc(numBlocks * superblock[SUPERBLOCK_ITEM_BLOCKSIZE]);
	unsigned int offset = sizeof(unsigned int);
	ul2char(dir->numEntries, &buffer[0]);
	for (unsigned int i = 0; i < dir->numEntries; i++)
	{
		ul2char(dir->entries[i].inodeNumber, &buffer[offset]);
		offset += sizeof(unsigned int);
		memcpy(&buffer[offset], dir->entries[i].name, MAX_FILENAME_LENGTH);
		offset += MAX_FILENAME_LENGTH;
	}
	unsigned int sectorPerBlock = superblock[SUPERBLOCK_ITEM_BLOCKSIZE] / DISK_SECTORDATASIZE;
	for (unsigned int i = 0; i < numBlocks; i++)
	{
		unsigned int firstSector = blocks[i] * sectorPerBlock;
		for (unsigned int j = 0; j < sectorPerBlock; j++)
		{
			char sector[DISK_SECTORDATASIZE];
			memcpy(sector, &buffer[i * superblock[SUPERBLOCK_ITEM_BLOCKSIZE] + j * DISK_SECTORDATASIZE], DISK_SECTORDATASIZE);
			if (diskWriteSector(d, firstSector + j, sector) == -1)
				return -1;
		}
	}
	setBlocksStatus(numBlocks, blocks, 1);
	free(blocks);
	free(buffer);
	return 0;
}

// Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
// se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
// um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle(Disk *d)
{
	// for (int i = 0; i < MAX_FILES; i++)
	// {
	// 	if (inodeGetRefCount(inodeLoad(i + 1, d)) > 0)
	// 		return 0;
	// }

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
	superblock[SUPERBLOCK_ITEM_BLOCKSIZE] = blockSize;
	superblock[SUPERBLOCK_ITEM_NUMBLOCKS] = diskGetSize(d) / blockSize;
	superblock[SUPERBLOCK_ITEM_NUMINODES] = superblock[SUPERBLOCK_ITEM_NUMBLOCKS] / NUMBLOCKS_PERINODE;
	if (saveSuperblock(d) == -1)
		return -1;

	// criar bitmap
	bitmap = malloc(superblock[SUPERBLOCK_ITEM_NUMBLOCKS] * sizeof(char));
	for (int i = 0; i < superblock[SUPERBLOCK_ITEM_NUMBLOCKS]; i++)
		bitmap[i] = 0;
	if (diskWriteSector(d, BITMAP_SECTOR, bitmap) == -1)
		return -1;

	Inode *inodeRoot;
	// Inicializar i-nodes
	for (int i = 1; i < superblock[SUPERBLOCK_ITEM_NUMINODES] + 1; i++)
	{
		Inode *inode = inodeCreate(i, d);
		if (inode == NULL)
			return -1;

		if (i == ROOT_INODE_NUMBER)
		{
			inodeRoot = inode;
			inodeSetFileType(inode, FILETYPE_DIR);
			inodeSetRefCount(inode, 2);
		}
		else
		{
			free(inode);
		}
	}

	unsigned int inodesSectors = superblock[SUPERBLOCK_ITEM_NUMINODES] / inodeNumInodesPerSector();
	unsigned int inodesBlocks = inodesSectors * DISK_SECTORDATASIZE / blockSize;
	for (int i = 0; i < inodesBlocks; i++)
		bitmap[i] = 1;

	Directory *root = createDirectory(d);
	addDirectoryEntry(root, "..", ROOT_INODE_NUMBER);
	saveDirectory(d, root, inodeRoot);
	free(root);

	return superblock[SUPERBLOCK_ITEM_NUMBLOCKS];
}

// Funcao para abertura de um arquivo, a partir do caminho especificado
// em path, no disco montado especificado em d, no modo Read/Write,
// criando o arquivo se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen(Disk *d, const char *path)
{

	return -1;
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
	FSInfo *myfs = (FSInfo *)malloc(sizeof(FSInfo));
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