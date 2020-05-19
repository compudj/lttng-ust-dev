/*
 * Copyright (C) 2009  Pierre-Marc Fournier
 * Copyright (C) 2011  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stddef.h>

/* write() */
#include <unistd.h>

/* writev() */
#include <sys/uio.h>

/* send() */
#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>

#include <share.h>

/*
 * This write is patient because it restarts if it was incomplete.
 */

ssize_t patient_write(int fd, const void *buf, size_t count)
{
	const char *bufc = (const char *) buf;
	int result;

	for(;;) {
		result = write(fd, bufc, count);
		if (result == -1 && errno == EINTR) {
			continue;
		}
		if (result <= 0) {
			return result;
		}
		count -= result;
		bufc += result;

		if (count == 0) {
			break;
		}
	}

	return bufc-(const char *)buf;
}

/*
 * The `struct iovec *iov` is not `const` because we modify it to support
 * partial writes.
 */
ssize_t patient_writev(int fd, struct iovec *iov, int iovcnt)
{
	ssize_t written, total_written = 0;
	int curr_element_idx = 0;

	for(;;) {
		written = writev(fd, iov + curr_element_idx,
				iovcnt - curr_element_idx);
		if (written == -1 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return written;
		}

		total_written += written;

		/*
		 * If it's not the last element in the vector and we have
		 * written more than the current element size, then increment
		 * the current element index until we reach the element that
		 * was partially written.
		 */
		while (curr_element_idx < iovcnt &&
				written >= iov[curr_element_idx].iov_len) {
			written -= iov[curr_element_idx].iov_len;
			curr_element_idx++;
		}

		/* Maybe we are done. */
		if (curr_element_idx >= iovcnt) {
			break;
		}

		/* Update the current element base and size. */
		iov[curr_element_idx].iov_base += written;
		iov[curr_element_idx].iov_len -= written;
	}

	return total_written;
}

ssize_t patient_send(int fd, const void *buf, size_t count, int flags)
{
	const char *bufc = (const char *) buf;
	int result;

	for(;;) {
		result = send(fd, bufc, count, flags);
		if (result == -1 && errno == EINTR) {
			continue;
		}
		if (result <= 0) {
			return result;
		}
		count -= result;
		bufc += result;

		if (count == 0) {
			break;
		}
	}

	return bufc - (const char *) buf;
}
