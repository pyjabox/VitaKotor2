/* fdtrack.h -- count the files and folders the process holds open (fdtrack.c). */
#ifndef FDTRACK_H
#define FDTRACK_H

/* One [fd] line: live files and folders, peaks, totals, failures. */
void fdtrack_report(void);

#endif
