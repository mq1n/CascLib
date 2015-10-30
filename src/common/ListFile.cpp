/*****************************************************************************/
/* ListFile.cpp                           Copyright (c) Ladislav Zezula 2004 */
/*---------------------------------------------------------------------------*/
/* Description:                                                              */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 12.06.04  1.00  Lad  The first version of ListFile.cpp                    */
/*****************************************************************************/

#define __CASCLIB_SELF__
#include "../CascLib.h"
#include "../CascCommon.h"

//-----------------------------------------------------------------------------
// Listfile entry structure

#define CACHE_BUFFER_SIZE  0x1000       // Size of the cache buffer

typedef bool (*RELOAD_CACHE)(void * pvCacheContext, LPBYTE pbBuffer, DWORD dwBytesToRead);
typedef void (*CLOSE_STREAM)(void * pvCacheContext);

struct TListFileCache
{
    RELOAD_CACHE pfnReloadCache;        // Function for reloading the cache
    CLOSE_STREAM pfnCloseStream;        // Function for closing the stream
    LPBYTE pBegin;                      // The begin of the listfile cache
    LPBYTE pPos;                        // Current position in the cache
    LPBYTE pEnd;                        // The last character in the file cache
    void * pvCacheContext;              // Reload context passed to reload function
    DWORD  dwFileSize;                  // Total size of the cached file
    DWORD  dwFilePos;                   // Position of the cache in the file

    // Followed by the cache (variable length)
};

//-----------------------------------------------------------------------------
// Dummy cache functions

static bool ReloadCache_DoNothing(void * /* pvCacheContext */, LPBYTE /* pbBuffer */, DWORD /* dwBytesToRead */)
{
    return false;
}

static void CloseStream_DoNothing(void * /* pvCacheContext */)
{}

//-----------------------------------------------------------------------------
// Reloading cache from a file

static bool ReloadCache_ExternalFile(void * pvCacheContext, LPBYTE pbBuffer, DWORD dwBytesToRead)
{
    TFileStream * pStream = (TFileStream *)pvCacheContext;

    return FileStream_Read(pStream, NULL, pbBuffer, dwBytesToRead);
}

static void CloseStream_ExternalFile(void * pvCacheContext)
{
    TFileStream * pStream = (TFileStream *)pvCacheContext;

    return FileStream_Close(pStream);
}

// Reloads the cache. Returns number of characters
// that has been loaded into the cache.
static DWORD ReloadListFileCache(TListFileCache * pCache)
{
    size_t cbCacheSize = (size_t)(pCache->pEnd - pCache->pBegin);
    DWORD dwBytesToRead = 0;

    // Only do something if the cache is empty
    if(pCache->pPos >= pCache->pEnd)
    {
        // Move the file position forward
        pCache->dwFilePos += cbCacheSize;
        if(pCache->dwFilePos >= pCache->dwFileSize)
            return 0;

        // Get the number of bytes remaining
        dwBytesToRead = pCache->dwFileSize - pCache->dwFilePos;
        if(dwBytesToRead > cbCacheSize)
            dwBytesToRead = cbCacheSize;

        // Load the next data chunk to the cache
        // If we didn't read anything, it might mean that the block
        // of the file is not available
        // We stop reading the file at this point, because the rest
        // of the listfile is unreliable
        if(!pCache->pfnReloadCache(pCache->pvCacheContext, pCache->pBegin, dwBytesToRead))
            return 0;

        // Set the buffer pointers
        pCache->pPos = pCache->pBegin;
        pCache->pEnd = pCache->pBegin + dwBytesToRead;
    }

    return dwBytesToRead;
}

