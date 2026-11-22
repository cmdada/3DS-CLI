#ifndef CORE_PREALLOC_H
#define CORE_PREALLOC_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "plat.h"

static inline bool file_prealloc(FILE *f, off_t len) {
#ifdef PLAT_FS_ZERO_FILLS
  (void)f; (void)len;
  return false;
#else
  if (fflush(f) != 0) return false;
  long pos = ftell(f);
  if (pos < 0) return false;
  bool ok = ftruncate(fileno(f), len) == 0;
  /* wut seek desync */
  fseek(f, pos + 1, SEEK_SET);
  fseek(f, pos, SEEK_SET);
  return ok;
#endif
}

#endif /* CORE_PREALLOC_H */
