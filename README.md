# MingGW-Compatibility-Functions

Posix compatibility functions for managing symbolic links with MinGW-w64.

  int link(const char *oldpath, const char *newpath);

  int lstat(const char *path, struct stat *buf);

  ssize_t readlink(const char *path, char *buf, size_t bufsiz);

  char* realpath(const char *path, char *resolved_path);

  int symlink(const char *oldpath, const char *newpath);

Add functionality for CLOCK_MONOTONIC to clock_nanosleep()
