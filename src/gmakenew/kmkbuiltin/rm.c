/*-
 * Copyright (c) 1990, 1993, 1994
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
 */

#if 0
#ifndef lint
static const char copyright[] =
"@(#) Copyright (c) 1990, 1993, 1994\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)rm.c	8.5 (Berkeley) 4/18/94";
#endif /* not lint */
#include <sys/cdefs.h>
//__FBSDID("$FreeBSD: src/bin/rm/rm.c,v 1.47 2004/04/06 20:06:50 markm Exp $");
#endif
#ifdef __EMX__
# define DO_RMTREE
#endif

#include <sys/stat.h>
#ifndef _MSC_VER
# include <sys/param.h>
# include <sys/mount.h>
#endif

#include "err.h"
#include <errno.h>
#include <fcntl.h>
#ifdef DO_RMTREE
# include <fts.h>
#endif
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#ifdef __sun__
# include "getopt.h"
#endif
#ifdef _MSC_VER
# include "mscfakes.h"
#endif

#ifdef __EMX__
#undef S_IFWHT
#undef S_ISWHT
#endif
#ifndef S_IFWHT
#define S_IFWHT 0
#define S_ISWHT(s) 0
#define undelete(s) (-1)
#endif

#if !defined(__FreeBSD__) && !defined(__APPLE__)
extern void strmode(mode_t mode, char *p);
#endif

static int dflag, eval, fflag, iflag, Pflag, vflag, Wflag, stdin_ok;
static uid_t uid;

static char *argv0;

static int	check(char *, char *, struct stat *);
static void	checkdot(char **);
static void	rm_file(char **);
static int	rm_overwrite(char *, struct stat *);
#ifdef DO_RMTREE
static void	rm_tree(char **);
#endif
static int	usage(void);

/*
 * rm --
 *	This rm is different from historic rm's, but is expected to match
 *	POSIX 1003.2 behavior.  The most visible difference is that -f
 *	has two specific effects now, ignore non-existent files and force
 * 	file removal.
 */
int
kmk_builtin_rm(int argc, char *argv[])
{
	int ch, rflag;
	char *p;

        /* reinitialize globals */
        argv0 = argv[0];
        dflag = eval = fflag = iflag = Pflag = vflag = Wflag = stdin_ok = 0;
        uid = 0;

        /* kmk: reset getopt and set program name. */
        g_progname = argv[0];
        opterr = 1;
        optarg = NULL;
        optopt = 0;
#if defined(__FreeBSD__) || defined(__EMX__) || defined(__APPLE__)
        optreset = 1;
        optind = 1;
#else
        optind = 0; /* init */
#endif

#if 0 /* kmk: we don't need this */
	/*
	 * Test for the special case where the utility is called as
	 * "unlink", for which the functionality provided is greatly
	 * simplified.
	 */
	if ((p = rindex(argv[0], '/')) == NULL)
		p = argv[0];
	else
		++p;
	if (strcmp(p, "unlink") == 0) {
		while (getopt(argc, argv, "") != -1)
			return usage();
		argc -= optind;
		argv += optind;
		if (argc != 1)
			return usage();
		rm_file(&argv[0]);
		return eval;
	}
#else
        (void)p;
#endif
	Pflag = rflag = 0;
	while ((ch = getopt(argc, argv, "dfiPRrvW")) != -1)
		switch(ch) {
		case 'd':
			dflag = 1;
			break;
		case 'f':
			fflag = 1;
			iflag = 0;
			break;
		case 'i':
			fflag = 0;
			iflag = 1;
			break;
		case 'P':
			Pflag = 1;
			break;
		case 'R':
		case 'r':			/* Compatibility. */
#ifdef DO_RMTREE
			rflag = 1;
			break;
#else
			errno = EINVAL;
			return err(1, "Recursion is not supported!");
#endif
		case 'v':
			vflag = 1;
			break;
#ifdef FTS_WHITEOUT
		case 'W':
			Wflag = 1;
			break;
#endif
		default:
			return usage();
		}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		if (fflag)
			return (0);
		return usage();
	}

	checkdot(argv);
	uid = geteuid();

	if (*argv) {
		stdin_ok = isatty(STDIN_FILENO);
#ifdef DO_RMTREE
		if (rflag)
			rm_tree(argv);
		else
#endif
			rm_file(argv);
	}

	return eval;
}

