#include "audio/AudioClipReader.h"

#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aud
{
    bool FileClipReader::Size(const std::string& path, unsigned int& bytes)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const DWORD size = GetFileSize(file, NULL);
        CloseHandle(file);
        if (size == INVALID_FILE_SIZE || size == 0)
            return false;
        bytes = size;
        return true;
    }

    bool FileClipReader::Read(const std::string& path, unsigned int offset,
                              unsigned int length, std::vector<unsigned char>& out)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        unsigned int size = length;
        if (size == 0)
        {
            const DWORD fileSize = GetFileSize(file, NULL);
            if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
            {
                CloseHandle(file);
                return false;
            }
            size = fileSize;
        }

        LONG highOffset = 0;
        SetLastError(NO_ERROR);
        if (SetFilePointer(file, (LONG)offset, &highOffset, FILE_BEGIN) ==
            INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        {
            CloseHandle(file);
            return false;
        }

        out.resize(size);
        DWORD read = 0;
        const BOOL ok = ReadFile(file, &out[0], size, &read, NULL);
        CloseHandle(file);
        if (!ok || read != size)
        {
            out.clear();
            return false;
        }
        return true;
    }

}