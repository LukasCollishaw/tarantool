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
//static int
//vy_upsert_try_to_squash(struct tuple_format *format,
//			const char *key_mp, const char *key_mp_end,
//			const char *old_serie, const char *old_serie_end,
//			const char *new_serie, const char *new_serie_end,
//			struct tuple **result_stmt)
//{
//	*result_stmt = NULL;
//
//	size_t squashed_size;
//	const char *squashed =
//		xrow_upsert_squash(old_serie, old_serie_end,
//				   new_serie, new_serie_end, format,
//				   &squashed_size, 0);
//	if (squashed == NULL)
//		return 0;
//	/* Successful squash! */
//	struct iovec operations[1];
//	operations[0].iov_base = (void *)squashed;
//	operations[0].iov_len = squashed_size;
//
//	*result_stmt = vy_stmt_new_upsert(format, key_mp, key_mp_end,
//					  operations, 1);
//	if (*result_stmt == NULL)
//		return -1;
//	return 0;
//}

/**
 * Check that key hasn't been changed after applying upsert operation.
 */
bool
vy_apply_result_does_cross_pk(struct tuple *new_stmt, struct tuple *old_stmt,
			      struct key_def *cmp_def, uint64_t col_mask)
{
	return (!key_update_can_be_skipped(cmp_def->column_mask, col_mask) &&
		vy_stmt_compare(old_stmt, HINT_NONE, new_stmt, HINT_NONE, cmp_def));
}

/**
 * @cmp_def Key definition required to provide check of primary key
 * modification.
 **/
static int
vy_apply_upsert_on_terminal_stmt(struct tuple *new_stmt, struct tuple *old_stmt,
				 struct key_def *cmp_def, struct tuple **result,
				 bool suppress_error) {
	assert(vy_stmt_type(new_stmt) == IPROTO_UPSERT);
	assert(old_stmt == NULL || vy_stmt_type(old_stmt) != IPROTO_UPSERT);

	*result = NULL;
	uint32_t mp_size;
	const char *new_ops = vy_stmt_upsert_ops(new_stmt, &mp_size);
	const char *new_ops_end = new_ops + mp_size;
	const char *result_mp;
	if (old_stmt != NULL && vy_stmt_type(old_stmt) != IPROTO_DELETE)
		result_mp = tuple_data_range(old_stmt, &mp_size);
	else
		result_mp = vy_upsert_data_range(new_stmt, &mp_size);
	const char *result_mp_end = result_mp + mp_size;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	uint64_t column_mask = COLUMN_MASK_FULL;
	struct tuple_format *format = old_stmt != NULL ? tuple_format(old_stmt) : tuple_format(new_stmt);

	if (old_stmt != NULL || vy_stmt_type(old_stmt) != IPROTO_DELETE) {
		result_mp = xrow_upsert_execute(new_ops, new_ops_end, result_mp,
						result_mp_end, format, &mp_size,
						0, suppress_error, &column_mask);
		if (result_mp == NULL) {
			region_truncate(region, region_svp);
			return -1;
		}
		result_mp_end = result_mp + mp_size;
	}
	struct tuple *ups_res = vy_stmt_new_replace(format, result_mp,
						    result_mp_end);
	region_truncate(region, region_svp);
	if (ups_res == NULL)
		return -1;
	vy_stmt_set_lsn(ups_res, vy_stmt_lsn(new_stmt));
	/*
	 * If it turns out that resulting tuple modifies primary
	 * key, than simply ignore this upsert.
	 * TODO: integrate this check into xrow_upsert_execute()
	 * so that *all* update operations of given upsert
	 * are skipped.
	 */
	if (vy_apply_result_does_cross_pk(ups_res, old_stmt, cmp_def,
					  column_mask)) {
		say_error("PK MODIFIED!!!");
		tuple_unref(ups_res);
		ups_res = vy_stmt_dup(old_stmt);
	}
	*result = ups_res;
	return 0;
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
		/* INSERT case: return new stmt. */
		if (vy_apply_upsert_on_terminal_stmt(new_stmt, old_stmt, cmp_def,
						      &result_stmt,
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
	const char *new_ops_end = new_ops + mp_size;

	const char *old_stmt_mp = vy_upsert_data_range(old_stmt, &mp_size);
	const char *old_stmt_mp_end = old_stmt_mp + mp_size;

	/*
	 * UPSERT + UPSERT case: combine operations
	 */
//	assert(old_ops_end - old_ops > 0);
//	if (vy_upsert_try_to_squash(format, result_mp, result_mp_end,
//				    old_ops, old_ops_end, new_ops, new_ops_end,
//				    &result_stmt) != 0) {
//		region_truncate(region, region_svp);
//		return NULL;
//	}
//	if (result_stmt != NULL) {
//		region_truncate(region, region_svp);
//		vy_stmt_set_lsn(result_stmt, vy_stmt_lsn(new_stmt));
//		goto check_key;
//	}


	/*
	 * Adding update operations. We keep order of update operations in
	 * the array the same (it is vital since first set of operations
	 * must be skipped in case upsert folds into insert). Consider:
	 * old_ops = {{op1}, {op2}, {op3}}
	 * new_ops = {{op4}, {op5}}
	 * res_ops = {{{op1}, {op2}, {op3}}, {{op4}, {op5}}}
	 *
	 */
	int old_ops_cnt, new_ops_cnt;
	struct iovec operations[3];

	old_ops_cnt = mp_decode_array(&old_ops);
	if (mp_typeof(old_ops) == MP_ARRAY) {
		say_error("old_osp");
	}
	operations[1].iov_base = (void *)old_ops;
	operations[1].iov_len = old_ops_end - old_ops;

	new_ops_cnt = mp_decode_array(&new_ops);
	operations[2].iov_base = (void *)new_ops;
	operations[2].iov_len = new_ops_end - new_ops;

	char ops_buf[16];
	char *header = mp_encode_array(ops_buf, old_ops_cnt + new_ops_cnt);
	operations[0].iov_base = (void *)ops_buf;
	operations[0].iov_len = header - ops_buf;

	struct tuple_format *format = tuple_format(old_stmt);
	result_stmt = vy_stmt_new_upsert(format, old_stmt_mp, old_stmt_mp_end,
					 operations, 3);
	region_truncate(region, region_svp);
	if (result_stmt == NULL)
		return NULL;
	vy_stmt_set_lsn(result_stmt, vy_stmt_lsn(new_stmt));
	say_error("upsert merger %s", vy_stmt_str(result_stmt));

	return result_stmt;
}
