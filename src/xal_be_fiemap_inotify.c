#include <asm-generic/errno.h>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fs.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_inotify.h>

#define XAL_INOTIFY_NOCHANGE 0
#define XAL_INOTIFY_REINDEX 1

/* Bounds how long the watch thread sleeps when it has nothing to do. It also bounds how long
 * xal_be_fiemap_inotify_close() waits in pthread_join(), and how late an externally marked
 * dirty flag is noticed, so it trades those two against the wakeup rate. */
#define XAL_INOTIFY_POLL_TIMEOUT_MS 100

KHASH_MAP_INIT_INT64(wd_to_inode, struct xal_inode *);

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify)
{
	kh_wd_to_inode_t *inode_map;

	if (!inotify) {
		XAL_DEBUG("SKIPPED: No xal_inotify given")
		return;
	}

	inode_map = inotify->inode_map;

	/* Reap whether it was told to stop or exited on its own. RUNNING says a thread was created
	 * and has not exited, JOINABLE says watch_thread_id names one nobody has joined yet, and
	 * only the second makes the join safe. A thread that has already exited ignores the stop,
	 * so requesting it here costs nothing and is what keeps a live one from being joined
	 * forever. */
	if (atomic_load(&inotify->flag) & XAL_BE_FIEMAP_INOTIFY_JOINABLE) {
		atomic_store(&inotify->stop, true);
		pthread_join(inotify->watch_thread_id, NULL);
		atomic_fetch_and(&inotify->flag,
				 ~(XAL_BE_FIEMAP_INOTIFY_RUNNING | XAL_BE_FIEMAP_INOTIFY_JOINABLE));
	}

	if (inode_map) {
		kh_destroy(wd_to_inode, inode_map);
	}

	if (inotify->fd) {
		close(inotify->fd);
	}
}

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode)
{
	if (!inotify) {
		XAL_DEBUG("FAILED: No xal_inotify given");
		return -EINVAL;
	}

	inotify->watch_mode = watch_mode;
	atomic_init(&inotify->flag, 0);
	atomic_init(&inotify->stop, false);

	if (!inotify->watch_mode) {
		XAL_DEBUG("INFO: Skipping xal_be_fiemap_inotify_init(), watch mode none given");
		return 0;
	}

	inotify->fd = inotify_init1(IN_NONBLOCK);
	if (inotify->fd < 0) {
		XAL_DEBUG("FAILED: inotify_init1(); errno(%d)", errno);
		return -errno;
	}

	inotify->inode_map = kh_init(wd_to_inode);
	if (!inotify->inode_map) {
		XAL_DEBUG("FAILED: kh_init()");
		return -EINVAL;
	}

	return 0;
}

int
xal_be_fiemap_inotify_drain(struct xal_inotify *inotify)
{
	char buf[4096];
	ssize_t len;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given");
		return -EINVAL;
	}

	do {
		len = read(inotify->fd, buf, sizeof(buf));
	} while (len > 0);

	return 0;
}

int
xal_be_fiemap_inotify_clear_inode_map(struct xal_inotify *inotify)
{
	khash_t(wd_to_inode) *inode_map;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given");
		return -EINVAL;
	}

	inode_map = inotify->inode_map;

	kh_clear(wd_to_inode, inode_map);

	return 0;
}

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path, struct xal_inode *inode)
{
	khash_t(wd_to_inode) *inode_map;
	uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_UNMOUNT;
	khiter_t iter;
	int wd, err;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given.");
		return -EINVAL;
	}

	inode_map = inotify->inode_map;

	if (!xal_inode_is_dir(inode)) {
		XAL_DEBUG("FAILED: cannot process directory at path(%s) - not a directory", path);
		return -EINVAL;
	}

	if (inotify->watch_mode) {
		wd = inotify_add_watch(inotify->fd, path, mask);
		if (wd < 0) {
			XAL_DEBUG("FAILED: inotify_add_watch(); errno(%d)", errno);
			return -errno;
		}

		iter = kh_put(wd_to_inode, inode_map, wd, &err);
		if (err < 0) {
			XAL_DEBUG("FAILED: kh_put()");
			return -EIO;
		}
		kh_value(inode_map, iter) = inode;
	}

	return 0;
}

