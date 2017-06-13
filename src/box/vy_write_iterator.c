/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_write_iterator.h"
#include "vy_mem.h"
#include "vy_run.h"
#include "vy_upsert.h"
#include "column_mask.h"

#define HEAP_FORWARD_DECLARATION
#include "salad/heap.h"

static bool
heap_less(heap_t *heap, struct heap_node *n1, struct heap_node *n2);

#define HEAP_NAME src_heap
#define HEAP_LESS heap_less
#include "salad/heap.h"

/**
 * Merge source of write iterator. Represents a mem or a run.
 */
struct vy_write_src {
	/* Link in vy_write_iterator::src_list */
	struct rlist in_src_list;
	/* Node in vy_write_iterator::src_heap */
	struct heap_node heap_node;
	/* Current tuple in the source (lowest and with maximal lsn) */
	struct tuple *tuple;
	/**
	 * There are special rules of comparison for virtual sources
	 * that represent a delimiter between the current key and
	 * the next key. They must be after (greater than) the sources with
	 * equal key despite of LSN. The flag below mean that the source is
	 * such a virtual source and must be compared correspondingly.
	 */
	bool is_end_of_key;
	/** Source iterator */
	union {
		struct vy_slice_stream slice_stream;
		struct vy_mem_stream mem_stream;
		struct vy_stmt_stream stream;
	};
};

/** Read view of a key. */
struct vy_read_view_stmt {
	/** Key version. */
	struct tuple *stmt;
	/** Read view LSN. */
	int64_t vlsn;
};

/**
 * Write iterator itself.
 */
struct vy_write_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_stream base;
	/* List of all sources of the iterator */
	struct rlist src_list;
	/* Heap with sources with the lowest source in head */
	heap_t src_heap;
	/** Index key definition used for storing statements on disk. */
	const struct key_def *key_def;
	/** Format to allocate new REPLACE and DELETE tuples from vy_run */
	struct tuple_format *format;
	/** Same as format, but for UPSERT tuples. */
	struct tuple_format *upsert_format;
	/* There are is no level older than the one we're writing to. */
	bool is_last_level;
	/** Set if this iterator is for a primary index. */
	bool is_primary;

	/** Length of the @read_views. */
	int rv_count;
	/**
	 * Current read view statement in @read_views. It is used
	 * to return key versions one by one from
	 * vy_write_iterator_next.
	 */
	int stmt_i;
	/**
	 * Read views of the same key, sorted by lsn in
	 * descending order and started from the INT64_MAX. Each
	 * is referenced if needed. Example:
	 * stmt_count = 3
	 * rv_count = 6
	 *    0      1      2     3     4     5
	 * [lsn=6, lsn=5, lsn=4,  -,    -,    -]
	 *
	 * @Read_views can have gaps, if there are read views with
	 * the same key versions. Example:
	 *
	 * LSN:                  20     -    -    -    10    9   8
	 * Read views:           *      *    *    *    *
	 * @read_views array: [lsn=20,  -,   -,   -, lsn=10, -, -]
	 *                             \___________________/
	 *                           Same versions of the key.
	 */
	struct vy_read_view_stmt read_views[0];
};

/**
 * Comparator of the heap. Generally compares two sources and finds out
 * whether one source is less than another.
 */
static bool
heap_less(heap_t *heap, struct heap_node *node1, struct heap_node *node2)
{
	struct vy_write_iterator *stream =
		container_of(heap, struct vy_write_iterator, src_heap);
	struct vy_write_src *src1 =
		container_of(node1, struct vy_write_src, heap_node);
	struct vy_write_src *src2 =
		container_of(node2, struct vy_write_src, heap_node);

	int cmp = tuple_compare(src1->tuple, src2->tuple, stream->key_def);
	if (cmp != 0)
		return cmp < 0;

	/**
	 * Keys are equal, order by lsn, descending.
	 * Virtual sources that represents end-of-key just use 0 as LSN,
	 * so they are after all equal keys automatically.
	 */
	int64_t lsn1 = src1->is_end_of_key  ? 0 : vy_stmt_lsn(src1->tuple);
	int64_t lsn2 = src2->is_end_of_key  ? 0 : vy_stmt_lsn(src2->tuple);
	if (lsn1 != lsn2)
		return lsn1 > lsn2;

	/** LSNs are equal, prioritize terminal (non-upsert) statements */
	return (vy_stmt_type(src1->tuple) == IPROTO_UPSERT ? 1 : 0) <
	       (vy_stmt_type(src2->tuple) == IPROTO_UPSERT ? 1 : 0);

}

