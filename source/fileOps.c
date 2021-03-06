/*////////////////////////////////////////////////////////////////////////////////////////

fsop contains coomprensive set of function for file and folder handling

en exposed s_fsop fsop structure can be used by callback to update operation status

(c) 2012 stfour

////////////////////////////////////////////////////////////////////////////////////////*/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/statvfs.h>

#include "svnrev.h"
#include "fileOps.h"
#include "Memory/mem2.hpp"

#define SET(a, b) a = b; DCFlushRange(&a, sizeof(a));

static u8 *buff = NULL;
static FILE *fs = NULL, *ft = NULL;
static u32 block = 32768;
static u32 blockIdx = 0;
static u32 blockInfo[2] = {0,0};
static u32 blockReady = 0;
static s32 stopThread;
static u64 folderSize = 0;

char fullGameName[80];
char *MainSource;
char *MainTarget;
const char *MIOS_Info;
u8 CopyProgress;
u64 FolderProgressBytes;

// return false if the file doesn't exist
bool fsop_GetFileSizeBytes(char *path, size_t *filesize)	// for me stats st_size report always 0 :(
{
	FILE *f;
	size_t size = 0;

	f = fopen(path, "rb");
	if(!f)
	{
		if(filesize)
			*filesize = size;
		return false;
	}

	//Get file size
	fseek( f, 0, SEEK_END);
	size = ftell(f);
	if(filesize)
		*filesize = size;
	fclose(f);
	
	return true;
}

/*
Recursive fsop_GetFolderBytes
*/
u64 fsop_GetFolderBytes (char *source)
{
	DIR *pdir;
	struct dirent *pent;
	char newSource[300];
	u64 bytes = 0;

	pdir = opendir(source);

	while ((pent=readdir(pdir)) != NULL) 
	{
		// Skip it
		if (strcmp (pent->d_name, ".") == 0 || strcmp (pent->d_name, "..") == 0)
			continue;

		snprintf(newSource, sizeof(newSource), "%s/%s", source, pent->d_name);

		// If it is a folder... recurse...
		if (fsop_DirExist (newSource))
		{
			bytes += fsop_GetFolderBytes (newSource);
		}
		else	// It is a file !
		{
			size_t s;
			fsop_GetFileSizeBytes (newSource, &s);
			bytes += s;
		}
	}
	closedir(pdir);

	return bytes;
}

u32 fsop_GetFolderKb (char *source)
{
	u32 ret = (u32) round ((double)fsop_GetFolderBytes (source) / 1000.0);

	return ret;
}

u32 fsop_GetFreeSpaceKb (char *path) // Return free kb on the device passed
{
	struct statvfs s;

	statvfs(path, &s);

	u32 ret = (u32)round( ((double)s.f_bfree / 1000.0) * s.f_bsize);

	return ret ;
}

bool fsop_FileExist(const char *fn)
{
	FILE * f;
	f = fopen(fn, "rb");
	if (f) 
	{
		fclose(f);
		return true;
	}
	return false;
}

bool fsop_DirExist(char *path)
{
	DIR *dir;

	dir=opendir(path);
	if (dir)
	{
		closedir(dir);
		return true;
	}

	return false;
}


bool fsop_MakeFolder(char *path)
{
	if(fsop_DirExist(path) || mkdir(path, S_IREAD | S_IWRITE) == 0)
		return true;

	return false;
}

static void *thread_CopyFileReader()
{
	u32 rb;
	stopThread = 0;
	DCFlushRange(&stopThread, sizeof(stopThread));
	do
	{
		SET(rb, fread(&buff[blockIdx*block], 1, block, fs ));
		SET(blockInfo[blockIdx], rb);
		SET(blockReady, 1);

		while(blockReady && !stopThread) usleep(1);
	}
	while(stopThread == 0);

	stopThread = -1;
	DCFlushRange(&stopThread, sizeof(stopThread));

	return 0;
}