static __attribute__((unused)) int
inotify_event_mask_pp(uint32_t mask, char *str, int str_sz) {
	int wrtn, idx = 0;

	if (mask & IN_MODIFY) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_MODIFY");
		idx += wrtn;
	}
	if (mask & IN_ATTRIB) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_ATTRIB");
		idx += wrtn;
	}
	if (mask & IN_CREATE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_CREATE");
		idx += wrtn;
	}
	if (mask & IN_DELETE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_DELETE");
		idx += wrtn;
	}
	if (mask & IN_MOVE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_MOVE");
		idx += wrtn;
	}
	if (mask & IN_ISDIR) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_ISDIR");
		idx += wrtn;
	}
	if (mask & IN_CLOSE_WRITE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_CLOSE_WRITE");
		idx += wrtn;
	}
	if (mask & IN_UNMOUNT) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_UNMOUNT");
		idx += wrtn;
	}

	return wrtn;
}

/**
 * Drain the inotify queue and report whether the index must be rebuilt
 *
 * @return On success XAL_INOTIFY_NOCHANGE or XAL_INOTIFY_REINDEX is returned, the latter asking
 * the caller for a full re-index. On error, negative errno is returned and the watch is over.
 */
static int
check_events(struct xal_inotify *inotify)
{
	char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	ssize_t len, i;

	len = read(inotify->fd, buf, sizeof buf);
	while (len > 0) {
		i = 0;

		while (i < len) {
			struct inotify_event *event = (struct inotify_event *)&buf[i];
			__attribute__((unused)) char mask_pp[128];

			XAL_DEBUG_FCALL(inotify_event_mask_pp, event->mask, mask_pp, 128);
			/* len is 0 for events about the watched directory itself. */
			XAL_DEBUG("INFO: mask(%s) for event with wd(%d) and name(%s)", &mask_pp[1], event->wd,
				  event->len ? event->name : "(none)")

			/* Events were dropped: wd is -1 and no name is carried, so nothing here can
			 * say what changed. Checked first -- no watch mode can do better than a
			 * re-index. */
			if (event->mask & IN_Q_OVERFLOW) {
				XAL_DEBUG("INFO: inotify queue overflowed; events were lost");
				return XAL_INOTIFY_REINDEX;
			}

			/* The only fatal one: the filesystem is gone, so there is neither
			 * anything left to watch nor anything a re-index could recover. */
			if (event->mask & IN_UNMOUNT) {
				XAL_DEBUG("FAILED: File system has been unmounted");
				return -EINVAL;
			}

			/* Our own snapshot work lands in the watched mountpoint directory. What
			 * happens inside a shadow dir is invisible -- the walk never descends into
			 * one -- so dropping the event by name is what keeps a snapshot from
			 * reporting itself as a change. */
			if ((inotify->watch_mode == XAL_WATCHMODE_REFLINK_SNAPSHOT) && event->len &&
			    (strncmp(event->name, XAL_SNAPSHOT_PREFIX,
				     sizeof(XAL_SNAPSHOT_PREFIX) - 1) == 0)) {
				XAL_DEBUG("INFO: ignoring own snapshot dir; name(%s)", event->name);
				i += sizeof(struct inotify_event) + event->len;
				continue;
			}

			/* Every surviving event means the same thing. The clones hold the blocks,
			 * so an event cannot invalidate extents a reader already has; it only says
			 * a re-snapshot would see something different. No per-event incremental
			 * path: re-reading extents for the changed file would take them from the
			 * unpinned origin. */
			XAL_DEBUG("INFO: file system moved on under the snapshot");

			return XAL_INOTIFY_REINDEX;
		}

		len = read(inotify->fd, buf, sizeof buf);
	}

	return XAL_INOTIFY_NOCHANGE;
}