static size_t ReadListFileLine(TListFileCache * pCache, char * szLine, size_t nMaxChars)
{
    char * szLineBegin = szLine;
    char * szLineEnd = szLine + nMaxChars - 1;
    char * szExtraString = NULL;
    
    // Skip newlines, spaces, tabs and another non-printable stuff
    for(;;)
    {
        // If we need to reload the cache, do it
        if(pCache->pPos == pCache->pEnd)
        {
            if(ReloadListFileCache(pCache) == 0)
                break;
        }

        // If we found a non-whitespace character, stop
        if(pCache->pPos[0] > 0x20)
            break;

        // Skip the character
        pCache->pPos++;
    }

    // Copy the remaining characters
    while(szLine < szLineEnd)
    {
        // If we need to reload the cache, do it now and resume copying
        if(pCache->pPos == pCache->pEnd)
        {
            if(ReloadListFileCache(pCache) == 0)
                break;
        }

        // If we have found a newline, stop loading
        if(pCache->pPos[0] == 0x0D || pCache->pPos[0] == 0x0A)
            break;

        // Blizzard listfiles can also contain information about patch:
        // Pass1\Files\MacOS\unconditional\user\Background Downloader.app\Contents\Info.plist~Patch(Data#frFR#base-frFR,1326)
        if(pCache->pPos[0] == '~')
            szExtraString = szLine;

        // Copy the character
        *szLine++ = *pCache->pPos++;
    }

    // Terminate line with zero
    *szLine = 0;

    // If there was extra string after the file name, clear it
    if(szExtraString != NULL)
    {
        if(szExtraString[0] == '~' && szExtraString[1] == 'P')
        {
            szLine = szExtraString;
            *szExtraString = 0;
        }
    }

    // Return the length of the line
    return (szLine - szLineBegin);
}

static TListFileCache * CreateListFileCache(RELOAD_CACHE pfnReloadCache, CLOSE_STREAM pfnCloseStream, void * pvCacheContext, DWORD dwFileSize)
{
    TListFileCache * pCache = NULL;
    DWORD dwBytesToRead;

    // Allocate cache for one file block
    pCache = (TListFileCache *)CASC_ALLOC(BYTE, sizeof(TListFileCache) + CACHE_BUFFER_SIZE);
    if(pCache != NULL)
    {
        // Clear the entire structure
        memset(pCache, 0, sizeof(TListFileCache));
        pCache->pfnReloadCache = pfnReloadCache;
        pCache->pfnCloseStream = pfnCloseStream;
        pCache->pvCacheContext = pvCacheContext;
        pCache->dwFileSize = dwFileSize;

        // Set the initial pointers
        pCache->pBegin = (LPBYTE)(pCache + 1);

        // Load the file cache from the file
        dwBytesToRead = CASCLIB_MIN(CACHE_BUFFER_SIZE, dwFileSize);
        if(pfnReloadCache(pvCacheContext, pCache->pBegin, dwBytesToRead))
        {
            // Allocate pointers
            pCache->pPos = pCache->pBegin;
            pCache->pEnd = pCache->pBegin + dwBytesToRead;
        }
        else
        {
            ListFile_Free(pCache);
            pCache = NULL;
        }
    }

    // Return the cache
    return pCache;
}

static TListFileCache * CreateListFileCache(LPBYTE pbBuffer, DWORD cbBuffer)
{
    TListFileCache * pCache = NULL;

    // Allocate cache for one file block
    pCache = (TListFileCache *)CASC_ALLOC(BYTE, sizeof(TListFileCache));
    if(pCache != NULL)
    {
        // Clear the entire structure
        memset(pCache, 0, sizeof(TListFileCache));
        pCache->pfnReloadCache = ReloadCache_DoNothing;
        pCache->pfnCloseStream = CloseStream_DoNothing;
        pCache->dwFileSize = cbBuffer;

        // Do not copy the buffer, just set pointers
        pCache->pBegin = pCache->pPos = pbBuffer;
        pCache->pEnd = pbBuffer + cbBuffer;
    }

    // Return the cache
    return pCache;
}

//-----------------------------------------------------------------------------
// Functions for parsing an external listfile

