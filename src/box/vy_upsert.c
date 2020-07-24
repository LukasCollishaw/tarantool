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
#include "vy_upsert.h"

#include <sys/uio.h>
#include <small/region.h>
#include <msgpuck/msgpuck.h>
#include "vy_stmt.h"
#include "xrow_update.h"
#include "fiber.h"
#include "column_mask.h"

/**
 * Try to squash two upsert series (msgspacked index_base + ops)
 * Try to create a tuple with squahed operations
 *
 * @retval 0 && *result_stmt != NULL : successful squash
 * @retval 0 && *result_stmt == NULL : unsquashable sources
 * @retval -1 - memory error
 */
static int
vy_upsert_try_to_squash(struct tuple_format *format,
			const char *key_mp, const char *key_mp_end,
			const char *old_serie, const char *old_serie_end,
			const char *new_serie, const char *new_serie_end,
			struct tuple **result_stmt)
{
	*result_stmt = NULL;

	size_t squashed_size;
	const char *squashed =
		xrow_upsert_squash(old_serie, old_serie_end,
				   new_serie, new_serie_end, format,
				   &squashed_size, 0);
	if (squashed == NULL)
		return 0;
	/* Successful squash! */
	struct iovec operations[1];
	operations[0].iov_base = (void *)squashed;
	operations[0].iov_len = squashed_size;

	*result_stmt = vy_stmt_new_upsert(format, key_mp, key_mp_end,
					  operations, 1);
	if (*result_stmt == NULL)
		return -1;
	return 0;
}

/**
 * Check that key hasn't been changed after applying upsert operation.
 */
bool
vy_apply_result_does_cross_pk(struct tuple *old_stmt, const char *new_key,
			      struct key_def *cmp_def, uint64_t col_mask)
{
	return (!key_update_can_be_skipped(cmp_def->column_mask, col_mask) &&
		vy_stmt_compare_with_raw_key(old_stmt, HINT_NONE, new_key,
			HINT_NONE, cmp_def));
}

/**
 * @cmp_def Key definition required to provide check of primary key
 * modification.
 */
static int
vy_apply_upsert_on_terminal_stmt(struct tuple *new_stmt, struct tuple *old_stmt,
				 struct key_def *cmp_def, struct tuple **result,
				 bool suppress_error) {
//	say_error("VY_APPLY_TERMINAL");
	assert(vy_stmt_type(new_stmt) == IPROTO_UPSERT);
	assert(old_stmt == NULL || vy_stmt_type(old_stmt) != IPROTO_UPSERT);

	*result = NULL;
	uint32_t mp_size;
	const char *new_ops = vy_stmt_upsert_ops(new_stmt, &mp_size);
	const char *result_mp;
	if (old_stmt != NULL && vy_stmt_type(old_stmt) != IPROTO_DELETE)
		result_mp = tuple_data_range(old_stmt, &mp_size);
	else
		result_mp = vy_upsert_data_range(new_stmt, &mp_size);
	const char *result_mp_end = result_mp + mp_size;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	uint64_t column_mask = COLUMN_MASK_FULL;
	struct tuple_format *format = tuple_format(new_stmt);

	assert(mp_typeof(*new_ops) == MP_ARRAY);
	uint32_t ups_cnt = mp_decode_array(&new_ops);
	const char *ups_ops = new_ops;
	/*
	 * In case upsert folds into insert, we must skip first
	 * update operations.
	 */
	if (old_stmt == NULL || vy_stmt_type(old_stmt) == IPROTO_DELETE) {
		ups_cnt--;
		mp_next(&ups_ops);
	}
	for (uint32_t i = 0; i < ups_cnt; ++i) {
		const char *ups_ops_end = ups_ops;
		mp_next(&ups_ops_end);
//		char *buf = tt_static_buf();
//		mp_snprint(buf, 1024, ups_ops);
//		say_error("OPS %d %s",i, buf);
//		mp_snprint(buf, 1024, result_mp);
//		say_error("result_mp %s", buf);
		assert(mp_typeof(*ups_ops) == MP_ARRAY);
		result_mp = xrow_upsert_execute(ups_ops, ups_ops_end, result_mp,
						result_mp_end, format, &mp_size,
						0, suppress_error, &column_mask);
		/*
		 * If it turns out that resulting tuple modifies primary
		 * key, than simply ignore this upsert.
		 */
		if (vy_apply_result_does_cross_pk(old_stmt, result_mp, cmp_def,
						  column_mask)) {
			say_error("Primary key has been modified");
			result_mp = ups_ops;
			result_mp_end = result_mp + mp_size;
			continue;
		}
		ups_ops = ups_ops_end;
		if (result_mp == NULL) {
			region_truncate(region, region_svp);
			return -1;
		}
//		mp_snprint(buf, 1024, result_mp);
//		say_error("result_mp %d %s", i, buf);
		result_mp_end = result_mp + mp_size;
	}
	struct tuple *ups_res = vy_stmt_new_replace(format, result_mp,
						    result_mp_end);
	region_truncate(region, region_svp);
	if (ups_res == NULL)
		return -1;
	vy_stmt_set_lsn(ups_res, vy_stmt_lsn(new_stmt));
	/*
	 * No need to check PK modifications in case upsert
	 * folds into insert.
	 */
	if (old_stmt == NULL || vy_stmt_type(old_stmt) == IPROTO_DELETE) {
		*result = ups_res;
		return 0;
	}
	
	*result = ups_res;
	return 0;
}