#ifdef DO_RMTREE
static void
rm_tree(char **argv)
{
	FTS *fts;
	FTSENT *p;
	int needstat;
	int flags;
	int rval;

	/*
	 * Remove a file hierarchy.  If forcing removal (-f), or interactive
	 * (-i) or can't ask anyway (stdin_ok), don't stat the file.
	 */
	needstat = !uid || (!fflag && !iflag && stdin_ok);

	/*
	 * If the -i option is specified, the user can skip on the pre-order
	 * visit.  The fts_number field flags skipped directories.
	 */
#define	SKIPPED	1

	flags = FTS_PHYSICAL;
	if (!needstat)
		flags |= FTS_NOSTAT;
#ifdef FTS_WHITEOUT
	if (Wflag)
		flags |= FTS_WHITEOUT;
#endif
	if (!(fts = fts_open(argv, flags, NULL))) {
		eval = err(1, "fts_open");
		return;
	}
	while ((p = fts_read(fts)) != NULL) {
		switch (p->fts_info) {
		case FTS_DNR:
			if (!fflag || p->fts_errno != ENOENT) {
				fprintf(stderr, "%s: %s: %s\n",
				        argv0, p->fts_path, strerror(p->fts_errno));
				eval = 1;
			}
			continue;
		case FTS_ERR:
			eval = errx(1, "%s: %s", p->fts_path, strerror(p->fts_errno));
                        return;
		case FTS_NS:
			/*
			 * Assume that since fts_read() couldn't stat the
			 * file, it can't be unlinked.
			 */
			if (!needstat)
				break;
			if (!fflag || p->fts_errno != ENOENT) {
				fprintf(stderr, "%s: %s: %s\n",
				        argv0, p->fts_path, strerror(p->fts_errno));
				eval = 1;
			}
			continue;
		case FTS_D:
			/* Pre-order: give user chance to skip. */
			if (!fflag && !check(p->fts_path, p->fts_accpath,
			    p->fts_statp)) {
				(void)fts_set(fts, p, FTS_SKIP);
				p->fts_number = SKIPPED;
			}
#ifdef UF_APPEND
			else if (!uid &&
				 (p->fts_statp->st_flags & (UF_APPEND|UF_IMMUTABLE)) &&
				 !(p->fts_statp->st_flags & (SF_APPEND|SF_IMMUTABLE)) &&
				 chflags(p->fts_accpath,
					 p->fts_statp->st_flags &= ~(UF_APPEND|UF_IMMUTABLE)) < 0)
				goto err;
#endif
			continue;
		case FTS_DP:
			/* Post-order: see if user skipped. */
			if (p->fts_number == SKIPPED)
				continue;
			break;
		default:
			if (!fflag &&
			    !check(p->fts_path, p->fts_accpath, p->fts_statp))
				continue;
		}

		rval = 0;
#ifdef UF_APPEND
		if (!uid &&
		    (p->fts_statp->st_flags & (UF_APPEND|UF_IMMUTABLE)) &&
		    !(p->fts_statp->st_flags & (SF_APPEND|SF_IMMUTABLE)))
			rval = chflags(p->fts_accpath,
				       p->fts_statp->st_flags &= ~(UF_APPEND|UF_IMMUTABLE));
#endif
		if (rval == 0) {
			/*
			 * If we can't read or search the directory, may still be
			 * able to remove it.  Don't print out the un{read,search}able
			 * message unless the remove fails.
			 */
			switch (p->fts_info) {
			case FTS_DP:
			case FTS_DNR:
				rval = rmdir(p->fts_accpath);
				if (rval == 0 || (fflag && errno == ENOENT)) {
					if (rval == 0 && vflag)
						(void)printf("%s\n",
						    p->fts_path);
					continue;
				}
				break;

#ifdef FTS_W
			case FTS_W:
				rval = undelete(p->fts_accpath);
				if (rval == 0 && (fflag && errno == ENOENT)) {
					if (vflag)
						(void)printf("%s\n",
						    p->fts_path);
					continue;
				}
				break;
#endif

			case FTS_NS:
				/*
				 * Assume that since fts_read() couldn't stat
				 * the file, it can't be unlinked.
				 */
				if (fflag)
					continue;
				/* FALLTHROUGH */
			default:
				if (Pflag)
					if (!rm_overwrite(p->fts_accpath, NULL))
						continue;
				rval = unlink(p->fts_accpath);
#ifdef _MSC_VER
				if (rval != 0) {
    					chmod(p->fts_accpath, 0777);
					rval = unlink(p->fts_accpath);
				}
#endif

				if (rval == 0 || (fflag && errno == ENOENT)) {
					if (rval == 0 && vflag)
						(void)printf("%s\n",
						    p->fts_path);
					continue;
				}
			}
		}
err:
		fprintf(stderr, "%s: %s: %s\n", argv0, p->fts_path, strerror(errno));
		eval = 1;
	}
	if (errno) {
		fprintf(stderr, "%s: fts_read: %s\n", argv0, strerror(errno));
                eval = 1;
        }
}
#endif /* DO_RMTREE */

