#ifndef ODB_WARP_H
#define ODB_WARP_H

#include "odb.h"

struct commit;
struct object;
struct repository;
struct multi_pack_index;

void odb_warp_free_type_bitmaps(struct multi_pack_index *m);

/*
 * Centralized fast-metadata ("Warp") read layer for Git on S3/Rclone VFS.
 *
 * Reuses small metadata files generated during repack & incremental fetch
 * (commit-graph / commit-graph-chain, packed-refs peeled tags, multi-pack-index,
 * .bitmap type bitmaps, and .idx) at the core read entrypoints before falling
 * back to opening or reading large .pack files.
 *
 * All functions gracefully return -1, 0, or NULL when auxiliary files are absent
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

#ifdef ODB_WARP_IMPLEMENTATION

#include "commit.h"
#include "commit-graph.h"
#include "ewah/ewok.h"
#include "hex.h"
#include "midx.h"
#include "odb/source-files.h"
#include "odb/source-packed.h"
#include "oidmap.h"
#include "pack-bitmap.h"
#include "pack-revindex.h"
#include "packfile.h"
#include "path.h"
#include "repository.h"
#include "strbuf.h"
#include "tag.h"

struct odb_warp_peeled_tag_entry {
	struct oidmap_entry entry;
	struct object_id peeled_oid;
};

static void odb_warp_clear_peeled_tags(struct object_database *odb)
{
	if (odb->warp_peeled_tags) {
		oidmap_clear(odb->warp_peeled_tags, 1);
		FREE_AND_NULL(odb->warp_peeled_tags);
	}
}

static void odb_warp_ensure_peeled_tags(struct repository *r)
{
	char *path;
	FILE *fp;
	struct strbuf line = STRBUF_INIT;
	struct object_id last_oid;
	int has_last = 0;

	if (!r || !r->gitdir || r->objects->warp_peeled_tags)
		return;

	CALLOC_ARRAY(r->objects->warp_peeled_tags, 1);
	oidmap_init(r->objects->warp_peeled_tags, 0);
	path = repo_git_path(r, "packed-refs");
	fp = fopen(path, "r");
	free(path);
	if (!fp)
		return;

	while (!strbuf_getline(&line, fp)) {
		const char *end;

		if (line.buf[0] == '^') {
			struct object_id peeled;
			if (has_last &&
			    !parse_oid_hex_algop(line.buf + 1, &peeled, &end, r->hash_algo) &&
			    !*end) {
				struct odb_warp_peeled_tag_entry *e;
				CALLOC_ARRAY(e, 1);
				oidcpy(&e->entry.oid, &last_oid);
				oidcpy(&e->peeled_oid, &peeled);
				free(oidmap_put(r->objects->warp_peeled_tags, e));
			}
			has_last = 0;
		} else {
			has_last = !parse_oid_hex_algop(line.buf, &last_oid, &end,
						       r->hash_algo) &&
				   *end == ' ';
		}
	}
	strbuf_release(&line);
	fclose(fp);
}

static const struct odb_warp_peeled_tag_entry *odb_warp_lookup_peeled_tag_entry(
	struct repository *r, const struct object_id *oid)
{
	if (!r || !oid)
		return NULL;
	odb_warp_ensure_peeled_tags(r);
	if (!r->objects->warp_peeled_tags)
		return NULL;
	return oidmap_get(r->objects->warp_peeled_tags, oid);
}

const struct object_id *odb_warp_lookup_peeled_tag(struct repository *r,
						   const struct object_id *oid)
{
	const struct odb_warp_peeled_tag_entry *e = odb_warp_lookup_peeled_tag_entry(r, oid);
	return e ? &e->peeled_oid : NULL;
}

/*
 * Lightweight MIDX type-bitmap cache: mmaps multi-pack-index-<hash>.bitmap
 * once to extract only the 4 EWAH type bitmaps (commits, trees, blobs, tags)
 * without opening any .pack or individual pack .idx/.rev files.
 */
struct odb_warp_type_bitmaps {
	struct bitmap *commits;
	struct bitmap *trees;
	struct bitmap *blobs;
	struct bitmap *tags;
};

