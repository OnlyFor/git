#ifndef ODB_WARP_H
#define ODB_WARP_H

#include "odb.h"

struct commit;
struct object;
struct repository;

/*
 * Centralized fast-metadata ("Warp") read layer for Git on S3/Rclone VFS.
 *
 * Reuses small metadata files generated during repack & incremental fetch
 * (commit-graph / commit-graph-chain, packed-refs peeled tags, multi-pack-index,
 * .bitmap type bitmaps, and .idx) at the core read entrypoints before falling
 * back to opening or reading large .pack files.
 *
 * All functions gracefully return -1 or NULL when auxiliary files are absent
 * or do not cover the target object, introducing zero extra IO.
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

int odb_warp_synthesize_header(struct repository *r,
			       const struct object_id *oid,
			       const struct object_id *peeled_hint,
			       enum object_type *type_out,
			       size_t *size_out,
			       void **content_out);

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

static struct oidmap odb_warp_peeled_tags = OIDMAP_INIT;
static struct repository *odb_warp_peeled_tags_repo;
static int odb_warp_peeled_tags_loaded;

static void odb_warp_ensure_peeled_tags(struct repository *r)
{
	char *path;
	FILE *fp;
	char line[1024];
	struct object_id last_oid;
	int has_last = 0;

	if (!r || !r->gitdir)
		return;
	if (odb_warp_peeled_tags_loaded && odb_warp_peeled_tags_repo == r)
		return;

	if (odb_warp_peeled_tags_loaded)
		oidmap_clear(&odb_warp_peeled_tags, 1);
	oidmap_init(&odb_warp_peeled_tags, 0);
	odb_warp_peeled_tags_repo = r;
	odb_warp_peeled_tags_loaded = 1;

	path = repo_git_path(r, "packed-refs");
	fp = fopen(path, "r");
	free(path);
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#') {
			has_last = 0;
			continue;
		}
		if (line[0] == '^') {
			struct object_id peeled;
			const char * end;
			if (has_last &&
			    !parse_oid_hex_algop(line + 1, &peeled, &end, r->hash_algo) &&
			    (*end == '\n' || *end == '\r' || *end == '\0')) {
				struct odb_warp_peeled_tag_entry *e;
				e = xmalloc(sizeof(*e));
				oidcpy(&e->entry.oid, &last_oid);
				oidcpy(&e->peeled_oid, &peeled);
				free(oidmap_put(&odb_warp_peeled_tags, e));
			}
			has_last = 0;
			continue;
		}
		{
			const char *end;
			if (!parse_oid_hex_algop(line, &last_oid, &end, r->hash_algo) &&
			    *end == ' ')
				has_last = 1;
			else
				has_last = 0;
		}
	}
	fclose(fp);
}

const struct object_id *odb_warp_lookup_peeled_tag(struct repository *r,
						   const struct object_id *oid)
{
	struct odb_warp_peeled_tag_entry *e;

	if (!r || !oid)
		return NULL;
	odb_warp_ensure_peeled_tags(r);
	e = oidmap_get(&odb_warp_peeled_tags, oid);
	return e ? &e->peeled_oid : NULL;
}

/*
 * Lightweight MIDX type-bitmap cache: mmaps multi-pack-index-<hash>.bitmap
 * once to extract only the 4 EWAH type bitmaps (commits, trees, blobs, tags)
 * without opening any .pack or individual pack .idx/.rev files.
 */
static struct multi_pack_index *odb_warp_bm_midx;
static int odb_warp_bm_attempted;
static struct bitmap *odb_warp_bm_commits;
static struct bitmap *odb_warp_bm_trees;
static struct bitmap *odb_warp_bm_blobs;
static struct bitmap *odb_warp_bm_tags;

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

