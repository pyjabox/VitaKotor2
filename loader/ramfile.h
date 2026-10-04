/* ramfile.h -- read-only files served from RAM (ramfile.c). */

#ifndef __RAMFILE_H__
#define __RAMFILE_H__

#include <stdio.h>

/* From fopen: a stream over the file's RAM copy, or NULL to open it as usual. */
FILE *ramfile_open(const char *path, const char *mode);
/* From fclose: 1 if f was one of ours (now closed). */
int ramfile_close(FILE *f);

#endif