static struct bitmap *odb_warp_read_type_bitmap(const unsigned char *map,
						size_t map_size,
						size_t *map_pos)
{
	struct ewah_bitmap *ewah;
	struct bitmap *bm;
	ssize_t sz;

	if (*map_pos >= map_size)
		return NULL;
	ewah = ewah_pool_new();
	sz = ewah_read_mmap(ewah, map + *map_pos, map_size - *map_pos);
	if (sz < 0) {
		ewah_pool_free(ewah);
		return NULL;
	}
	*map_pos += (size_t)sz;
	bm = ewah_to_bitmap(ewah);
	ewah_pool_free(ewah);
	return bm;
}

void odb_warp_free_type_bitmaps(struct multi_pack_index *m)
{
	struct odb_warp_type_bitmaps *bm = m->warp_type_bitmaps;

	if (!bm)
		return;
	if (bm->commits)
		bitmap_free(bm->commits);
	if (bm->trees)
		bitmap_free(bm->trees);
	if (bm->blobs)
		bitmap_free(bm->blobs);
	if (bm->tags)
		bitmap_free(bm->tags);
	FREE_AND_NULL(m->warp_type_bitmaps);
}

static int odb_warp_ensure_type_bitmaps(struct multi_pack_index *m)
{
	struct odb_warp_type_bitmaps *bm;
	struct bitmap_disk_header *header;
	struct strbuf bm_name = STRBUF_INIT;
	const struct git_hash_algo *algo;
	size_t header_size, map_size, map_pos;
	unsigned char *map;
	struct stat st;
	int fd;

	if (!m || m->base_midx || m->has_chain)
		return -1;
	if (m->warp_type_bitmaps)
		return m->warp_type_bitmaps->blobs ? 0 : -1;

	CALLOC_ARRAY(m->warp_type_bitmaps, 1);
	bm = m->warp_type_bitmaps;

	if (m->chunk_revindex &&
	    m->chunk_revindex_len != st_mult(sizeof(uint32_t), m->num_objects))
		return -1;

	algo = m->source->base.odb->repo->hash_algo;
	header_size = sizeof(struct bitmap_disk_header) - GIT_MAX_RAWSZ + algo->rawsz;

	get_midx_filename_ext(m->source, &bm_name,
			      midx_get_checksum_hash(m), MIDX_EXT_BITMAP);
	fd = git_open(bm_name.buf);
	strbuf_release(&bm_name);
	if (fd < 0)
		return -1;

	if (fstat(fd, &st) || st.st_size < (off_t)(header_size + algo->rawsz)) {
		close(fd);
		return -1;
	}

	if (load_midx_revindex(m)) {
		close(fd);
		return -1;
	}

	map_size = xsize_t(st.st_size);
	map = xmmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	header = (void *)map;
	if (memcmp(header->magic, BITMAP_IDX_SIGNATURE, sizeof(BITMAP_IDX_SIGNATURE)) ||
	    ntohs(header->version) != 1 ||
	    !(ntohs(header->options) & BITMAP_OPT_FULL_DAG) ||
	    !hasheq(header->checksum, midx_get_checksum_hash(m), algo)) {
		munmap(map, map_size);
		return -1;
	}

	map_pos = header_size;
	bm->commits = odb_warp_read_type_bitmap(map, map_size - algo->rawsz, &map_pos);
	bm->trees = odb_warp_read_type_bitmap(map, map_size - algo->rawsz, &map_pos);
	bm->blobs = odb_warp_read_type_bitmap(map, map_size - algo->rawsz, &map_pos);
	bm->tags = odb_warp_read_type_bitmap(map, map_size - algo->rawsz, &map_pos);
	munmap(map, map_size);

	if (!bm->commits || !bm->trees ||
	    !bm->blobs || !bm->tags) {
		odb_warp_free_type_bitmaps(m);
		CALLOC_ARRAY(m->warp_type_bitmaps, 1);
		return -1;
	}
	return 0;
}

