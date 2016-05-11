// Taken from MUSL library, edited by Vladimir Still

#ifndef	_SYS_MMAN_H
#define	_SYS_MMAN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <features.h>

#define __NEED_size_t

#include <bits/alltypes.h>

#define MAP_FAILED ((void *) -1)

#define MAP_SHARED     0x01
#define MAP_ANON       0x20
#define MAP_ANONYMOUS  MAP_ANON

#define PROT_NONE      0
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4

void *__mmap_anon( size_t );

#ifdef __cplusplus
}
#endif
#endif