static void odb_warp_free_type_bitmaps(void)
{
	if (odb_warp_bm_commits) {
		bitmap_free(odb_warp_bm_commits);
		odb_warp_bm_commits = NULL;
	}
	if (odb_warp_bm_trees) {
		bitmap_free(odb_warp_bm_trees);
		odb_warp_bm_trees = NULL;
	}
	if (odb_warp_bm_blobs) {
		bitmap_free(odb_warp_bm_blobs);
		odb_warp_bm_blobs = NULL;
	}
	if (odb_warp_bm_tags) {
		bitmap_free(odb_warp_bm_tags);
		odb_warp_bm_tags = NULL;
	}
}

static int odb_warp_ensure_type_bitmaps(struct multi_pack_index *m)
{
	struct strbuf bm_name = STRBUF_INIT;
	const struct git_hash_algo *algo;
	size_t header_size, map_size, map_pos;
	unsigned char *map;
	struct stat st;
	int fd;

	if (!m || m->base_midx || m->has_chain)
		return -1;
	if (odb_warp_bm_attempted && odb_warp_bm_midx == m)
		return odb_warp_bm_blobs ? 0 : -1;

	odb_warp_free_type_bitmaps();
	odb_warp_bm_midx = m;
	odb_warp_bm_attempted = 1;

	if (load_midx_revindex(m))
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

	map_size = xsize_t(st.st_size);
	map = xmmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	if (memcmp(map, BITMAP_IDX_SIGNATURE, sizeof(BITMAP_IDX_SIGNATURE)) != 0) {
		munmap(map, map_size);
		return -1;
	}

	map_pos = header_size;
	odb_warp_bm_commits = odb_warp_read_type_bitmap(map, map_size, &map_pos);
	odb_warp_bm_trees = odb_warp_read_type_bitmap(map, map_size, &map_pos);
	odb_warp_bm_blobs = odb_warp_read_type_bitmap(map, map_size, &map_pos);
	odb_warp_bm_tags = odb_warp_read_type_bitmap(map, map_size, &map_pos);
	munmap(map, map_size);

	if (!odb_warp_bm_commits || !odb_warp_bm_trees ||
	    !odb_warp_bm_blobs || !odb_warp_bm_tags) {
		odb_warp_free_type_bitmaps();
		return -1;
	}
	return 0;
}

