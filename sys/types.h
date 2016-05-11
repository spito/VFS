// -*- C++ -*- (c) 2015 Jiří Weiser

#ifndef _SYS_TYPES_H_
#define _SYS_TYPES_H_

// size_t
#include <stddef.h>

#if !defined( __divine__ ) && !defined( __divinecc__ )
#if defined( __MAC_OS_X_VERSION_MAX_ALLOWED )
typedef __darwin_socklen_t         socklen_t;
#else
typedef __socklen_t         socklen_t;
#endif
#else
#include "bits/types.h"


typedef __intptr_t          ssize_t;

typedef __int32_t           pid_t;


typedef __uintptr_t         ptr_t;
typedef __uint32_t          socklen_t;

typedef __uint32_t          uid_t;
typedef __uint32_t          gid_t;

typedef __int32_t           off_t;
typedef __int64_t           off64_t;

typedef __uint32_t          mode_t;

typedef __uint32_t          dev_t;
typedef __uint32_t          ino_t;

typedef __uint32_t          nlink_t;
typedef __uint32_t          blksize_t;
typedef __uint32_t          blkcnt_t;

/* Convenience types.  */
typedef __u_char            u_char;
typedef __u_short           u_short;
typedef __u_int             u_int;
typedef __u_long            u_long;
typedef __uint16_t          u_int16_t;
typedef __uint32_t          u_int32_t;

#endif

#endif // _SYS_TYPES_H_