/**
 * Allocate a source and put it to the list.
 * The underlying stream (src->stream) must be opened immediately.
 * @param stream - the write iterator.
 * @return the source or NULL on memory error.
 */
static struct vy_write_src *
vy_write_iterator_new_src(struct vy_write_iterator *stream)
{
	struct vy_write_src *res = (struct vy_write_src *) malloc(sizeof(*res));
	if (res == NULL) {
		diag_set(OutOfMemory, sizeof(*res),
			 "malloc", "write stream src");
		return NULL;
	}
	res->is_end_of_key = false;
	rlist_add(&stream->src_list, &res->in_src_list);
	return res;
}


/**
 * Close the stream of the source, remove from list and delete.
 */
static void
vy_write_iterator_delete_src(struct vy_write_iterator *stream,
			     struct vy_write_src *src)
{
	(void)stream;
	assert(!src->is_end_of_key);
	if (src->stream.iface->stop != NULL)
		src->stream.iface->stop(&src->stream);
	if (src->stream.iface->close != NULL)
		src->stream.iface->close(&src->stream);
	rlist_del(&src->in_src_list);
	free(src);
}

/**
 * Put the source to the heap. Source's stream must be opened.
 * @return 0 - success, not 0 - error.
 */
static NODISCARD int
vy_write_iterator_add_src(struct vy_write_iterator *stream,
			  struct vy_write_src *src)
{
	if (src->stream.iface->start != NULL) {
		int rc = src->stream.iface->start(&src->stream);
		if (rc != 0) {
			vy_write_iterator_delete_src(stream, src);
			return rc;
		}
	}
	int rc = src->stream.iface->next(&src->stream, &src->tuple);
	if (rc != 0 || src->tuple == NULL) {
		vy_write_iterator_delete_src(stream, src);
		return rc;
	}
	rc = src_heap_insert(&stream->src_heap, &src->heap_node);
	if (rc != 0) {
		diag_set(OutOfMemory, sizeof(void *),
			 "malloc", "write stream heap");
		vy_write_iterator_delete_src(stream, src);
		return rc;
	}
	return 0;
}

/**
 * Remove the source from the heap, destroy and free it.
 */
static void
vy_write_iterator_remove_src(struct vy_write_iterator *stream,
			   struct vy_write_src *src)
{
	src_heap_delete(&stream->src_heap, &src->heap_node);
	vy_write_iterator_delete_src(stream, src);
}

static const struct vy_stmt_stream_iface vy_slice_stream_iface;

/**
 * Open an empty write iterator. To add sources to the iterator
 * use vy_write_iterator_add_* functions.
 * @return the iterator or NULL on error (diag is set).
 */
struct vy_stmt_stream *
vy_write_iterator_new(const struct key_def *key_def, struct tuple_format *format,
		      struct tuple_format *upsert_format, bool is_primary,
		      bool is_last_level, struct rlist *read_views)
{
	/*
	 * One is reserved for the INT64_MAX - maximal read view.
	 */
	int count = 1;
	struct rlist *unused;
	rlist_foreach(unused, read_views)
		++count;
	size_t size = sizeof(struct vy_write_iterator) +
		      count * sizeof(struct vy_read_view_stmt);
	struct vy_write_iterator *stream =
		(struct vy_write_iterator *) calloc(1, size);
	if (stream == NULL) {
		diag_set(OutOfMemory, size, "malloc", "write stream");
		return NULL;
	}
	stream->rv_count = count;
	stream->read_views[0].vlsn = INT64_MAX;
	count--;
	struct vy_read_view *rv;
	/* Descending order. */
	rlist_foreach_entry(rv, read_views, in_read_views)
		stream->read_views[count--].vlsn = rv->vlsn;
	assert(count == 0);

	stream->base.iface = &vy_slice_stream_iface;
	src_heap_create(&stream->src_heap);
	rlist_create(&stream->src_list);
	stream->key_def = key_def;
	stream->format = format;
	tuple_format_ref(stream->format, 1);
	stream->upsert_format = upsert_format;
	tuple_format_ref(stream->upsert_format, 1);
	stream->is_primary = is_primary;
	stream->is_last_level = is_last_level;
	return &stream->base;
}

/**
 * Start the search. Must be called after *add* methods and
 * before *next* method.
 * @return 0 on success or not 0 on error (diag is set).
 */
static int
vy_write_iterator_start(struct vy_stmt_stream *vstream)
{
	assert(vstream->iface->start == vy_write_iterator_start);
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	struct vy_write_src *src, *tmp;
	rlist_foreach_entry_safe(src, &stream->src_list, in_src_list, tmp) {
		if (vy_write_iterator_add_src(stream, src) != 0)
			return -1;
	}
	return 0;
}

