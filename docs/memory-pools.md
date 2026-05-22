# Pool Memory

xal uses three pools (``struct xal_pool``), each backed by a large
over-committed ``mmap`` region:

- ``inodes`` — fixed-size ``struct xal_inode`` slots, addressed by index.
- ``dentry_blocks`` — fixed-size ``struct xal_dentry_block`` slots, each
  holding up to ``XAL_DENTRY_BLOCK_CAP`` child indices and a ``next_idx``
  forming a chain rooted at a directory inode.
- ``extent_blocks`` — fixed-size ``struct xal_extent_block`` slots, each
  holding up to ``XAL_EXTENT_BLOCK_CAP`` extents inline plus a ``next_idx``
  forming a chain rooted at a regular-file inode.

Each pool reserves a virtual address range upfront sized for the maximum
expected number of elements, but only commits physical pages in chunks as
elements are claimed (via ``mprotect``). Elements never move, so pool indices
remain stable across insertions.

## Variable-length data via chained blocks

A directory can have any number of children and a regular file any number of
extents, but the inode struct is fixed size. The variable-length data is
stored externally in a chain of fixed-size blocks: the inode holds
``first_block_idx`` and a total ``count``; each block holds up to its
capacity and points to the next block. Iterators
(``xal_dentry_iter_init``/``xal_extent_iter_init``) hide the chain.

## Per-element claim and release

Each pool supports both a bump-allocator path
(``xal_pool_claim_contig`` for contiguous claim of N elements) and a per-element
path with an intrusive freelist (``xal_pool_claim_one`` /
``xal_pool_release_one``). The freelist link occupies the first
``uint32_t`` of a free element; on claim the element is zero-initialized.

Releasing a chain (``xal_dentries_release`` / ``xal_extents_release``)
returns each block in the chain to the pool's freelist so future allocations
can reuse them. This is what enables the FIEMAP backend's
``XAL_WATCHMODE_EXTENT_UPDATE`` path to update extents on file modification
without leaking the previous extents until the next full reindex.

## Lazy growth (anonymous mode)

By default, pools use private anonymous memory. The full virtual address range
is reserved with ``PROT_NONE`` at open time; physical pages are committed in
chunks of ``growby`` elements by calling ``mprotect(PROT_READ|PROT_WRITE)``
whenever the pool runs low.

## Shared memory mode

When ``xal_opts.shm_name`` is set, the pools are backed by POSIX shared
memory objects. Because all internal cross-references use integer indices
rather than raw pointers, the data is valid regardless of the virtual
address at which it is mapped in each process. The object names are derived
from the base name with these suffixes:

- ``_inodes`` — the inodes pool
- ``_dblks`` — the dentry-blocks pool
- ``_eblks`` — the extent-blocks pool
- ``_dirty`` — the atomic dirty flag

Example::

   opts.shm_name = "/myapp_xal";
   /* creates /myapp_xal_inodes, /myapp_xal_dblks,
      /myapp_xal_eblks, /myapp_xal_dirty */

In this mode the full reserved size is committed upfront via ``ftruncate()``
and ``mmap(MAP_SHARED)``; there is no lazy growth. The objects persist in the
shared memory filesystem (``/dev/shm`` on Linux) until explicitly removed.
The process that opened xal with ``shm_name`` set is responsible for calling
``shm_unlink()`` on each object when no longer needed.
``xal_close()`` will ``munmap`` the regions but will not unlink them.

## Consumer processes: ``xal_from_pools()``

A secondary process that needs read-only access to an already-indexed pool
attaches to all three shared memory objects and the dirty flag, then calls
``xal_from_pools()``::

   const struct xal_sb *sb = /* superblock communicated OOB */;
   void *inodes_mem = /* mmap of /myapp_xal_inodes */;
   void *dblks_mem  = /* mmap of /myapp_xal_dblks */;
   void *eblks_mem  = /* mmap of /myapp_xal_eblks */;
   _Atomic bool *dirty = /* mmap of /myapp_xal_dirty */;
   struct xal *view;

   xal_from_pools(sb, NULL, inodes_mem, dblks_mem, eblks_mem, dirty, &view);
   xal_walk(view, xal_get_root(view), my_callback, NULL);
   xal_close(view); /* frees the struct; does NOT munmap or unlink */

The resulting ``struct xal`` has ``shared_view`` set to ``true``.
``xal_close()`` on a shared view frees only the ``struct xal`` allocation;
the pool memory regions are left mapped and must be unmapped by the caller.
The process that created the shared memory objects is responsible for
``shm_unlink()``.