void * ListFile_OpenExternal(const TCHAR * szListFile)
{
    TListFileCache * pCache;
    TFileStream * pStream;
    ULONGLONG FileSize = 0;

    // Open the external listfile
    pStream = FileStream_OpenFile(szListFile, STREAM_FLAG_READ_ONLY);
    if(pStream != NULL)
    {
        // Retrieve the size of the external listfile
        FileStream_GetSize(pStream, &FileSize);
        if(0 < FileSize && FileSize <= 0xFFFFFFFF)
        {                                                               
            // Create the cache for the listfile
            pCache = CreateListFileCache(ReloadCache_ExternalFile, CloseStream_ExternalFile, pStream, (DWORD)FileSize);
            if(pCache != NULL)
                return pCache;
        }

        // Close the file stream
        FileStream_Close(pStream);
    }

    return NULL;
}

void * ListFile_FromBuffer(LPBYTE pbBuffer, DWORD cbBuffer)
{
    return CreateListFileCache(pbBuffer, cbBuffer);
}

size_t ListFile_GetNextLine(void * pvListFile, char * szBuffer, size_t nMaxChars)
{
    TListFileCache * pCache = (TListFileCache *)pvListFile;
    size_t nLength = 0;
    int nError = ERROR_INVALID_PARAMETER;

    // Check for parameters
    if(pCache != NULL)
    {
        // Read the (next) line
        nLength = ReadListFileLine(pCache, szBuffer, nMaxChars);
        nError = (nLength != 0) ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    }

    if(nError != ERROR_SUCCESS)
        SetLastError(nError);
    return nLength;
}

size_t ListFile_GetNext(void * pvListFile, const char * szMask, char * szBuffer, size_t nMaxChars)
{
    TListFileCache * pCache = (TListFileCache *)pvListFile;
    size_t nLength = 0;
    int nError = ERROR_INVALID_PARAMETER;

    // Check for parameters
    if(pCache != NULL)
    {
        for(;;)
        {
            // Read the (next) line
            nLength = ReadListFileLine(pCache, szBuffer, nMaxChars);
            if(nLength == 0)
            {
                nError = ERROR_NO_MORE_FILES;
                break;
            }

            // If some mask entered, check it
            if(CheckWildCard(szBuffer, szMask))
            {
                nError = ERROR_SUCCESS;
                break;
            }
        }
    }

    if(nError != ERROR_SUCCESS)
        SetLastError(nError);
    return nLength;
}

void ListFile_Free(void * pvListFile)
{
    TListFileCache * pCache = (TListFileCache *)pvListFile;

    // Valid parameter check
    if(pCache != NULL)
    {
        if(pCache->pfnCloseStream != NULL)
            pCache->pfnCloseStream(pCache->pvCacheContext);
        CASC_FREE(pCache);
    }
}

//-----------------------------------------------------------------------------
// Functions for creating a listfile map

#define LISTMAP_INITIAL   0x100000

static PLISTFILE_MAP ListMap_Create()
{
    PLISTFILE_MAP pListMap;
    size_t cbToAllocate;
    
    // Create buffer for the listfile
    // Note that because the listfile is quite big and CASC_REALLOC
    // is a costly operation, we want to have as few reallocs as possible.
    cbToAllocate = sizeof(LISTFILE_MAP) + LISTMAP_INITIAL;
    pListMap = (PLISTFILE_MAP)CASC_ALLOC(BYTE, cbToAllocate);
    if(pListMap != NULL)
    {
        // Fill the listfile buffer
        memset(pListMap, 0, sizeof(LISTFILE_MAP));
        pListMap->cbBufferMax = LISTMAP_INITIAL;
    }

    return pListMap;
}