static void *
background_thread_start(void *arg)
{
	struct xal *xal = arg;
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct pollfd pfd;
	int cb_seq = -1;
	int err = 0;

	XAL_DEBUG("INFO: starting background thread");

	if (!be->inotify) {
		XAL_DEBUG("FAILED: inotify not initialised, exit thread");
		goto exit_thread;
	}

	pfd.fd = be->inotify->fd;
	pfd.events = POLLIN;

	while (!atomic_load(&be->inotify->stop)) {
		int state = atomic_load(xal->index_state);

		if (state == XAL_STATE_INDEXING) {
			/* Someone else is rebuilding the pools. Nothing here can make that
			 * finish sooner, so wait rather than spin on the state. */
			poll(NULL, 0, XAL_INOTIFY_POLL_TIMEOUT_MS);
			continue;
		}

		if (state == XAL_STATE_DIRTY) {
			/* Notify at most once per tree version: fire only when seq is stable
			 * and differs from the version the cb last fired at, so a mark that
			 * survived an index this loop never observed still gets its cb. */
			int seq = atomic_load(xal->seq_lock);

			if (!(seq & 1) && seq != cb_seq) {
				if (be->inotify->cb) {
					be->inotify->cb(xal, be->inotify->cb_args);
				}
				cb_seq = seq;
				continue;
			}

			/* Already notified for this version, and the queue cannot help: events
			 * pending there would make a poll on the fd return at once. */
			poll(NULL, 0, XAL_INOTIFY_POLL_TIMEOUT_MS);
			continue;
		}

		cb_seq = -1;

		/* The fd is non-blocking, so without this the read() below returns EAGAIN
		 * immediately and the loop becomes a busy poll. The timeout is what lets a
		 * dirty mark set elsewhere be noticed while no event ever arrives. */
		err = poll(&pfd, 1, XAL_INOTIFY_POLL_TIMEOUT_MS);
		if (err < 0) {
			if (errno == EINTR) {
				continue;
			}
			XAL_DEBUG("FAILED: poll(); errno(%d), exit thread", errno);
			err = -errno;
			xal_mark_dirty(xal);
			goto exit_thread;
		}
		if (!err) {
			continue;
		}

		err = check_events(be->inotify);
		if (err < 0) {
			XAL_DEBUG("FAILED: check_events(), exit thread; err(%d)", err);
			/* Nothing re-indexes once this loop is left, so leave the index dirty:
			 * readers get -ESTALE instead of extents for a vanished filesystem. */
			xal_mark_dirty(xal);
			goto exit_thread;
		}

		if (err) {
			XAL_DEBUG("INFO: Found breaking changes, marking xal as dirty");
			xal_mark_dirty(xal);
		}

	}

exit_thread:
	XAL_DEBUG("INFO: unlocked xal lock");

	if (be->inotify) {
		atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_RUNNING);
	}
	pthread_exit((void *)(intptr_t)err);
}

int
xal_watch_filesystem(struct xal *xal, xal_dirty_cb cb, void *cb_args)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)&xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->inotify || !be->inotify->watch_mode) {
		XAL_DEBUG("FAILED: xal opened without watch mode");
		return -EINVAL;
	}

	if (atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_RUNNING) {
		XAL_DEBUG("SKIPPED: thread already running");
		return 0;
	}

	if (atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_JOINABLE) {
		pthread_join(be->inotify->watch_thread_id, NULL);
		atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_JOINABLE);
	}

	if (xal->root_idx == XAL_POOL_IDX_NONE) {
		XAL_DEBUG("FAILED: Missing call to xal_index()");
		return -EINVAL;
	}

	be->inotify->cb = cb;
	be->inotify->cb_args = cb_args;
	atomic_store(&be->inotify->stop, false);

	err = pthread_create(&be->inotify->watch_thread_id, NULL, &background_thread_start, xal);
	if (err) {
		XAL_DEBUG("FAILED: pthread_create(); err(%d)", err);
		return -err;
	}

	/* Raised here rather than by the thread itself: until this store, watch_thread_id names no
	 * thread the reapers may touch, and a RUNNING raised by the thread would leave a window in
	 * which a started thread looks stopped. */
	atomic_fetch_or(&be->inotify->flag,
			XAL_BE_FIEMAP_INOTIFY_RUNNING | XAL_BE_FIEMAP_INOTIFY_JOINABLE);

	return 0;
}

int
xal_stop_watching_filesystem(struct xal *xal)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)&xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->inotify || !be->inotify->watch_mode) {
		XAL_DEBUG("FAILED: xal opened without watch mode");
		return -EINVAL;
	}

	if (!(atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_RUNNING)) {
		/* Not running, but a thread that exited on its own is still holding its stack. */
		if (atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_JOINABLE) {
			err = pthread_join(be->inotify->watch_thread_id, NULL);
			if (err) {
				XAL_DEBUG("FAILED: pthread_join(); err(%d)", err);
				return -err;
			}
			atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_JOINABLE);
		}

		XAL_DEBUG("FAILED: thread is not running");
		return -EINVAL;
	}

	atomic_store(&be->inotify->stop, true);
	err = pthread_join(be->inotify->watch_thread_id, NULL);
	if (err) {
		XAL_DEBUG("FAILED: pthread_join(); err(%d)", err);
		return -err;
	}

	atomic_fetch_and(&be->inotify->flag,
			 ~(XAL_BE_FIEMAP_INOTIFY_RUNNING | XAL_BE_FIEMAP_INOTIFY_JOINABLE));

	return 0;
}
