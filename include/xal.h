#include <stdatomic.h>
#include <unistd.h>
#include <xal_pool.h>

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode
#define XAL_BACKEND_SIZE 64

struct xal_backend_base {
	enum xal_backend type;
	int (*index)(struct xal *xal);
	void (*close)(struct xal *xal);
};

/**
 * XAL
 * 
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 *
 * @struct xal
 */
/**
 * Append a child inode index to the dentry chain rooted at `dentries`. Allocates a new
 * dentry-block from xal->dentry_blocks if the current tail is full or the chain is empty.
 *
 * @return 0 on success, negative errno on failure.
 */
int
xal_dentries_append(struct xal *xal, struct xal_dentries *dentries, uint32_t child_idx);

/**
 * Release the entire dentry-block chain back to xal->dentry_blocks freelist. Resets
 * `dentries` to an empty state. The child inodes themselves are NOT released; the caller
 * decides whether to free them via xal_pool_release_one().
 */
void
xal_dentries_release(struct xal *xal, struct xal_dentries *dentries);

/**
 * Append an extent to the extent chain rooted at `extents`. Allocates a new extent-block
 * from xal->extent_blocks if the current tail is full or the chain is empty.
 */
int
xal_extents_append(struct xal *xal, struct xal_extents *extents, const struct xal_extent *e);

/**
 * Release the entire extent-block chain back to xal->extent_blocks freelist. Resets
 * `extents` to an empty state.
 */
void
xal_extents_release(struct xal *xal, struct xal_extents *extents);

struct xal {
	struct xnvme_dev *dev;
	struct xal_pool inodes;         ///< Pool of inodes in host-native format
	struct xal_pool dentry_blocks;  ///< Pool of struct xal_dentry_block, chained per-directory
	struct xal_pool extent_blocks;  ///< Pool of struct xal_extent_block, chained per-file
	uint32_t root_idx;       ///< Index of the root inode in the inodes pool
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
	atomic_bool *dirty;      ///< Whether the file system has changed since last index; may point to external shared memory
	atomic_bool _dirty_storage; ///< Backing store for dirty when no external pointer is provided
	atomic_int seq_lock;     ///< An uneven number indicates the struct is being modified and is not safe to read
	bool shared_view;        ///< If true, pool memory is owned externally; xal_close() will not unmap it
};
