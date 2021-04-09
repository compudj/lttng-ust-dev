/*	$OpenBSD: wsetup.c,v 1.7 2005/08/08 08:05:36 espie Exp $ */
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (C) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "local.h"

/*
 * Various output routines call wsetup to be sure it is safe to write,
 * because either _flags does not include __SWR, or _buf is NULL.
 * _wsetup returns 0 if OK to write, nonzero otherwise.
 */
int
__swsetup(LTTNG_UST_LFILE *fp)
{
	/* make sure stdio is set up */
//	if (!__sdidinit)
//		__sinit();

	/*
	 * If we are not writing, we had better be reading and writing.
	 */
	if ((fp->_flags & __SWR) == 0) {
		if ((fp->_flags & __SRW) == 0)
			return (EOF);
		if (fp->_flags & __SRD) {
			/* clobber any ungetc data */
			if (HASUB(fp))
				FREEUB(fp);
			fp->_flags &= ~(__SRD|__SEOF);
			fp->_r = 0;
			fp->_p = fp->_bf._base;
		}
		fp->_flags |= __SWR;
	}

	/*
	 * Make a buffer if necessary, then set _w.
	 */
	if (fp->_bf._base == NULL) {
//		if ((fp->_flags & (__SSTR | __SALC)) == __SSTR)
//			return (EOF);
//		__smakebuf(fp);
		assert(0);
	}
	if (fp->_flags & __SLBF) {
		/*
		 * It is line buffered, so make _lbfsize be -_bufsize
		 * for the putc() macro.  We will change _lbfsize back
		 * to 0 whenever we turn off __SWR.
		 */
		fp->_w = 0;
		fp->_lbfsize = -fp->_bf._size;
	} else
		fp->_w = fp->_flags & __SNBF ? 0 : fp->_bf._size;
	return (0);
}