static enum object_type odb_warp_lookup_midx_bitmap_type(struct multi_pack_index *m,
							 uint32_t midx_pos)
{
	struct odb_warp_type_bitmaps *bm;
	uint32_t pack_pos;

	if (odb_warp_ensure_type_bitmaps(m) < 0)
		return OBJ_NONE;
	bm = m->warp_type_bitmaps;
	if (midx_to_pack_pos(m, midx_pos, &pack_pos) < 0)
		return OBJ_NONE;

	if (bitmap_get(bm->blobs, pack_pos))
		return OBJ_BLOB;
	if (bitmap_get(bm->trees, pack_pos))
		return OBJ_TREE;
	if (bitmap_get(bm->commits, pack_pos))
		return OBJ_COMMIT;
	if (bitmap_get(bm->tags, pack_pos))
		return OBJ_TAG;
	return OBJ_NONE;
}

static enum object_type odb_warp_find_midx_type(struct repository *r,
						const struct object_id *oid)
{
	struct odb_source *source;

	if (!r || !r->objects || !oid)
		return OBJ_NONE;

	for (source = r->objects->sources; source; source = source->next) {
		struct odb_source_files *files;
		struct odb_source_packed *packed;
		uint32_t midx_pos;

		if (source->type != ODB_SOURCE_FILES)
			continue;
		files = odb_source_files_downcast(source);
		packed = files->packed;
		odb_source_prepare(&packed->base, 0);

		if (packed->midx && bsearch_midx(oid, packed->midx, &midx_pos)) {
			enum object_type t = odb_warp_lookup_midx_bitmap_type(
				packed->midx, midx_pos);
			if (t != OBJ_NONE)
				return t;
		}
	}
	return OBJ_NONE;
}

static int odb_warp_quiet_walk;

void odb_warp_set_quiet_walk(int v)
{
	odb_warp_quiet_walk = v;
}

int odb_warp_has_midx(struct repository *r)
{
	struct odb_source *source;

	if (!r || !r->objects)
		return 0;
	for (source = r->objects->sources; source; source = source->next) {
		struct odb_source_files *files;
		if (source->type != ODB_SOURCE_FILES)
			continue;
		files = odb_source_files_downcast(source);
		odb_source_prepare(&files->packed->base, 0);
		if (files->packed->midx)
			return 1;
	}
	return 0;
}

int odb_warp_is_midx_bitmapped_tree(struct repository *r,
				    const struct object_id *oid)
{
	if (!odb_warp_quiet_walk)
		return 0;
	return odb_warp_find_midx_type(r, oid) == OBJ_TREE;
}

int odb_warp_read_object_info(struct object_database *odb,
			      const struct object_id *real,
			      struct object_info *oi,
			      unsigned flags)
{
	struct odb_source *source;
	int type_only = 0;

	if (!odb || !odb->repo || (flags & OBJECT_INFO_SECOND_READ))
		return -1;

	if (oi) {
		if (!oi->typep || oi->sizep || oi->disk_sizep ||
		    oi->delta_base_oid || oi->contentp || oi->mtimep ||
		    oi->source_infop)
			return -1;
		type_only = 1;
	}

	/*
	 * 1. commit-graph (covers base commit-graph + 20-fetch split chain).
	 * Note: when !oi (odb_has_object), only check commit-graph if
	 * GIT_COMMIT_GRAPH_PARANOIA is not enabled, preventing recursion.
	 */
	if (type_only) {
		if (lookup_commit_in_graph(odb->repo, real)) {
			*oi->typep = OBJ_COMMIT;
			return 0;
		}
	}

	/*
	 * 2. MIDX + .bitmap + non-MIDX incremental .idx files.
	 * Answers existence (!oi) without opening any .pack file, and answers
	 * type_only queries for Trees/Blobs/Tags via the MIDX .bitmap type bitmaps.
	 */
	for (source = odb->sources; source; source = source->next) {
		struct odb_source_files *files;
		struct odb_source_packed *packed;
		struct packfile_list_entry *l;

		if (source->type != ODB_SOURCE_FILES)
			continue;
		files = odb_source_files_downcast(source);
		packed = files->packed;
		odb_source_prepare(&packed->base, 0);

		if (packed->midx) {
			uint32_t midx_pos;
			if (bsearch_midx(real, packed->midx, &midx_pos)) {
				if (!oi)
					return 0;
				if (type_only) {
					enum object_type t = odb_warp_lookup_midx_bitmap_type(
						packed->midx, midx_pos);
					if (t != OBJ_NONE) {
						*oi->typep = t;
						return 0;
					}
				}
			}
		}

		if (!oi) {
			for (l = packed->packs.head; l; l = l->next) {
				if (!l->pack->multi_pack_index &&
				    find_pack_entry_one(real, l->pack))
					return 0;
			}
		}
	}

	/*
	 * 3. Peeled tags in packed-refs (fallback for tags not covered by bitmap).
	 */
	if (type_only && odb_warp_lookup_peeled_tag(odb->repo, real)) {
		*oi->typep = OBJ_TAG;
		return 0;
	}

	return -1;
}

