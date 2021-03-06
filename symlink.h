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
#ifndef _SYMLINK_H
#define _SYMLINK_H

#include <stdbool.h>
#include <sys/stat.h>

/* Not designed for Unicode filesystems.
 */

#define S_IFLNK (S_IFREG | S_IFCHR)
#define S_ISLNK(m) ((m & S_IFMT) == S_IFLNK)

#ifdef  __cplusplus
extern "C" {
#endif

char* realpath(const char *path, char *resolved_path);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int lstat(const char *path, struct stat *buf);
int symlink(const char *oldpath, const char *newpath);
int link(const char *oldpath, const char *newpath);


/* Returns:
   -1 : failed
    0 : not a sym link
    1 : is a sym link
*/
int isSymLink(const char *path);

#ifdef __cplusplus
}
#endif

#endif