static bool
vy_format_is_suitable_for_squash(struct tuple_format *format)
{
	struct tuple_field *field;
	json_tree_foreach_entry_preorder(field, &format->fields.root,
					 struct tuple_field, token) {
		if (field->type == FIELD_TYPE_UNSIGNED)
				return false;
	}
	return true;
}

/** Unpack upsert's update operations into array of iovecs. */
static void
upsert_ops_to_iovec(const char *ops, uint32_t ops_cnt, struct iovec *iov_arr)
{
	for (uint32_t i = 0; i < ops_cnt; ++i) {
		assert(mp_typeof(*ops) == MP_ARRAY);
		iov_arr[i].iov_base = (char *) ops;
		mp_next(&ops);
		iov_arr[i].iov_len = ops - (char *) iov_arr[i].iov_base;
	}
}

struct tuple *
vy_apply_upsert(struct tuple *new_stmt, struct tuple *old_stmt,
		struct key_def *cmp_def, bool suppress_error)
{
	/*
	 * old_stmt - previous (old) version of stmt
	 * new_stmt - next (new) version of stmt
	 * result_stmt - the result of merging new and old
	 */
	assert(new_stmt != NULL);
	assert(new_stmt != old_stmt);
	assert(vy_stmt_type(new_stmt) == IPROTO_UPSERT);
	say_error("new_stmt %s", vy_stmt_str(new_stmt));
	say_error("old_stmt %s", vy_stmt_str(old_stmt));


	struct tuple *result_stmt = NULL;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	if (old_stmt == NULL || vy_stmt_type(old_stmt) != IPROTO_UPSERT) {
		if (vy_apply_upsert_on_terminal_stmt(new_stmt, old_stmt,
						     cmp_def, &result_stmt,
						     suppress_error) != 0)
			return NULL;
		say_error("terminal result %s", vy_stmt_str(result_stmt));
		return result_stmt;
	}

	assert(vy_stmt_type(old_stmt) == IPROTO_UPSERT);
	/*
	 * Unpack UPSERT operation from the old stmt
	 */
	assert(old_stmt != NULL);
	uint32_t mp_size;
	const char *old_ops = vy_stmt_upsert_ops(old_stmt, &mp_size);
	const char *old_ops_end = old_ops + mp_size;

	assert(old_ops_end > old_ops);
	const char *new_ops = vy_stmt_upsert_ops(new_stmt, &mp_size);
	const char *old_stmt_mp = vy_upsert_data_range(old_stmt, &mp_size);
	const char *old_stmt_mp_end = old_stmt_mp + mp_size;

	/*
	 * UPSERT + UPSERT case: squash arithmetic operations.
	 * Note that we can process this only in case result
	 * can't break format under no circumstances. Since
	 * subtraction can lead to negative values, unsigned
	 * field are considered to be inappropriate.
	 */
	struct tuple_format *format = tuple_format(old_stmt);
	if (vy_format_is_suitable_for_squash(format)) {
		const char *new_ops = vy_stmt_upsert_ops(new_stmt, &mp_size);
		const char *new_ops_end = new_ops + mp_size;
		if (vy_upsert_try_to_squash(format, old_stmt_mp, old_stmt_mp_end,
					    old_ops, old_ops_end, new_ops,
					    new_ops_end, &result_stmt) != 0) {
			/* OOM */
			region_truncate(region, region_svp);
			return NULL;
		}
	}

	/*
	 * Adding update operations. We keep order of update operations in
	 * the array the same. It is vital since first set of operations
	 * must be skipped in case upsert folds into insert. For instance:
	 * old_ops = {{{op1}, {op2}}, {{op3}}}
	 * new_ops = {{{op4}, {op5}}}
	 * res_ops = {{{op1}, {op2}}, {{op3}}, {{op4}, {op5}}}
	 * If upsert corresponding to old_ops becomes insert, then
	 * {{op1}, {op2}} update operations are not applied.
	 */
	uint32_t old_ops_cnt = mp_decode_array(&old_ops);
	uint32_t new_ops_cnt = mp_decode_array(&new_ops);
	size_t ops_size = sizeof(struct iovec) * (old_ops_cnt + new_ops_cnt);
	struct iovec *operations = region_alloc(region, ops_size);
	if (operations == NULL) {
		region_truncate(region, region_svp);
		diag_set(OutOfMemory, ops_size, "region_alloc", "operations");
		return NULL;
	}
	upsert_ops_to_iovec(old_ops, old_ops_cnt, operations);
	upsert_ops_to_iovec(new_ops, new_ops_cnt, &operations[old_ops_cnt]);

//	char *buf = tt_static_buf();
//	for (uint32_t i = 0; i < new_ops_cnt + old_ops_cnt; ++i) {
//		mp_snprint(buf, 1024, operations[i].iov_base);
//		say_error("new ops %d %s", i, buf);
//	}
//	mp_snprint(buf, 1024, old_stmt_mp);
//	say_error("old_stmt_mp %s", buf);
	result_stmt = vy_stmt_new_upsert(format, old_stmt_mp, old_stmt_mp_end,
					 operations, old_ops_cnt + new_ops_cnt);
	region_truncate(region, region_svp);
	if (result_stmt == NULL)
		return NULL;
	vy_stmt_set_lsn(result_stmt, vy_stmt_lsn(new_stmt));
//	say_error("upsert merger %s", vy_stmt_str(result_stmt));

	return result_stmt;
}
