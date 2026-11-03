/* voice_close.h -- close the voice line a destroyed sound source forgets (voice_close.c). */
#ifndef VOICE_CLOSE_H
#define VOICE_CLOSE_H

/* Takes CExoStreamingSoundSourceInternal's destructor; after so_resolve. */
void voice_close_install(void);

#endif
