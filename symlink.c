/*
 Released under MIT License

 Copyright (c) 2021 Glenn Burkhardt.

 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */

/* Not designed for Unicode filesystems.
 */
#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <windows.h>
#include "symlink.h"

static void setErrno(const char* funcName)
{
    DWORD err = GetLastError();

    switch (err) {
      case ERROR_FILE_NOT_FOUND:    errno = ENOENT;  break;
      case ERROR_ACCESS_DENIED:     errno = EACCES;  break;
      case ERROR_ALREADY_EXISTS:
      case ERROR_FILE_EXISTS:       errno = EEXIST;  break;
      case ERROR_PATH_NOT_FOUND:    errno = ENAMETOOLONG;  break;
      case ERROR_NOT_ENOUGH_MEMORY: errno = ENOMEM;  break;
      case ERROR_NOT_SAME_DEVICE:   errno = EPERM;  break;
      default:
        {
            char* msg = 0;
            FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER
                          |FORMAT_MESSAGE_FROM_SYSTEM
                          | FORMAT_MESSAGE_IGNORE_INSERTS,
                          0, err, 0, (LPTSTR)&msg, 0, 0);
            if (msg) {
                fprintf(stderr, "%s: %s", funcName, msg);
                LocalFree(msg);
            } else {
                fprintf(stderr, "%s: error %#lx\n", funcName, err);
            }
            errno = EIO;
        }
        break;
    }
}

char* realpath(const char *path, char *resolved_path)
{
    HANDLE hPath = CreateFile(path, 0, 0, 0, OPEN_EXISTING, 0, 0);
    if (hPath == INVALID_HANDLE_VALUE) {
        setErrno("realpath");
        return 0;
    }

    DWORD s;
    char *fn;
    if (!resolved_path) {
        // get pathname size
        s = GetFinalPathNameByHandleA(hPath, 0, 0, FILE_NAME_OPENED);
        if (!s) {
            setErrno("realpath");
            return 0;
        }

        fn = (char*)malloc(++s);
    } else {
        char buf[PATH_MAX+4+1];
        s = sizeof(buf);
        fn = buf;
    }

    GetFinalPathNameByHandleA(hPath, fn, s, FILE_NAME_OPENED);
    CloseHandle(hPath);

    if (s && !memcmp(fn, "\\\\?\\", 4)){
        memcpy(fn, fn+4, strlen(fn)+1);
    }

    if (resolved_path) {
        strcpy(resolved_path, fn);
        return resolved_path;
    } else {
        return fn;
    }
}

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_reparse_data_buffer

typedef struct _REPARSE_DATA_BUFFER {
  ULONG  ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  union {
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      ULONG  Flags;
      WCHAR  PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR  PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  } DUMMYUNIONNAME;
} _REPARSE_DATA_BUFFER;

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    DWORD st = GetFileAttributesA(path);
    if (st == INVALID_FILE_ATTRIBUTES) {
        setErrno("readlink");
        return -1;
    }
    if (!(st & FILE_ATTRIBUTE_REPARSE_POINT)) {
        errno = EINVAL;
        return -1;
    }

    HANDLE handle = CreateFileA(path, 0, 0, 0, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        setErrno("readlink");
        return -1;
    }

    // If the filesystem is Unicode, MAX_PATH is 32,767.  If the buffer
    // isn't big enough, 'DeviceIoControl' will fail.
    char rdbbuf[sizeof(_REPARSE_DATA_BUFFER) + MAX_PATH*2];
    DWORD sz;
    
    int s = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT,
                            0, 0, rdbbuf, sizeof(rdbbuf), &sz, 0);
    CloseHandle(handle);

    if (!s) {
        setErrno("readlink");
        return -1;
    }

    _REPARSE_DATA_BUFFER *rdb = (_REPARSE_DATA_BUFFER*)rdbbuf;
    
    wchar_t *buffer;
    size_t offset, len;
    switch (rdb->ReparseTag)
    {
      case IO_REPARSE_TAG_MOUNT_POINT:
        buffer = rdb->MountPointReparseBuffer.PathBuffer;
        offset = rdb->MountPointReparseBuffer.PrintNameOffset;
        len = rdb->MountPointReparseBuffer.PrintNameLength;
        break;
      case IO_REPARSE_TAG_SYMLINK:
        buffer = rdb->SymbolicLinkReparseBuffer.PathBuffer;
        offset = rdb->SymbolicLinkReparseBuffer.PrintNameOffset;
        len = rdb->SymbolicLinkReparseBuffer.PrintNameLength;
        // Note: iff rdb->SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE
        //       -> resulting path is relative to the source
        break;
      default:
        errno = EINVAL;
        return -1;
    }

    len /= sizeof(wchar_t);
    offset /= sizeof(wchar_t);

    // Convert to ASCII
    int i;
    for (i=0; i < min(len, bufsiz-1); i++)
        wctomb(&buf[i], buffer[offset++]);

    buf[i++] = 0;

    return i;
}

/* Returns:
   -1 : failed
    0 : not a sym link
    1 : is a sym link
*/
int isSymLink(const char *path)
{
    WIN32_FIND_DATAA wd;
    HANDLE h = FindFirstFile(path, &wd);
    if (h == INVALID_HANDLE_VALUE) {
        setErrno("isSymLink");
        return -1;
    }

    CloseHandle(h);

    return ((wd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) 
        && (wd.dwReserved0 == IO_REPARSE_TAG_SYMLINK)) ? 1:0;
}

/*
  https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/stat-functions?view=msvc-160

  "_stat does work correctly with symbolic links."
*/
int lstat(const char *path, struct stat *buf)
{
    struct  _stat64 st;

    int s = _stat64(path, &st); // sets errno on failure

    buf->st_dev = st.st_dev;
    buf->st_ino = st.st_ino;
    buf->st_mode = st.st_mode;
    buf->st_nlink = st.st_nlink;
    buf->st_uid = st.st_uid;
    buf->st_gid = st.st_gid;
    buf->st_rdev = st.st_rdev;
    buf->st_size = (_off_t) st.st_size;
    buf->st_atime = st.st_atime;
    buf->st_mtime = st.st_mtime;
    buf->st_ctime = st.st_ctime;

    int is = isSymLink(path);
    if (is > 0) {
        buf->st_mode |= S_IFLNK;
        // on Linux, links to directories report as regular files
        buf->st_mode &= ~S_IFDIR;
    } else if (is < 0) {
        return -1;
    }

    return s;
}

int symlink(const char *oldpath, const char *newpath)
{
    DWORD dwflags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    struct stat statbuf;
        
    int s = stat(oldpath, &statbuf);
    if (s) return s;

    if (S_ISDIR(statbuf.st_mode))
        dwflags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

    s = CreateSymbolicLinkA(newpath, oldpath, dwflags);

    if (s)
        return 0;
    else {
        setErrno("symlink");
        return -1;
    }
}

// hard link
// https://docs.microsoft.com/en-us/windows/win32/fileio/hard-links-and-junctions
int link(const char *oldpath, const char *newpath)
{
    struct stat statbuf;
        
    int s = stat(oldpath, &statbuf);
    if (s) return s;

    if (S_ISDIR(statbuf.st_mode)) {
        errno = EPERM;  // hard links for directories not supported on NTFS
        return -1;
    }

    s = CreateHardLinkA(newpath, oldpath, 0);

    if (s)
        return 0;
    else {
        setErrno("link");
        return -1;
    }
}