static void
rm_file(char **argv)
{
	struct stat sb;
	int rval;
	char *f;

	/*
	 * Remove a file.  POSIX 1003.2 states that, by default, attempting
	 * to remove a directory is an error, so must always stat the file.
	 */
	while ((f = *argv++) != NULL) {
		/* Assume if can't stat the file, can't unlink it. */
		if (lstat(f, &sb)) {
#ifdef FTS_WHITEOUT
			if (Wflag) {
				sb.st_mode = S_IFWHT|S_IWUSR|S_IRUSR;
			} else {
#else
			{
#endif
				if (!fflag || errno != ENOENT) {
					fprintf(stderr, "%s: %s: %s\n", argv0, f, strerror(errno));
					eval = 1;
				}
				continue;
			}
#ifdef FTS_WHITEOUT
		} else if (Wflag) {
			fprintf(stderr, "%s: %s: %s\n", argv0, f, strerror(EEXIST));
			eval = 1;
			continue;
#endif
		}

		if (S_ISDIR(sb.st_mode) && !dflag) {
			fprintf(stderr, "%s: %s: is a directory\n", argv0, f);
			eval = 1;
			continue;
		}
		if (!fflag && !S_ISWHT(sb.st_mode) && !check(f, f, &sb))
			continue;
		rval = 0;
#ifdef UF_APPEND
		if (!uid &&
		    (sb.st_flags & (UF_APPEND|UF_IMMUTABLE)) &&
		    !(sb.st_flags & (SF_APPEND|SF_IMMUTABLE)))
			rval = chflags(f, sb.st_flags & ~(UF_APPEND|UF_IMMUTABLE));
#endif
		if (rval == 0) {
			if (S_ISWHT(sb.st_mode))
				rval = undelete(f);
			else if (S_ISDIR(sb.st_mode))
				rval = rmdir(f);
			else {
				if (Pflag)
					if (!rm_overwrite(f, &sb))
						continue;
				rval = unlink(f);
#ifdef _MSC_VER
				if (rval != 0) {
    					chmod(f, 0777);
					rval = unlink(f);
				}
#endif
			}
		}
		if (rval && (!fflag || errno != ENOENT)) {
			fprintf(stderr, "%s: %s: %s\n", argv0, f, strerror(errno));
			eval = 1;
		}
		if (vflag && rval == 0)
			(void)printf("%s\n", f);
	}
}

/*
 * rm_overwrite --
 *	Overwrite the file 3 times with varying bit patterns.
 *
 * XXX
 * This is a cheap way to *really* delete files.  Note that only regular
 * files are deleted, directories (and therefore names) will remain.
 * Also, this assumes a fixed-block file system (like FFS, or a V7 or a
 * System V file system).  In a logging file system, you'll have to have
 * kernel support.
 */
static int
rm_overwrite(char *file, struct stat *sbp)
{
	struct stat sb;
#ifdef HAVE_FSTATFS
	struct statfs fsb;
#endif
	off_t len;
	int bsize, fd, wlen;
	char *buf = NULL;

	fd = -1;
	if (sbp == NULL) {
		if (lstat(file, &sb))
			goto err;
		sbp = &sb;
	}
	if (!S_ISREG(sbp->st_mode))
		return (1);
	if ((fd = open(file, O_WRONLY, 0)) == -1)
		goto err;
#ifdef HAVE_FSTATFS
	if (fstatfs(fd, &fsb) == -1)
		goto err;
	bsize = MAX(fsb.f_iosize, 1024);
#elif defined(HAVE_ST_BLKSIZE)
	bsize = MAX(sb.st_blksize, 1024);
#else
	bsize = 1024;
#endif
	if ((buf = malloc(bsize)) == NULL)
		exit(err(1, "%s: malloc", file));

#define	PASS(byte) {							\
	memset(buf, byte, bsize);					\
	for (len = sbp->st_size; len > 0; len -= wlen) {		\
		wlen = len < bsize ? len : bsize;			\
		if (write(fd, buf, wlen) != wlen)			\
			goto err;					\
	}								\
}
	PASS(0xff);
	if (fsync(fd) || lseek(fd, (off_t)0, SEEK_SET))
		goto err;
	PASS(0x00);
	if (fsync(fd) || lseek(fd, (off_t)0, SEEK_SET))
		goto err;
	PASS(0xff);
	if (!fsync(fd) && !close(fd)) {
		free(buf);
		return (1);
	}

err:	eval = 1;
	if (buf)
		free(buf);
	if (fd != -1)
		close(fd);
	fprintf(stderr, "%s: %s: %s\n", argv0, file, strerror(errno));
	return (0);
}