bool fsop_CopyFile(char *source, char *target)
{
	int err = 0;

	u32 size;
	u32 rb,wb;

	fs = fopen(source, "rb");
	if (!fs)
	{
		return false;
	}

	ft = fopen(target, "wt");
	if (!ft)
	{
		fclose(fs);
		return false;
	}

	//Get file size
	fseek (fs, 0, SEEK_END);
	size = ftell(fs);

	if (size == 0)
	{
		fclose(fs);
		fclose(ft);
		return true;
	}

	// Return to beginning....
	fseek(fs, 0, SEEK_SET);

	u8 *threadStack = NULL;
	lwp_t hthread = LWP_THREAD_NULL;

	buff = MEM2_alloc(block * 2);
	if(buff == NULL)
		return false;

	blockIdx = 0;
	blockReady = 0;
	blockInfo[0] = 0;
	blockInfo[1] = 0;
	u32 bytes = 0;

	threadStack = MEM2_alloc(32768);
	if(threadStack == NULL)
		return false;

	LWP_CreateThread(&hthread, thread_CopyFileReader, NULL, threadStack, 32768, 30);

	while(stopThread != 0)
		usleep(5);

	u32 bi;
	do
	{
		while(!blockReady)
			usleep(1); // Let's wait for incoming block from the thread
		
		bi = blockIdx;

		// let's th thread to read the next buff
		SET(blockIdx, 1 - blockIdx);
		SET(blockReady, 0);

		rb = blockInfo[bi];
		// write current block
		wb = fwrite(&buff[bi*block], 1, rb, ft);

		if(wb != wb || rb == 0)
			err = 1;
		bytes += rb;

		FolderProgressBytes += rb;
		refreshProgressBar();
	}
	while(bytes < size && err == 0);

	stopThread = 1;
	DCFlushRange(&stopThread, sizeof(stopThread));

	while(stopThread != -1)
		usleep(5);

	LWP_JoinThread(hthread, NULL);
	MEM2_free(threadStack);

	stopThread = 1;
	DCFlushRange(&stopThread, sizeof(stopThread));

	fclose(fs);
	fclose(ft);
	MEM2_free(buff);

	if(err) 
	{
		unlink (target);
		return false;
	}

	return true;
}

/*
Recursive copyfolder
*/
static bool doCopyFolder(char *source, char *target)
{
	DIR *pdir;
	struct dirent *pent;
	char newSource[300], newTarget[300];
	bool ret = true;

	// If target folder doesn't exist, create it !
	if(!fsop_DirExist(target))
		fsop_MakeFolder(target);

	pdir = opendir(source);

	while((pent=readdir(pdir)) != NULL && ret == true) 
	{
		// Skip it
		if(strcmp (pent->d_name, ".") == 0 || strcmp (pent->d_name, "..") == 0)
			continue;
	
		snprintf(newSource, sizeof(newSource), "%s/%s", source, pent->d_name);
		snprintf(newTarget, sizeof(newTarget), "%s/%s", target, pent->d_name);
		
		// If it is a folder... recurse...
		if(fsop_DirExist(newSource))
			ret = doCopyFolder(newSource, newTarget);
		else	// It is a file !
			ret = fsop_CopyFile(newSource, newTarget);
	}

	closedir(pdir);

	return ret;
}

bool fsop_CopyFolder(char *source, char *target, const char *gamename, const char *gameID, const char *MIOS_inf)
{
	snprintf(fullGameName, sizeof(fullGameName), "[%s] %s", gameID, gamename);
	CopyProgress = 0;
	FolderProgressBytes = 0;
	MainSource = source;
	MainTarget = target;
	MIOS_Info = MIOS_inf;
	folderSize = fsop_GetFolderBytes(source);
	refreshProgressBar();

	return doCopyFolder(source, target);
}

void fsop_deleteFolder(char *source)
{
	DIR *pdir;
	struct dirent *pent;
	char newSource[300];

	pdir = opendir(source);

	while((pent = readdir(pdir)) != NULL) 
	{
		// Skip it
		if(strcmp(pent->d_name, ".") == 0 || strcmp(pent->d_name, "..") == 0)
			continue;

		snprintf(newSource, sizeof(newSource), "%s/%s", source, pent->d_name);

		// If it is a folder... recurse...
		if(fsop_DirExist(newSource))
			fsop_deleteFolder(newSource);
		else	// It is a file !
			fsop_deleteFile(newSource);
	}
	closedir(pdir);
	unlink(source);
}

void fsop_deleteFile(char *source)
{
	if(fsop_FileExist(source))
		remove(source);
}

void refreshProgressBar()
{
	/* Clear console */
	VIDEO_WaitVSync();
	printf("\x1b[2J");
	printf("\x1b[37m");
	printf("DML Game Booter SVN r%s by FIX94, Game Copier by stfour\n", SVN_REV);
	printf(MIOS_Info);

	printf("Copying game: %s...\n \n", fullGameName);
	printf("Progress: %3.2f%% \n \n", (float)FolderProgressBytes / (float)folderSize * 100);
	printf("Source Folder: %s\n", MainSource);
	printf("Target Folder: %s\n", MainTarget);
}
