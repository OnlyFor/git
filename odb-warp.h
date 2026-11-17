#ifndef ODB_WARP_H
#define ODB_WARP_H

#include "odb.h"

struct commit;
struct object;
struct repository;
struct multi_pack_index;
struct packed_git;

void odb_warp_free_type_bitmaps(struct multi_pack_index *m);

/*
 * Centralized fast-metadata ("Warp") read layer for Git on S3/Rclone VFS.
 *
 * Reuses small metadata files generated during repack & incremental fetch
 * (commit-graph / commit-graph-chain, packed-refs peeled tags, multi-pack-index,
 * .bitmap type bitmaps, and .idx) at the core read entrypoints before falling
 * back to opening or reading large .pack files.
 *
 * Metadata queries gracefully return -1, 0, or NULL when auxiliary files are absent
 * or do not cover the target object. Loading metadata can itself require IO.
 */

int odb_warp_read_object_info(struct object_database *odb,
			      const struct object_id *real,
			      struct object_info *oi,
			      unsigned flags);

void odb_warp_peel_to_tree_oid(struct repository *r,
			       struct object_id *actual_oid,
			       enum object_type required_type);

struct object *odb_warp_parse_object(struct repository *r,
				     const struct object_id *oid,
				     const struct object_id *repl,
				     unsigned flags);

struct commit *odb_warp_deref_to_commit(struct repository *r,
					const struct object_id *oid,
					unsigned int mark_tag_flags);

const struct object_id *odb_warp_lookup_peeled_tag(struct repository *r,
						   const struct object_id *oid);

void odb_warp_set_quiet_walk(int v);

int odb_warp_has_midx(struct repository *r);

int odb_warp_is_midx_bitmapped_tree(struct repository *r,
				    const struct object_id *oid);

/*
 * Fetch and integrity policy. Callers retain ownership of protocol encoding,
 * object validation and command-specific output.
 */
int odb_warp_fetch_use_thin_pack(struct repository *r, int requested);
int odb_warp_check_fetch_graph_objects(void);
int odb_warp_check_index_pack_collisions(int strict);
int odb_warp_check_connected_objects(void);
int odb_warp_can_defer_pack_index(struct packed_git *p);

/* Release repository-owned metadata when the ODB is closed or refreshed. */
void odb_warp_clear_peeled_tags(struct object_database *odb);

#endif /* ODB_WARP_H */
