#include <stdint.h>

/**
 * A pool of mmap backed memory for fixed-size elements.
 *
 * This is utilized for inodes, extent-blocks and dentry-blocks. The useful feature is that we can
 * have a contigous virtual address space which can grow without having to move elements nor change
 * pointers to them, as one would otherwise have to do with malloc()/realloc().
 *
 * The pool supports both bump allocation (xal_pool_claim_contig for contiguous claim) and per-element
 * claim/release with an intrusive freelist (xal_pool_claim_one / xal_pool_release_one). When an
 * element sits on the freelist, the first uint32_t of its memory holds the index of the next free
 * element. The pool treats element memory as opaque on release; the caller is expected to write
 * any data layout it needs on claim.
 */
struct xal_pool {
	size_t reserved;     ///< Maximum number of elements in the pool
	size_t allocated;    ///< Number of reserved elements that are allocated
	size_t growby;	     ///< Number of reserved elements to allocate at a time
	size_t free;	     ///< Bump cursor; index of the next never-yet-claimed element
	size_t element_size; ///< Size of a single element in bytes
	void *memory;	     ///< Memory space for elements
	uint32_t freelist_head; ///< XAL_POOL_IDX_NONE means freelist is empty
};

int
xal_pool_unmap(struct xal_pool *pool);

/**
 * Initialize the given pool.
 *
 * If shm_name is NULL, uses private anonymous memory with lazy mprotect growth.
 * If shm_name is non-NULL, backs the pool with a POSIX shared memory object of that name.
 * In the shm case the full reserved size is committed upfront and the caller is responsible
 * for shm_unlink() when the shm is no longer needed.
 */
int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size,
             const char *shm_name);

/**
 * Claim a single element. Pulls from the freelist if possible, otherwise bumps the cursor and
 * grows the allocated region as needed. The claimed element is zero-initialized.
 */
int
xal_pool_claim_one(struct xal_pool *pool, uint32_t *idx);

/**
 * Claim a contiguous run of 'count' elements. Always bumps the cursor; never consults the
 * freelist (the freelist tracks single elements). Used for the inode pool where the root inode
 * is allocated and to keep an option open for contiguous claims.
 */
int
xal_pool_claim_contig(struct xal_pool *pool, size_t count, uint32_t *idx);

/**
 * Release a previously-claimed element back to the freelist.
 */
int
xal_pool_release_one(struct xal_pool *pool, uint32_t idx);

int
xal_pool_clear(struct xal_pool *pool);