static PLISTFILE_MAP ListMap_InsertName(PLISTFILE_MAP pListMap, const char * szFileName, size_t nLength)
{
    PLISTFILE_ENTRY pListEntry;
    char szFileName2[MAX_PATH+1];
    size_t cbToAllocate;
    size_t cbEntrySize;
    uint32_t dwHashHigh = 0;
    uint32_t dwHashLow = 0;

    // Make sure there is enough space in the list map
    cbEntrySize = sizeof(LISTFILE_ENTRY) + nLength;
    cbEntrySize = ALIGN_TO_SIZE(cbEntrySize, 8);
    if((pListMap->cbBuffer + cbEntrySize) > pListMap->cbBufferMax)
    {
        cbToAllocate = sizeof(LISTFILE_MAP) + (pListMap->cbBufferMax * 3) / 2;
        pListMap = (PLISTFILE_MAP)CASC_REALLOC(BYTE, pListMap, cbToAllocate);
        if(pListMap == NULL)
            return NULL;

        pListMap->cbBufferMax = (pListMap->cbBufferMax * 3) / 2;
    }

    // Get the pointer to the first entry
    pListEntry = (PLISTFILE_ENTRY)((LPBYTE)(pListMap + 1) + pListMap->cbBuffer);

    // Get the name hash
    NormalizeFileName_UpperBkSlash(szFileName2, szFileName, MAX_PATH);
    hashlittle2(szFileName2, nLength, &dwHashHigh, &dwHashLow);
    
    // Calculate the HASH value of the normalized file name
    pListEntry->FileNameHash = ((ULONGLONG)dwHashHigh << 0x20) | dwHashLow;
    pListEntry->cbEntrySize = (DWORD)cbEntrySize;
    memcpy(pListEntry->szFileName, szFileName, nLength);
    pListEntry->szFileName[nLength] = 0;

    // Move the next entry
    pListMap->cbBuffer += cbEntrySize;
    pListMap->nEntries++;
    return pListMap;
}

static PLISTFILE_MAP ListMap_Finish(PLISTFILE_MAP pListMap)
{
    PLISTFILE_ENTRY pListEntry;
    PCASC_MAP pMap;
    LPBYTE pbEntry;

    // Sanity check
    assert(pListMap->pNameMap == NULL);
    
    // Create the map
    pListMap->pNameMap = pMap = Map_Create((DWORD)pListMap->nEntries, sizeof(ULONGLONG), 0);
    if(pListMap->pNameMap == NULL)
    {
        ListFile_FreeMap(pListMap);
        return NULL;
    }

    // Fill the map
    pbEntry = (LPBYTE)(pListMap + 1);
    for(size_t i = 0; i < pListMap->nEntries; i++)
    {
        // Get the listfile entry
        pListEntry = (PLISTFILE_ENTRY)pbEntry;
        pbEntry += pListEntry->cbEntrySize;

        // Insert the entry to the map
        Map_InsertObject(pMap, pListEntry, &pListEntry->FileNameHash);
    }

    return pListMap;
}

PLISTFILE_MAP ListFile_CreateMap(const TCHAR * szListFile)
{
    PLISTFILE_MAP pListMap = NULL;
    void * pvListFile;
    char szFileName[MAX_PATH+1];
    size_t nLength;

    // Only if the listfile name has been given
    if(szListFile != NULL)
    {
        // Create map for the listfile
        pListMap = ListMap_Create();
        if(pListMap != NULL)
        {
            // Open the external listfile
            pvListFile = ListFile_OpenExternal(szListFile);
            if(pvListFile != NULL)
            {
                // Go through the entire listfile and insert each name to the map
                while((nLength = ListFile_GetNext(pvListFile, "*", szFileName, MAX_PATH)) != 0)
                {
                    // Insert the file name to the map
                    pListMap = ListMap_InsertName(pListMap, szFileName, nLength);
                    if(pListMap == NULL)
                        break;
                }

                // Finish the listfile map
                pListMap = ListMap_Finish(pListMap);

                // Free the listfile
                ListFile_Free(pvListFile);
            }
        }
    }

    // Return the created map
    return pListMap;
}

const char * ListFile_FindName(PLISTFILE_MAP pListMap, ULONGLONG FileNameHash)
{
    PLISTFILE_ENTRY pListEntry = NULL;

    if(pListMap != NULL)
        pListEntry = (PLISTFILE_ENTRY)Map_FindObject(pListMap->pNameMap, &FileNameHash, NULL);
    return (pListEntry != NULL) ? pListEntry->szFileName : "";
}

void ListFile_FreeMap(PLISTFILE_MAP pListMap)
{
    if(pListMap != NULL)
    {
        if(pListMap->pNameMap != NULL)
            Map_Free(pListMap->pNameMap);
        CASC_FREE(pListMap);
    }
}