/**
 * Free all resources.
 */
static void
vy_write_iterator_stop(struct vy_stmt_stream *vstream)
{
	assert(vstream->iface->stop == vy_write_iterator_stop);
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	for (int i = 0; i < stream->rv_count; ++i) {
		struct vy_read_view_stmt *rv = &stream->read_views[i];
		vy_stmt_unref_if_possible(rv->stmt);
	}
	struct vy_write_src *src, *tmp;
	rlist_foreach_entry_safe(src, &stream->src_list, in_src_list, tmp)
		vy_write_iterator_delete_src(stream, src);
}

/**
 * Delete the iterator.
 */
static void
vy_write_iterator_close(struct vy_stmt_stream *vstream)
{
	assert(vstream->iface->close == vy_write_iterator_close);
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	vy_write_iterator_stop(vstream);
	tuple_format_ref(stream->format, -1);
	tuple_format_ref(stream->upsert_format, -1);
	free(stream);
}

/**
 * Add a mem as a source of iterator.
 * @return 0 on success or not 0 on error (diag is set).
 */
NODISCARD int
vy_write_iterator_add_mem(struct vy_stmt_stream *vstream, struct vy_mem *mem)
{
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	struct vy_write_src *src = vy_write_iterator_new_src(stream);
	if (src == NULL)
		return -1;
	vy_mem_stream_open(&src->mem_stream, mem);
	return 0;
}

/**
 * Add a run slice as a source of iterator.
 * @return 0 on success or not 0 on error (diag is set).
 */
NODISCARD int
vy_write_iterator_add_slice(struct vy_stmt_stream *vstream,
			    struct vy_slice *slice, struct vy_run_env *run_env)
{
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	struct vy_write_src *src = vy_write_iterator_new_src(stream);
	if (src == NULL)
		return -1;
	vy_slice_stream_open(&src->slice_stream, slice, stream->key_def,
			     stream->format, stream->upsert_format, run_env,
			     stream->is_primary);
	return 0;
}

/**
 * Go to next tuple in terms of sorted (merged) input steams.
 * @return 0 on success or not 0 on error (diag is set).
 */
static NODISCARD int
vy_write_iterator_merge_step(struct vy_write_iterator *stream)
{
	struct heap_node *node = src_heap_top(&stream->src_heap);
	assert(node != NULL);
	struct vy_write_src *src = container_of(node, struct vy_write_src,
						heap_node);
	int rc = src->stream.iface->next(&src->stream, &src->tuple);
	if (rc != 0)
		return rc;
	if (src->tuple != NULL)
		src_heap_update(&stream->src_heap, node);
	else
		vy_write_iterator_remove_src(stream, src);
	return 0;
}

/**
 * Try to get VLSN of the read view with the specified number in
 * the vy_write_iterator.read_views array.
 * If the requested read view is older than all existing ones,
 * return 0, as the oldest possible VLSN.
 *
 * @param stream Write iterator.
 * @param current_rv_i Number of the read view.
 *
 * @retval VLSN.
 */
static inline int64_t
vy_get_read_view_lsn(struct vy_write_iterator *stream, int current_rv_i)
{
	if (current_rv_i >= stream->rv_count)
		return 0;
	return stream->read_views[current_rv_i].vlsn;
}

/**
 * Remember the @tuple as the read view of the key with the
 * specified number.
 * @param stream Write iterator.
 * @param tuple Key version.
 * @param current_rv_i Number of the current read view.
 */
static inline void
vy_write_iterator_add_read_view_stmt(struct vy_write_iterator *stream,
				     struct tuple *tuple, int current_rv_i)
{
	assert(current_rv_i < stream->rv_count);
	struct vy_read_view_stmt *rv = &stream->read_views[current_rv_i];
	assert(rv->stmt == NULL);
	rv->stmt = tuple;
	vy_stmt_ref_if_possible(tuple);
	assert(rv->vlsn >= vy_stmt_lsn(tuple));
}

/**
 * Return the next statement from the current key read view
 * statements sequence. Unref the previous statement, if needed.
 * We can't unref the statement right before returning it to the
 * caller, because reference in the read_views array can be
 * single reference of this statement, and unref could delete it
 * before returning.
 *
 * @param stream Write iterator.
 * @retval not NULL Next statement of the current key.
 * @retval     NULL End of the key (not the end of the sources).
 */
