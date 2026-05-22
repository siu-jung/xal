#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xal_pool.h>

int
xal_pool_unmap(struct xal_pool *pool)
{
	return munmap(pool->memory, pool->reserved * pool->element_size);
}

static int
xal_pool_grow(struct xal_pool *pool, size_t growby)
{
	size_t growby_nbytes = growby * pool->element_size;
	uint8_t *tail = (uint8_t *)pool->memory + pool->allocated * pool->element_size;

	if (mprotect(tail, growby_nbytes, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}
	memset(tail, 0, growby_nbytes);

	pool->allocated += growby;

	return 0;
}

int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size,
             const char *shm_name)
{
	size_t nbytes = reserved * element_size;
	int err;

	if (pool->reserved) {
		XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
		return -EINVAL;
	}

	if (element_size < sizeof(uint32_t)) {
		XAL_DEBUG("FAILED: element_size(%zu) too small for freelist link", element_size);
		return -EINVAL;
	}

	pool->reserved = reserved;
	pool->element_size = element_size;
	pool->free = 0;
	pool->freelist_head = XAL_POOL_IDX_NONE;

	if (shm_name) {
		int fd;

		fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
		if (fd < 0) {
			XAL_DEBUG("FAILED: shm_open(%s); errno(%d)", shm_name, errno);
			return -errno;
		}

		err = ftruncate(fd, nbytes);
		if (err) {
			XAL_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
			close(fd);
			return -errno;
		}

		pool->memory = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (pool->memory == MAP_FAILED) {
			XAL_DEBUG("FAILED: mmap(); errno(%d)", errno);
			return -errno;
		}

		memset(pool->memory, 0, nbytes);
		pool->allocated = reserved;
		pool->growby = reserved;
	} else {
		if (allocated > reserved) {
			XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
			return -EINVAL;
		}

		pool->allocated = 0;
		pool->growby = allocated;

		pool->memory =
		    mmap(NULL, nbytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (MAP_FAILED == pool->memory) {
			XAL_DEBUG("FAILED: mmap(...); errno(%d)", errno);
			return -errno;
		}

		err = xal_pool_grow(pool, allocated);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(...); err(%d)", err);
			xal_pool_unmap(pool);
			return err;
		}
	}

	return 0;
}

static void *
slot_at(struct xal_pool *pool, uint32_t idx)
{
	return (uint8_t *)pool->memory + (size_t)idx * pool->element_size;
}

int
xal_pool_claim_one(struct xal_pool *pool, uint32_t *idx)
{
	int err;

	if (pool->freelist_head != XAL_POOL_IDX_NONE) {
		uint32_t slot = pool->freelist_head;
		uint32_t *link = slot_at(pool, slot);

		pool->freelist_head = *link;
		memset(link, 0, pool->element_size);

		if (idx) {
			*idx = slot;
		}
		return 0;
	}

	if (pool->free >= pool->allocated) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (pool->free >= UINT32_MAX) {
		XAL_DEBUG("FAILED: pool->free exceeds uint32_t range");
		return -EOVERFLOW;
	}

	if (idx) {
		*idx = (uint32_t)pool->free;
	}
	pool->free += 1;

	return 0;
}

int
xal_pool_claim_contig(struct xal_pool *pool, size_t count, uint32_t *idx)
{
	int err;

	if (count == 0) {
		return -EINVAL;
	}
	if (count > pool->growby) {
		XAL_DEBUG("FAILED: count > pool->growby");
		return -EINVAL;
	}

	if (pool->allocated < (pool->free + count)) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (pool->free + count > UINT32_MAX) {
		XAL_DEBUG("FAILED: pool->free exceeds uint32_t range");
		return -EOVERFLOW;
	}

	if (idx) {
		*idx = (uint32_t)pool->free;
	}
	pool->free += count;

	return 0;
}

int
xal_pool_release_one(struct xal_pool *pool, uint32_t idx)
{
	uint32_t *link;

	if (idx >= pool->free) {
		XAL_DEBUG("FAILED: release of slot(%u) >= free(%zu)", idx, pool->free);
		return -EINVAL;
	}

	link = slot_at(pool, idx);
	*link = pool->freelist_head;
	pool->freelist_head = idx;

	return 0;
}

int
xal_pool_clear(struct xal_pool *pool)
{
	if (mprotect(pool->memory, pool->reserved * pool->element_size, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}
	memset(pool->memory, 0, pool->reserved * pool->element_size);

	pool->free = 0;
	pool->allocated = 0;
	pool->freelist_head = XAL_POOL_IDX_NONE;

	return 0;
}
