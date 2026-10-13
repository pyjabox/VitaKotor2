/* test_modstack.c -- drives loader/modstack.c on the host against a folder
 * laid out like the memory card, for tools/test_modstack.py to check against
 * its own model of the rules. Not built into the VPK.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -I tools/hoststubs/sceio -I loader \
 *      -o /tmp/test_modstack tools/test_modstack.c loader/modstack.c loader/k2res.c -lz
 * Run (tools/test_modstack.py does this):
 *   HOST_UX0=<card root> /tmp/test_modstack <tag> <engine folder> [translation dir] < requests
 * Requests, one per line: "R <path>" resolve, "L <path>" list a folder,
 * "F <rel>" find a file of the view, "T" the talk table. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "modstack.h"

void log_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("# ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: test_modstack <tag> <engine folder> [translation dir]\n");
    return 2;
  }
  modstack_scan();
  modstack_build(argv[1], argv[2], argc > 3 ? argv[3] : NULL);
  char line[1024], out[1024], member[128];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == 'R') {
      out[0] = 0;
      int r = modstack_resolve(line + 2, out, sizeof out);
      printf("R %d %s\n", r, out);
    } else if (line[0] == 'L') {
      void *d = modstack_opendir(line + 2);
      const char *n;
      int dir;
      while (d && (n = modstack_readdir(d, &dir))) printf("L %s%s\n", n, dir ? "/" : "");
      modstack_closedir(d);
      printf("L.\n");
    } else if (line[0] == 'F') {
      out[0] = 0;
      printf("F %d %s\n", modstack_find(line + 2, out, sizeof out), out);
    } else if (line[0] == 'T') {
      out[0] = member[0] = 0;
      int k = modstack_tlk(out, sizeof out, member, sizeof member);
      printf("T %d %s\n", k, k == 1 ? out : member);
    }
    fflush(stdout);
  }
  return 0;
}