static inline struct tuple *
vy_write_iterator_pop_read_view_stmt(struct vy_write_iterator *stream)
{
	struct vy_read_view_stmt *rv;
	if (stream->stmt_i > 0) {
		/* Unref the previous. */
		rv = &stream->read_views[stream->stmt_i - 1];
		vy_stmt_unref_if_possible(rv->stmt);
		rv->stmt = NULL;
	}
next_version:
	if (stream->stmt_i == stream->rv_count)
		return NULL;
	assert(stream->stmt_i < stream->rv_count);
	rv = &stream->read_views[stream->stmt_i];
	stream->stmt_i++;
	if (rv->stmt == NULL)
		goto next_version;
	return rv->stmt;
}

/**
 * Split the next key in the sequence of the read view statements.
 * @sa struct vy_write_iterator comment for details about
 * algorithm and optimizations.
 *
 * @param stream Write iterator.
 * @param[out] count Length of the result key versions sequence.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
static NODISCARD int
vy_write_iterator_next_key(struct vy_write_iterator *stream, int *count)
{
	*count = 0;
	assert(stream->stmt_i == 0);
	struct heap_node *node = src_heap_top(&stream->src_heap);
	if (node == NULL)
		return 0; /* no more data */
	struct vy_write_src *src =
		container_of(node, struct vy_write_src, heap_node);
	/* Search must be started in the task. */
	assert(src->tuple != NULL);
	/*
	 * A virtual source instance that represents the end on current key in
	 * source heap. Due to a special branch in heap's comparator the
	 * source will come into the heap head after all equal to the current
	 * key statement but before any greater statement. Having inserted
	 * the source to the heap, the moment we get this source from the heap
	 * signals that there are no statements that are equal to the current.
	 */
	struct vy_write_src end_of_key_src;
	end_of_key_src.is_end_of_key = true;
	end_of_key_src.tuple = src->tuple;
	if (vy_stmt_is_refable(src->tuple))
		tuple_ref(src->tuple);
	int rc = src_heap_insert(&stream->src_heap, &end_of_key_src.heap_node);
	if (rc) {
		diag_set(OutOfMemory, sizeof(void *),
			 "malloc", "write stream heap");
		return rc;
	}
	/*
	 * On each step merge statements between merge_until_lsn
	 * and curr_rv_lsn. (merge_until_lsn <= curr_rv_lsn).
	 * current_rv_i - index of the current read view.
	 */
	int current_rv_i = 0;
	int64_t current_rv_lsn = vy_get_read_view_lsn(stream, 0);
	int64_t merge_until_lsn = vy_get_read_view_lsn(stream, 1);
	uint64_t key_mask = stream->key_def->column_mask;
	/* Merged sequence of upserts. */
	struct tuple *upserts_seq = NULL;

	while (true) {
		/*
		 * Skip statements, unvisible by the current read
		 * view and unused by the previous read view.
		 * Used by Optimizations
		 * 1: skip last level DELETE.
		 * 2: skip after REPLACE/DELETE;
		 */
		if (vy_stmt_lsn(src->tuple) > current_rv_lsn)
			goto next_step;
		/*
		 * The iterator reached the border of two read
		 * views.
		 */
		if (vy_stmt_lsn(src->tuple) <= merge_until_lsn) {
			/*
			 * Before moving to the next read view
			 * check, if some UPSERTs were merged for
			 * the current read view.
			 */
			if (upserts_seq != NULL) {
				vy_write_iterator_add_read_view_stmt(stream,
								     upserts_seq,
								     current_rv_i);
				++*count;
				if (vy_stmt_is_refable(upserts_seq))
					tuple_unref(upserts_seq);
				upserts_seq = NULL;
			}
			/*
			 * Skip read views, which have the same
			 * versions of the key.
			 * The src->tuple must be between
			 * merge_until_lsn and current_rv_lsn.
			 */
			do {
				current_rv_i++;
				current_rv_lsn = merge_until_lsn;
				merge_until_lsn = vy_get_read_view_lsn(stream,
								       current_rv_i + 1);
			} while (vy_stmt_lsn(src->tuple) <= merge_until_lsn);
		}

		/*
		 * Optimization 1: skip last level delete.
		 * @sa vy_write_iterator for details about this
		 * and other optimizations.
		 */
		if (vy_stmt_type(src->tuple) == IPROTO_DELETE &&
		    stream->is_last_level && merge_until_lsn == 0) {
			current_rv_lsn = 0;
			goto next_step;
		}

		/*
		 * Optimization 2: skip after REPLACE/DELETE.
		 */
		if (vy_stmt_type(src->tuple) == IPROTO_REPLACE ||
		    vy_stmt_type(src->tuple) == IPROTO_DELETE) {
			uint64_t stmt_mask = vy_stmt_column_mask(src->tuple);
			/*
			 * Optimization 3: skip statements, which
			 * do not update the secondary key.
			 */
			if (!stream->is_primary &&
			    key_update_can_be_skipped(key_mask, stmt_mask) &&
			    upserts_seq == NULL)
				goto next_step;

			/*
			 * Apply accumulated upserts, if possible.
			 */
			if (upserts_seq != NULL) {
				struct tuple *applied =
					vy_apply_upsert(upserts_seq, src->tuple,
							stream->key_def,
							stream->format,
							stream->upsert_format,
							false);
				if (vy_stmt_is_refable(upserts_seq))
					tuple_unref(upserts_seq);
				upserts_seq = NULL;
				if (applied == NULL) {
					rc = -1;
					break;
				}
				vy_write_iterator_add_read_view_stmt(stream,
								     applied,
								     current_rv_i);
				tuple_unref(applied);
			} else {
				vy_write_iterator_add_read_view_stmt(stream,
								     src->tuple,
								     current_rv_i);
			}
			++*count;
			current_rv_i++;
			current_rv_lsn = merge_until_lsn;
			merge_until_lsn = vy_get_read_view_lsn(stream,
							       current_rv_i + 1);
			goto next_step;
		}

		assert(vy_stmt_type(src->tuple) == IPROTO_UPSERT);
		/*
		 * Simply remember the first upsert in the
		 * sequence.
		 */
		if (upserts_seq == NULL) {
			upserts_seq = src->tuple;
			if (vy_stmt_is_refable(upserts_seq))
				tuple_ref(upserts_seq);
		} else {
			/*
			 * Merge upserts sequence in the one
			 * variable to either apply it to a
			 * REPLACE/DELETE or remember as read
			 * view later.
			 */
			struct tuple *applied =
				vy_apply_upsert(upserts_seq, src->tuple,
						stream->key_def, stream->format,
						stream->upsert_format, false);
			if (vy_stmt_is_refable(upserts_seq))
				tuple_unref(upserts_seq);
			upserts_seq = applied;
			if (applied == NULL) {
				rc = -1;
				break;
			}
		}

next_step:
		rc = vy_write_iterator_merge_step(stream);
		if (rc != 0)
			break;
		node = src_heap_top(&stream->src_heap);
		assert(node != NULL);
		src = container_of(node, struct vy_write_src, heap_node);
		assert(src->tuple != NULL);
		if (src->is_end_of_key) {
			/* Key can end with UPSERTs sequence. */
			if (upserts_seq != NULL) {
				vy_write_iterator_add_read_view_stmt(stream,
								     upserts_seq,
								     current_rv_i);
				if (vy_stmt_is_refable(upserts_seq))
					tuple_unref(upserts_seq);
				++*count;
			}
			break;
		}
	}

	src_heap_delete(&stream->src_heap, &end_of_key_src.heap_node);
	if (vy_stmt_is_refable(end_of_key_src.tuple))
		tuple_unref(end_of_key_src.tuple);
	return rc;
}

