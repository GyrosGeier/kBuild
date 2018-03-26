/*-
 * Copyright (c) 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)extern.h	8.2 (Berkeley) 4/1/94
 * $FreeBSD: src/bin/cp/extern.h,v 1.19 2004/04/06 20:06:44 markm Exp $
 */

#include "kmkbuiltin.h" /* for PATH_MAX on GNU/hurd */

typedef struct {
	char	*p_end;			/* pointer to NULL at end of path */
	char	*target_end;		/* pointer to end of target base */
	char	p_path[PATH_MAX];	/* pointer to the start of a path */
} PATH_T;

#define argv0   cp_argv0
#define to      cp_to
#define fflag   cp_fflag
#define iflag   cp_iflag
#define nflag   cp_nflag
#define pflag   cp_pflag
#define vflag   cp_vflag
#define info    cp_info
#define usage   cp_usage
#define setfile cp_setfile

extern const char *argv0;
extern PATH_T to;
extern int fflag, iflag, nflag, pflag, vflag;
extern volatile sig_atomic_t info;

int	copy_fifo(PKMKBUILTINCTX pCtx, struct stat *, int);
int	copy_file(PKMKBUILTINCTX pCtx, const FTSENT *, int, int, int *);
int	copy_link(PKMKBUILTINCTX pCtx, const FTSENT *, int);
int	copy_special(PKMKBUILTINCTX pCtx, struct stat *, int);
int	setfile(PKMKBUILTINCTX pCtx, struct stat *, int);