static int
check(char *path, char *name, struct stat *sp)
{
	int ch, first;
	char modep[15], *flagsp;

	/* Check -i first. */
	if (iflag)
		(void)fprintf(stderr, "remove %s? ", path);
	else {
		/*
		 * If it's not a symbolic link and it's unwritable and we're
		 * talking to a terminal, ask.  Symbolic links are excluded
		 * because their permissions are meaningless.  Check stdin_ok
		 * first because we may not have stat'ed the file.
		 * Also skip this check if the -P option was specified because
		 * we will not be able to overwrite file contents and will
		 * barf later.
		 */
		if (!stdin_ok || S_ISLNK(sp->st_mode) || Pflag ||
		    (!access(name, W_OK) &&
#ifdef SF_APPEND
		    !(sp->st_flags & (SF_APPEND|SF_IMMUTABLE)) &&
		    (!(sp->st_flags & (UF_APPEND|UF_IMMUTABLE)) || !uid))
#else
                    1)
#endif
                    )
			return (1);
		strmode(sp->st_mode, modep);
#ifdef SF_APPEND
		if ((flagsp = fflagstostr(sp->st_flags)) == NULL)
			exit(err(1, "fflagstostr"));
                (void)fprintf(stderr, "override %s%s%s/%s %s%sfor %s? ",
                    modep + 1, modep[9] == ' ' ? "" : " ",
                    user_from_uid(sp->st_uid, 0),
                    group_from_gid(sp->st_gid, 0),
                    *flagsp ? flagsp : "", *flagsp ? " " : "",
                    path);
		free(flagsp);
#else
                (void)flagsp;
                (void)fprintf(stderr, "override %s%s %d/%d for %s? ",
                    modep + 1, modep[9] == ' ' ? "" : " ",
                    sp->st_uid, sp->st_gid, path);
#endif
	}
	(void)fflush(stderr);

	first = ch = getchar();
	while (ch != '\n' && ch != EOF)
		ch = getchar();
	return (first == 'y' || first == 'Y');
}

#define ISDOT(a)	((a)[0] == '.' && (!(a)[1] || ((a)[1] == '.' && !(a)[2])))
static void
checkdot(char **argv)
{
	char *p, **save, **t;
	int complained;

	complained = 0;
	for (t = argv; *t;) {
		if ((p = strrchr(*t, '/')) != NULL)
			++p;
		else
			p = *t;
		if (ISDOT(p)) {
			if (!complained++)
				fprintf(stderr, "%s: \".\" and \"..\" may not be removed\n", argv0);
			eval = 1;
			for (save = t; (t[0] = t[1]) != NULL; ++t)
				continue;
			t = save;
		} else
			++t;
	}
}

static int
usage(void)
{

	(void)fprintf(stderr, "%s\n%s\n",
	    "usage: rm [-f | -i] [-dPRrvW] file ...\n",
	    "       unlink file");
	return EX_USAGE;
}