/**
 * Get the next statement to write.
 * The user of the write iterator simply expects a stream
 * of statements to write to the output.
 * The tuple *ret is guaranteed to be valid until next tuple is
 *  returned (thus last non-null tuple is valid after EOF).
 *
 * @return 0 on success or not 0 on error (diag is set).
 */
static NODISCARD int
vy_write_iterator_next(struct vy_stmt_stream *vstream,
		       struct tuple **ret)
{
	assert(vstream->iface->next == vy_write_iterator_next);
	struct vy_write_iterator *stream = (struct vy_write_iterator *)vstream;
	/*
	 * Try to get the next statement from the current key
	 * read view statements sequence.
	 */
	*ret = vy_write_iterator_pop_read_view_stmt(stream);
	if (*ret != NULL)
		return 0;

	/* Build the next key sequence. */
	stream->stmt_i = 0;
	int count = 0;

	while (true) {
		/* Squash upserts and/or go to the next key */
		if (vy_write_iterator_next_key(stream, &count) != 0)
			return -1;
		/*
		 * Next_key() routine could skip the next key, for
		 * example, if it was truncated by last level
		 * DELETE or it consisted only from optimized
		 * updates. Then try get the next key.
		 */
		if (count != 0 || stream->src_heap.size == 0)
			break;
	}
	/*
	 * Again try to get the statement, after calling next_key.
	 */
	*ret = vy_write_iterator_pop_read_view_stmt(stream);
	return 0;
}

static const struct vy_stmt_stream_iface vy_slice_stream_iface = {
	.start = vy_write_iterator_start,
	.next = vy_write_iterator_next,
	.stop = vy_write_iterator_stop,
	.close = vy_write_iterator_close
};