void odb_warp_peel_to_tree_oid(struct repository *r,
			       struct object_id *actual_oid,
			       enum object_type required_type)
{
	const struct object_id *peeled;
	struct commit *c;
	struct object *obj;
	enum object_type bm_type;

	if (!r || required_type != OBJ_TREE)
		return;

	obj = lookup_object(r, actual_oid);
	if (obj) {
		if (obj->type == OBJ_TREE || obj->type == OBJ_BLOB)
			return;
		if (obj->type == OBJ_TAG && ((struct tag *)obj)->tagged)
			oidcpy(actual_oid, &((struct tag *)obj)->tagged->oid);
	}

	c = lookup_commit_in_graph(r, actual_oid);
	if (c) {
		oidcpy(actual_oid, get_commit_tree_oid(c));
		return;
	}

	bm_type = odb_warp_find_midx_type(r, actual_oid);
	if (bm_type != OBJ_NONE && bm_type != OBJ_TAG)
		return;

	peeled = odb_warp_lookup_peeled_tag(r, actual_oid);
	if (peeled) {
		oidcpy(actual_oid, peeled);
		c = lookup_commit_in_graph(r, actual_oid);
		if (c)
			oidcpy(actual_oid, get_commit_tree_oid(c));
	}
}

struct object *odb_warp_parse_object(struct repository *r,
				     const struct object_id *oid UNUSED,
				     const struct object_id *repl,
				     unsigned flags)
{
	struct commit *commit;

	if (!r || !(flags & PARSE_OBJECT_SKIP_HASH_CHECK))
		return NULL;

	commit = lookup_commit_in_graph(r, repl);
	return commit ? &commit->object : NULL;
}

struct commit *odb_warp_deref_to_commit(struct repository *r,
					const struct object_id *oid,
					unsigned int mark_tag_flags)
{
	struct commit *commit;
	const struct odb_warp_peeled_tag_entry *peeled_entry;
	struct object *obj;
	enum object_type bm_type;

	if (!r || !oid)
		return NULL;

	commit = lookup_commit_in_graph(r, oid);
	if (commit)
		return commit;

	/*
	 * A fully peeled OID is sufficient for negotiation tips, but not for
	 * marking a chain of tag objects COMPLETE. Let the caller walk the
	 * actual chain in that case.
	 */
	if (mark_tag_flags)
		return NULL;

	obj = lookup_object(r, oid);
	if (obj) {
		if (obj->type == OBJ_TREE || obj->type == OBJ_BLOB)
			return NULL;
		if (obj->type == OBJ_TAG && ((struct tag *)obj)->tagged)
			return lookup_commit_in_graph(r, &((struct tag *)obj)->tagged->oid);
	}

	bm_type = odb_warp_find_midx_type(r, oid);
	if (bm_type != OBJ_NONE && bm_type != OBJ_TAG)
		return NULL;

	peeled_entry = odb_warp_lookup_peeled_tag_entry(r, oid);
	if (peeled_entry)
		return lookup_commit_in_graph(r, &peeled_entry->peeled_oid);

	return NULL;
}

#endif /* ODB_WARP_IMPLEMENTATION */
#endif /* ODB_WARP_H */