static enum object_type odb_warp_lookup_midx_bitmap_type(struct multi_pack_index *m,
							 uint32_t midx_pos)
{
	uint32_t pack_pos;

	if (odb_warp_ensure_type_bitmaps(m) < 0)
		return OBJ_NONE;
	if (midx_to_pack_pos(m, midx_pos, &pack_pos) < 0)
		return OBJ_NONE;

	if (bitmap_get(odb_warp_bm_blobs, pack_pos))
		return OBJ_BLOB;
	if (bitmap_get(odb_warp_bm_trees, pack_pos))
		return OBJ_TREE;
	if (bitmap_get(odb_warp_bm_commits, pack_pos))
		return OBJ_COMMIT;
	if (bitmap_get(odb_warp_bm_tags, pack_pos))
		return OBJ_TAG;
	return OBJ_NONE;
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
		if (odb_warp_lookup_peeled_tag(odb->repo, real)) {
			*oi->typep = OBJ_TAG;
			return 0;
		}
	}

	/*
	 * 2. MIDX + .bitmap + non-MIDX incremental .idx files.
	 * Answers existence (!oi) without opening any .pack file, and answers
	 * type_only queries for Trees/Blobs via the MIDX .bitmap type bitmaps.
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

	return -1;
}

void odb_warp_peel_to_tree_oid(struct repository *r,
			       struct object_id *actual_oid,
			       enum object_type required_type)
{
	const struct object_id *peeled;
	struct commit *c;

	if (!r || required_type != OBJ_TREE)
		return;

	peeled = odb_warp_lookup_peeled_tag(r, actual_oid);
	if (peeled)
		oidcpy(actual_oid, peeled);

	c = lookup_commit_in_graph(r, actual_oid);
	if (c)
		oidcpy(actual_oid, get_commit_tree_oid(c));
}

struct object *odb_warp_parse_object(struct repository *r,
				     const struct object_id *oid,
				     const struct object_id *repl,
				     unsigned flags)
{
	int skip_hash = !!(flags & PARSE_OBJECT_SKIP_HASH_CHECK);
	struct commit *commit;
	const struct object_id *peeled;

	if (!r || (!skip_hash && save_commit_buffer))
		return NULL;

	commit = lookup_commit_in_graph(r, repl);
	if (commit)
		return &commit->object;

	peeled = odb_warp_lookup_peeled_tag(r, repl);
	if (peeled) {
		commit = lookup_commit_in_graph(r, peeled);
		if (commit) {
			struct tag *tag = lookup_tag(r, oid);
			if (tag) {
				tag->object.parsed = 1;
				tag->tagged = &commit->object;
				tag->date = commit->date;
				return &tag->object;
			}
		}
	}

	return NULL;
}

struct commit *odb_warp_deref_to_commit(struct repository *r,
					const struct object_id *oid,
					unsigned int mark_tag_flags)
{
	struct commit *commit;
	const struct object_id *peeled;
	struct object *obj;

	if (!r || !oid)
		return NULL;

	commit = lookup_commit_in_graph(r, oid);
	if (commit)
		return commit;

	obj = lookup_object(r, oid);
	if (obj && obj->type == OBJ_TAG && ((struct tag *)obj)->tagged) {
		if (mark_tag_flags)
			obj->flags |= mark_tag_flags;
		return lookup_commit_in_graph(r, &((struct tag *)obj)->tagged->oid);
	}

	peeled = odb_warp_lookup_peeled_tag(r, oid);
	if (peeled) {
		commit = lookup_commit_in_graph(r, peeled);
		if (commit) {
			struct tag *tag = lookup_tag(r, oid);
			if (tag) {
				tag->object.parsed = 1;
				tag->tagged = &commit->object;
				tag->date = commit->date;
				if (mark_tag_flags)
					tag->object.flags |= mark_tag_flags;
			}
			return commit;
		}
	}

	return NULL;
}

int odb_warp_synthesize_header(struct repository *r,
			       const struct object_id *oid,
			       const struct object_id *peeled_hint,
			       enum object_type *type_out,
			       size_t *size_out,
			       void **content_out)
{
	struct commit *c;
	const struct object_id *peeled;

	if (!r || !oid)
		return -1;

	c = lookup_commit_in_graph(r, oid);
	if (c) {
		struct strbuf sb = STRBUF_INIT;
		struct commit_list *p;

		strbuf_addf(&sb, "tree %s\n", oid_to_hex(get_commit_tree_oid(c)));
		for (p = c->parents; p; p = p->next)
			strbuf_addf(&sb, "parent %s\n", oid_to_hex(&p->item->object.oid));
		strbuf_addf(&sb, "author <> %"PRItime" +0000\n", c->date);
		strbuf_addf(&sb, "committer <> %"PRItime" +0000\n\n", c->date);

		*type_out = OBJ_COMMIT;
		*content_out = strbuf_detach(&sb, size_out);
		return 0;
	}

	peeled = peeled_hint ? peeled_hint : odb_warp_lookup_peeled_tag(r, oid);
	if (peeled) {
		c = lookup_commit_in_graph(r, peeled);
		if (c) {
			struct strbuf sb = STRBUF_INIT;

			strbuf_addf(&sb, "object %s\ntype commit\ntag v\n",
				    oid_to_hex(peeled));
			strbuf_addf(&sb, "tagger <> %"PRItime" +0000\n\n", c->date);

			*type_out = OBJ_TAG;
			*content_out = strbuf_detach(&sb, size_out);
			return 0;
		}
	}

	return -1;
}

#endif /* ODB_WARP_IMPLEMENTATION */
#endif /* ODB_WARP_H */
