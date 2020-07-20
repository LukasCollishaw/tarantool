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
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains the C-language implementations for many of the SQL
 * functions of sql.  (Some function, and in particular the date and
 * time functions, are implemented separately.)
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "version.h"
#include "coll/coll.h"
#include "tarantoolInt.h"
#include <unicode/ustring.h>
#include <unicode/ucasemap.h>
#include <unicode/ucnv.h>
#include <unicode/uchar.h>
#include <unicode/ucol.h>
#include "box/coll_id_cache.h"
#include "box/schema.h"
#include "box/func.h"
#include "box/port.h"
#include "box/tuple.h"
#include "lua/msgpack.h"
#include "lua/utils.h"
#include "mpstream/mpstream.h"

/*
 * Return the collating function associated with a function.
 */
static struct coll *
sqlGetFuncCollSeq(sql_context * context)
{
	VdbeOp *pOp;
	assert(context->pVdbe != 0);
	pOp = &context->pVdbe->aOp[context->iOp - 1];
	assert(pOp->opcode == OP_CollSeq);
	assert(pOp->p4type == P4_COLLSEQ || pOp->p4.pColl == NULL);
	return pOp->p4.pColl;
}

/*
 * Indicate that the accumulator load should be skipped on this
 * iteration of the aggregate loop.
 */
static void
sqlSkipAccumulatorLoad(sql_context * context)
{
	context->skipFlag = 1;
}

/**
 * Allocate a sequence of initialized vdbe memory registers
 * on region.
 */
static struct Mem *
vdbemem_alloc_on_region(uint32_t count)
{
	struct region *region = &fiber()->gc;
	size_t size;
	struct Mem *ret = region_alloc_array(region, typeof(*ret), count,
					     &size);
	if (ret == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_array", "ret");
		return NULL;
	}
	memset(ret, 0, count * sizeof(*ret));
	for (uint32_t i = 0; i < count; i++) {
		sqlVdbeMemInit(&ret[i], sql_get(), MEM_Null);
		assert(memIsValid(&ret[i]));
	}
	return ret;
}

static void
port_vdbemem_dump_lua(struct port *base, struct lua_State *L, bool is_flat)
{
	(void) is_flat;
	struct port_vdbemem *port = (struct port_vdbemem *) base;
	assert(is_flat == true);
	for (uint32_t i = 0; i < port->mem_count; i++) {
		sql_value *param = port->mem + i;
		switch (sql_value_type(param)) {
		case MP_INT:
			luaL_pushint64(L, sql_value_int64(param));
			break;
		case MP_UINT:
			luaL_pushuint64(L, sql_value_uint64(param));
			break;
		case MP_DOUBLE:
			lua_pushnumber(L, sql_value_double(param));
			break;
		case MP_STR:
			lua_pushstring(L, (const char *) sql_value_text(param));
			break;
		case MP_BIN:
		case MP_ARRAY:
		case MP_MAP:
			lua_pushlstring(L, sql_value_blob(param),
					(size_t) sql_value_bytes(param));
			break;
		case MP_NIL:
			lua_pushnil(L);
			break;
		case MP_BOOL:
			lua_pushboolean(L, sql_value_boolean(param));
			break;
		default:
			unreachable();
		}
	}
}

static const char *
port_vdbemem_get_msgpack(struct port *base, uint32_t *size)
{
	struct port_vdbemem *port = (struct port_vdbemem *) base;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	bool is_error = false;
	struct mpstream stream;
	mpstream_init(&stream, region, region_reserve_cb, region_alloc_cb,
		      set_encode_error, &is_error);
	mpstream_encode_array(&stream, port->mem_count);
	for (uint32_t i = 0; i < port->mem_count && !is_error; i++) {
		sql_value *param = port->mem + i;
		switch (sql_value_type(param)) {
		case MP_INT: {
			sql_int64 val = sql_value_int64(param);
			if (val < 0) {
				mpstream_encode_int(&stream, val);
				break;
			}
			FALLTHROUGH;
		}
		case MP_UINT: {
			sql_uint64 val = sql_value_uint64(param);
			mpstream_encode_uint(&stream, val);
			break;
		}
		case MP_DOUBLE: {
			mpstream_encode_double(&stream,
					       sql_value_double(param));
			break;
		}
		case MP_STR: {
			const char *str = (const char *) sql_value_text(param);
			mpstream_encode_str(&stream, str);
			break;
		}
		case MP_BIN:
		case MP_ARRAY:
		case MP_MAP: {
			mpstream_encode_binl(&stream, sql_value_bytes(param));
			mpstream_memcpy(&stream, sql_value_blob(param),
					sql_value_bytes(param));
			break;
		}
		case MP_NIL: {
			mpstream_encode_nil(&stream);
			break;
		}
		case MP_BOOL: {
			mpstream_encode_bool(&stream, sql_value_boolean(param));
			break;
		}
		default:
			unreachable();
		}
	}
	mpstream_flush(&stream);
	*size = region_used(region) - region_svp;
	if (is_error)
		goto error;
	const char *ret = (char *)region_join(region, *size);
	if (ret == NULL)
		goto error;
	return ret;
error:
	diag_set(OutOfMemory, *size, "region", "ret");
	return NULL;
}

static const struct port_vtab port_vdbemem_vtab;

void
port_vdbemem_create(struct port *base, struct Mem *mem, uint32_t mem_count)
{
	struct port_vdbemem *port = (struct port_vdbemem *) base;
	port->vtab = &port_vdbemem_vtab;
	port->mem = mem;
	port->mem_count = mem_count;
}

static struct Mem *
port_vdbemem_get_vdbemem(struct port *base, uint32_t *mem_count)
{
	struct port_vdbemem *port = (struct port_vdbemem *) base;
	assert(port->vtab == &port_vdbemem_vtab);
	*mem_count = port->mem_count;
	return port->mem;
}

static const struct port_vtab port_vdbemem_vtab = {
	.dump_msgpack = NULL,
	.dump_msgpack_16 = NULL,
	.dump_lua = port_vdbemem_dump_lua,
	.dump_plain = NULL,
	.get_msgpack = port_vdbemem_get_msgpack,
	.get_vdbemem = port_vdbemem_get_vdbemem,
	.destroy = NULL,
};

struct Mem *
port_lua_get_vdbemem(struct port *base, uint32_t *size)
{
	struct port_lua *port = (struct port_lua *) base;
	struct lua_State *L = port->L;
	int argc = lua_gettop(L);
	if (argc == 0 || argc > 1) {
		diag_set(ClientError, ER_SQL_FUNC_WRONG_RET_COUNT, "Lua", argc);
		return NULL;
	}
	*size = argc;
	/** FIXME: Implement an ability to return a vector. */
	assert(*size == 1);
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	struct Mem *val = vdbemem_alloc_on_region(argc);
	if (val == NULL)
		return NULL;
	for (int i = 0; i < argc; i++) {
		struct luaL_field field;
		if (luaL_tofield(L, luaL_msgpack_default,
				 NULL, -1 - i, &field) < 0) {
			goto error;
		}
		switch (field.type) {
		case MP_BOOL:
			mem_set_bool(&val[i], field.bval);
			break;
		case MP_FLOAT:
			mem_set_double(&val[i], field.fval);
			break;
		case MP_DOUBLE:
			mem_set_double(&val[i], field.dval);
			break;
		case MP_INT:
			mem_set_i64(&val[i], field.ival);
			break;
		case MP_UINT:
			mem_set_u64(&val[i], field.ival);
			break;
		case MP_STR:
			if (sqlVdbeMemSetStr(&val[i], field.sval.data,
					     field.sval.len, 1,
					     SQL_TRANSIENT) != 0)
				goto error;
			break;
		case MP_NIL:
			sqlVdbeMemSetNull(&val[i]);
			break;
		default:
			diag_set(ClientError, ER_SQL_EXECUTE,
				 "Unsupported type passed from Lua");
			goto error;
		}
	}
	return val;
error:
	for (int i = 0; i < argc; i++)
		sqlVdbeMemRelease(&val[i]);
	region_truncate(region, region_svp);
	return NULL;
}

struct Mem *
port_c_get_vdbemem(struct port *base, uint32_t *size)
{
	struct port_c *port = (struct port_c *)base;
	*size = port->size;
	if (*size == 0 || *size > 1) {
		diag_set(ClientError, ER_SQL_FUNC_WRONG_RET_COUNT, "C", *size);
		return NULL;
	}
	/** FIXME: Implement an ability to return a vector. */
	assert(*size == 1);
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	struct Mem *val = vdbemem_alloc_on_region(port->size);
	if (val == NULL)
		return NULL;
	int i = 0;
	const char *data;
	struct port_c_entry *pe;
	for (pe = port->first; pe != NULL; pe = pe->next) {
		if (pe->mp_size == 0) {
			data = tuple_data(pe->tuple);
			if (mp_decode_array(&data) != 1) {
				diag_set(ClientError, ER_SQL_EXECUTE,
					 "Unsupported type passed from C");
				goto error;
			}
		} else {
			data = pe->mp;
		}
		uint32_t len;
		const char *str;
		switch (mp_typeof(*data)) {
		case MP_BOOL:
			mem_set_bool(&val[i], mp_decode_bool(&data));
			break;
		case MP_FLOAT:
			mem_set_double(&val[i], mp_decode_float(&data));
			break;
		case MP_DOUBLE:
			mem_set_double(&val[i], mp_decode_double(&data));
			break;
		case MP_INT:
			mem_set_i64(&val[i], mp_decode_int(&data));
			break;
		case MP_UINT:
			mem_set_u64(&val[i], mp_decode_uint(&data));
			break;
		case MP_STR:
			str = mp_decode_str(&data, &len);
			if (sqlVdbeMemSetStr(&val[i], str, len,
					     1, SQL_TRANSIENT) != 0)
				goto error;
			break;
		case MP_NIL:
			sqlVdbeMemSetNull(&val[i]);
			break;
		default:
			diag_set(ClientError, ER_SQL_EXECUTE,
				 "Unsupported type passed from C");
			goto error;
		}
		i++;
	}
	return val;
error:
	for (int i = 0; i < port->size; i++)
		sqlVdbeMemRelease(&val[i]);
	region_truncate(region, region_svp);
	return NULL;
}

/*
 * Implementation of the non-aggregate min() and max() functions
 */
static int
sql_func_least_greatest(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	uint32_t i;
	int iBest;
	struct coll *pColl;
	int mask = sql_func_flag_is_set(base, SQL_FUNC_MAX) ? -1 : 0;
	if (argc < 2) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT,
		mask ? "GREATEST" : "LEAST", "at least two", argc);
		return -1;
	}
	pColl = sqlGetFuncCollSeq(func->context);
	assert(mask == -1 || mask == 0);
	iBest = 0;
	if (sql_value_is_null(&argv[0])) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		if (sql_value_is_null(&argv[i])) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		if ((sqlMemCompare(&argv[iBest], &argv[i], pColl) ^ mask) >=
		    0) {
			testcase(mask == 0);
			iBest = i;
		}
	}
	struct Mem *result = sqlValueNew(sql_get());
	sqlVdbeMemCopy(result, &argv[iBest]);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Return the type of the argument.
 */
static int
sql_func_typeof(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	const char *z = 0;
	struct Mem *result = sqlValueNew(sql_get());
	enum field_type f_t = argv[0].field_type;
	/*
	 * SCALAR is not a basic type, but rather an aggregation of
	 * types. Thus, ignore SCALAR field type and return msgpack
	 * format type.
	 */
	if (f_t != field_type_MAX && f_t != FIELD_TYPE_SCALAR &&
	    f_t != FIELD_TYPE_ANY) {
		z = field_type_strs[argv[0].field_type];
		if (sqlVdbeMemSetStr(result, z, -1, 1, SQL_STATIC) != 0)
			return -1;
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	switch (sql_value_type(&argv[0])) {
	case MP_INT:
	case MP_UINT:
		z = "integer";
		break;
	case MP_STR:
		z = "string";
		break;
	case MP_DOUBLE:
		z = "double";
		break;
	case MP_BIN:
	case MP_ARRAY:
	case MP_MAP:
		z = "varbinary";
		break;
	case MP_BOOL:
	case MP_NIL:
		z = "boolean";
		break;
	default:
		unreachable();
		break;
	}
	if (sqlVdbeMemSetStr(result, z, -1, 1, SQL_STATIC) != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of the length() function
 */
static int
sql_func_length(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	int len;

	assert(argc == 1);
	UNUSED_PARAMETER(argc);
	switch (sql_value_type(&argv[0])) {
	case MP_BIN:
	case MP_ARRAY:
	case MP_MAP:
	case MP_INT:
	case MP_UINT:
	case MP_BOOL:
	case MP_DOUBLE: {
			len = sql_value_bytes(&argv[0]);
			break;
		}
	case MP_STR:{
			const unsigned char *z = sql_value_text(&argv[0]);
			if (z == NULL) {
				port_vdbemem_create(ret, NULL, 0);
				return 0;
			}
			len = sql_utf8_char_count(z, sql_value_bytes(&argv[0]));
			break;
		}
	default:{
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
	}
	struct Mem *result = sqlValueNew(sql_get());
	mem_set_int(result, len, false);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of the abs() function.
 *
 * IMP: R-23979-26855 The abs(X) function returns the absolute value of
 * the numeric argument X.
 */
static int
sql_func_abs(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	struct Mem *result = sqlValueNew(sql_get());

	switch (sql_value_type(&argv[0])) {
	case MP_UINT: {
		mem_set_u64(result, sql_value_uint64(&argv[0]));
		break;
	}
	case MP_INT: {
		int64_t value = sql_value_int64(&argv[0]);
		assert(value < 0);
		mem_set_u64(result, (uint64_t)(-value));
		break;
	}
	case MP_NIL:{
			/* IMP: R-37434-19929 Abs(X) returns NULL if X is NULL. */
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
	case MP_BOOL:
	case MP_BIN:
	case MP_ARRAY:
	case MP_MAP: {
		diag_set(ClientError, ER_INCONSISTENT_TYPES, "number",
			 mem_type_to_str(&argv[0]));
		return -1;
	}
	default:{
			/* Because sql_value_double() returns 0.0 if the argument is not
			 * something that can be converted into a number, we have:
			 * IMP: R-01992-00519 Abs(X) returns 0.0 if X is a string or blob
			 * that cannot be converted to a numeric value.
			 */
			double rVal = sql_value_double(&argv[0]);
			if (rVal < 0)
				rVal = -rVal;
			mem_set_double(result, rVal);
			break;
		}
	}

	port_vdbemem_create(ret, result, 1);
	return 0;
}

/**
 * Implementation of the position() function.
 *
 * position(needle, haystack) finds the first occurrence of needle
 * in haystack and returns the number of previous characters
 * plus 1, or 0 if needle does not occur within haystack.
 *
 * If both haystack and needle are BLOBs, then the result is one
 * more than the number of bytes in haystack prior to the first
 * occurrence of needle, or 0 if needle never occurs in haystack.
 */
static int
sql_func_position(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 2);

	struct Mem *result;
	struct Mem *needle = &argv[0];
	struct Mem *haystack = &argv[1];
	enum mp_type needle_type = sql_value_type(needle);
	enum mp_type haystack_type = sql_value_type(haystack);

	if (haystack_type == MP_NIL || needle_type == MP_NIL) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	/*
	 * Position function can be called only with string
	 * or blob params.
	 */
	struct Mem *inconsistent_type_arg = NULL;
	if (needle_type != MP_STR && needle_type != MP_BIN)
		inconsistent_type_arg = needle;
	if (haystack_type != MP_STR && haystack_type != MP_BIN)
		inconsistent_type_arg = haystack;
	if (inconsistent_type_arg != NULL) {
		diag_set(ClientError, ER_INCONSISTENT_TYPES,
			 "text or varbinary",
			 mem_type_to_str(inconsistent_type_arg));
		return -1;
	}
	/*
	 * Both params of Position function must be of the same
	 * type.
	 */
	if (haystack_type != needle_type) {
		diag_set(ClientError, ER_INCONSISTENT_TYPES,
			 mem_type_to_str(needle), mem_type_to_str(haystack));
		return -1;
	}

	int n_needle_bytes = sql_value_bytes(needle);
	int n_haystack_bytes = sql_value_bytes(haystack);
	uint64_t position = 1;
	if (n_needle_bytes > 0) {
		const unsigned char *haystack_str;
		const unsigned char *needle_str;
		if (haystack_type == MP_BIN) {
			needle_str = sql_value_blob(needle);
			haystack_str = sql_value_blob(haystack);
			assert(needle_str != NULL);
			assert(haystack_str != NULL || n_haystack_bytes == 0);
			/*
			 * Naive implementation of substring
			 * searching: matching time O(n * m).
			 * Can be improved.
			 */
			while (n_needle_bytes <= n_haystack_bytes &&
			       memcmp(haystack_str, needle_str, n_needle_bytes) != 0) {
				position++;
				n_haystack_bytes--;
				haystack_str++;
			}
			if (n_needle_bytes > n_haystack_bytes)
				position = 0;
		} else {
			/*
			 * Code below handles not only simple
			 * cases like position('a', 'bca'), but
			 * also more complex ones:
			 * position('a', 'bcá' COLLATE "unicode_ci")
			 * To do so, we need to use comparison
			 * window, which has constant character
			 * size, but variable byte size.
			 * Character size is equal to
			 * needle char size.
			 */
			haystack_str = sql_value_text(haystack);
			needle_str = sql_value_text(needle);

			int n_needle_chars =
				sql_utf8_char_count(needle_str, n_needle_bytes);
			int n_haystack_chars =
				sql_utf8_char_count(haystack_str,
						    n_haystack_bytes);

			if (n_haystack_chars < n_needle_chars) {
				position = 0;
				goto finish;
			}
			/*
			 * Comparison window is determined by
			 * beg_offset and end_offset. beg_offset
			 * is offset in bytes from haystack
			 * beginning to window beginning.
			 * end_offset is offset in bytes from
			 * haystack beginning to window end.
			 */
			int end_offset = 0;
			for (int c = 0; c < n_needle_chars; c++) {
				SQL_UTF8_FWD_1(haystack_str, end_offset,
					       n_haystack_bytes);
			}
			int beg_offset = 0;
			struct coll *coll = sqlGetFuncCollSeq(func->context);
			int c;
			for (c = 0; c + n_needle_chars <= n_haystack_chars; c++) {
				if (coll->cmp((const char *) haystack_str + beg_offset,
					      end_offset - beg_offset,
					      (const char *) needle_str,
					      n_needle_bytes, coll) == 0)
					goto finish;
				position++;
				/* Update offsets. */
				SQL_UTF8_FWD_1(haystack_str, beg_offset,
					       n_haystack_bytes);
				SQL_UTF8_FWD_1(haystack_str, end_offset,
					       n_haystack_bytes);
			}
			/* Needle was not found in the haystack. */
			position = 0;
		}
	}
finish:
	result = sqlValueNew(sql_get());
	mem_set_u64(result, position);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of the printf() function.
 */
static int
sql_func_printf(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	PrintfArguments x;
	StrAccum str;
	const char *zFormat;
	int n;
	sql *db = sql_get();

	if (argc >= 1
	    && (zFormat = (const char *)sql_value_text(&argv[0])) != 0) {
		struct region *region = &fiber()->gc;
		size_t region_svp = region_used(region);
		uint32_t size = (argc - 1) * sizeof(struct Mem *);
		struct Mem **array =
			region_aligned_alloc(region, size, alignof(struct Mem));
		for (uint32_t i = 0; i < argc - 1; ++i)
			array[i] = &argv[i + 1];
		x.nArg = argc - 1;
		x.nUsed = 0;
		x.apArg = array;
		sqlStrAccumInit(&str, db, 0, 0,
				    db->aLimit[SQL_LIMIT_LENGTH]);
		str.printfFlags = SQL_PRINTF_SQLFUNC;
		sqlXPrintf(&str, zFormat, &x);
		region_truncate(region, region_svp);
		n = str.nChar;
		struct Mem *result = sqlValueNew(db);
		const char *z = sqlStrAccumFinish(&str);
		if (sqlVdbeMemSetStr(result, z, n, 1, SQL_DYNAMIC) != 0)
			return -1;
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

/*
 * Implementation of the substr() function.
 *
 * substr(x,p1,p2)  returns p2 characters of x[] beginning with p1.
 * p1 is 1-indexed.  So substr(x,1,1) returns the first character
 * of x.  If x is text, then we actually count UTF-8 characters.
 * If x is a blob, then we count bytes.
 *
 * If p1 is negative, then we begin abs(p1) from the end of x[].
 *
 * If p2 is negative, return the p2 characters preceding p1.
 */
static int
sql_func_substr(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	const unsigned char *z;
	const unsigned char *z2;
	int len;
	int p0type;
	i64 p1, p2;
	int negP2 = 0;

	if (argc != 2 && argc != 3) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT, "SUBSTR",
			 "1 or 2", argc);
		return -1;
	}
	if (sql_value_is_null(&argv[1])
	    || (argc == 3 && sql_value_is_null(&argv[2]))
	    ) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	p0type = sql_value_type(&argv[0]);
	p1 = sql_value_int(&argv[1]);
	if (p0type == MP_BIN) {
		len = sql_value_bytes(&argv[0]);
		z = sql_value_blob(&argv[0]);
		if (z == NULL) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		assert(len == sql_value_bytes(&argv[0]));
	} else {
		z = sql_value_text(&argv[0]);
		if (z == NULL) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		len = 0;
		if (p1 < 0)
			len = sql_utf8_char_count(z, sql_value_bytes(&argv[0]));
	}
	if (argc == 3) {
		p2 = sql_value_int(&argv[2]);
		if (p2 < 0) {
			p2 = -p2;
			negP2 = 1;
		}
	} else {
		p2 = sql_get()->aLimit[SQL_LIMIT_LENGTH];
	}
	if (p1 < 0) {
		p1 += len;
		if (p1 < 0) {
			p2 += p1;
			if (p2 < 0)
				p2 = 0;
			p1 = 0;
		}
	} else if (p1 > 0) {
		p1--;
	} else if (p2 > 0) {
		p2--;
	}
	if (negP2) {
		p1 -= p2;
		if (p1 < 0) {
			p2 += p1;
			p1 = 0;
		}
	}
	assert(p1 >= 0 && p2 >= 0);
	struct Mem *result = sqlValueNew(sql_get());
	if (p0type != MP_BIN) {
		/*
		 * In the code below 'cnt' and 'n_chars' is
		 * used because '\0' is not supposed to be
		 * end-of-string symbol.
		 */
		int byte_size = sql_value_bytes(&argv[0]);
		int n_chars = sql_utf8_char_count(z, byte_size);
		int cnt = 0;
		int i = 0;
		while (cnt < n_chars && p1) {
			SQL_UTF8_FWD_1(z, i, byte_size);
			cnt++;
			p1--;
		}
		z += i;
		i = 0;
		for (z2 = z; cnt < n_chars && p2; p2--) {
			SQL_UTF8_FWD_1(z2, i, byte_size);
			cnt++;
		}
		z2 += i;
		if (sqlVdbeMemSetStr(result, (char *)z, z2 - z, 1,
				     SQL_TRANSIENT) != 0)
			return -1;
	} else {
		if (p1 + p2 > len) {
			p2 = len - p1;
			if (p2 < 0)
				p2 = 0;
		}
		if (sqlVdbeMemSetStr(result, (char *)&z[p1], (uint64_t)p2, 1,
				     SQL_TRANSIENT) != 0)
			return -1;
	}
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of the round() function
 */
static int
sql_func_round(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	int n = 0;
	double r;
	if (argc != 1 && argc != 2) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT, "ROUND",
			 "1 or 2", argc);
		return -1;
	}
	if (argc == 2) {
		if (sql_value_is_null(&argv[1])) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		n = sql_value_int(&argv[1]);
		if (n < 0)
			n = 0;
	}
	if (sql_value_is_null(&argv[0])) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	enum mp_type mp_type = sql_value_type(&argv[0]);
	if (mp_type_is_bloblike(mp_type)) {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 sql_value_to_diag_str(&argv[0]), "numeric");
		return -1;
	}
	r = sql_value_double(&argv[0]);
	/* If Y==0 and X will fit in a 64-bit int,
	 * handle the rounding directly,
	 * otherwise use printf.
	 */
	if (n == 0 && r >= 0 && r < LARGEST_INT64 - 1) {
		r = (double)((sql_int64) (r + 0.5));
	} else if (n == 0 && r < 0 && (-r) < LARGEST_INT64 - 1) {
		r = -(double)((sql_int64) ((-r) + 0.5));
	} else {
		const char *rounded_value = tt_sprintf("%.*f", n, r);
		sqlAtoF(rounded_value, &r, sqlStrlen30(rounded_value));
	}
	struct Mem *result = sqlValueNew(sql_get());
	mem_set_double(result, r);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Allocate nByte bytes of space using sqlMalloc(). If the
 * allocation fails, return NULL. If nByte is larger than the
 * maximum string or blob length, then raise an error and return
 * NULL.
 */
static void *
contextMalloc(sql_context * context, i64 nByte)
{
	char *z;
	sql *db = sql_context_db_handle(context);
	assert(nByte > 0);
	testcase(nByte == db->aLimit[SQL_LIMIT_LENGTH]);
	testcase(nByte == db->aLimit[SQL_LIMIT_LENGTH] + 1);
	if (nByte > db->aLimit[SQL_LIMIT_LENGTH]) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		context->is_aborted = true;
		z = 0;
	} else {
		z = sqlMalloc(nByte);
		if (z == NULL)
			context->is_aborted = true;
	}
	return z;
}

/*
 * Implementation of the upper() and lower() SQL functions.
 */
static int
icu_lower_upper(struct func *base, struct port *args, struct port *ret,
		bool is_lower)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	if (mp_type_is_bloblike(sql_value_type(&argv[0]))) {
		diag_set(ClientError, ER_INCONSISTENT_TYPES, "text",
			 "varbinary");
		return -1;
	}
	const char *z2 = (const char *)sql_value_text(&argv[0]);
	if (z2 == NULL) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	int n = sql_value_bytes(&argv[0]);
	char *z1 = contextMalloc(func->context, n + 1);
	if (z1 == NULL)
		return -1;
	UErrorCode status = U_ZERO_ERROR;
	struct coll *coll = sqlGetFuncCollSeq(func->context);
	const char *locale = NULL;
	if (coll != NULL && coll->type == COLL_TYPE_ICU) {
		locale = ucol_getLocaleByType(coll->collator,
					      ULOC_VALID_LOCALE, &status);
	}
	UCaseMap *case_map = ucasemap_open(locale, 0, &status);
	assert(case_map != NULL);
	int len;
	if (is_lower)
		len = ucasemap_utf8ToLower(case_map, z1, n, z2, n, &status);
	else
		len = ucasemap_utf8ToUpper(case_map, z1, n, z2, n, &status);
	if (len > n) {
		status = U_ZERO_ERROR;
		sql_free(z1);
		z1 = contextMalloc(func->context, len + 1);
		if (z1 == NULL)
			return -1;
		if (is_lower)
			ucasemap_utf8ToLower(case_map, z1, len, z2, n, &status);
		else
			ucasemap_utf8ToUpper(case_map, z1, len, z2, n, &status);
	}
	ucasemap_close(case_map);

	struct Mem *result = sqlValueNew(sql_get());
	if (sqlVdbeMemSetStr(result, z1, len, 1, sql_free) != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

static int
sql_func_lower(struct func *base, struct port *args, struct port *ret)
{
	return icu_lower_upper(base, args, ret, true);
}

static int
sql_func_upper(struct func *base, struct port *args, struct port *ret)
{
	return icu_lower_upper(base, args, ret, false);
}

/*
 * Some functions like COALESCE() and IFNULL() and UNLIKELY() are implemented
 * as VDBE code so that unused argument values do not have to be computed.
 * However, we still need some kind of function implementation for this
 * routines in the function table.  The noopFunc macro provides this.
 * noopFunc will never be called so it doesn't matter what the implementation
 * is.  We might as well use the "version()" function as a substitute.
 */
#define noopFunc sql_func_version /* Substitute function - never called */

/*
 * Implementation of random().  Return a random integer.
 */
static int
sql_func_random(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	int64_t r;
	sql_randomness(sizeof(r), &r);

	struct Mem *result = sqlValueNew(sql_get());
	mem_set_int(result, r, r < 0);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of randomblob(N).  Return a random blob
 * that is N bytes long.
 */
static int
sql_func_randomblob(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	int n;
	unsigned char *p;
	if (mp_type_is_bloblike(sql_value_type(&argv[0]))) {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 sql_value_to_diag_str(&argv[0]), "numeric");
		return -1;
	}
	n = sql_value_int(&argv[0]);
	if (n < 1) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	p = contextMalloc(func->context, n);
	if (p) {
		sql_randomness(n, p);
		struct Mem *result = sqlValueNew(sql_get());
		if (sqlVdbeMemSetStr(result, (char *)p, n, 0, sql_free) != 0)
			return -1;
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	return -1;
}

#define Utf8Read(s, e) \
	ucnv_getNextUChar(icu_utf8_conv, &(s), (e), &status)

#define SQL_END_OF_STRING        0xffff
#define SQL_INVALID_UTF8_SYMBOL  0xfffd

/**
 * Returns codes from sql_utf8_pattern_compare().
 */
enum pattern_match_status {
	MATCH = 0,
	NO_MATCH = 1,
	/** No match in spite of having * or % wildcards. */
	NO_WILDCARD_MATCH = 2,
	/** Pattern contains invalid UTF-8 symbol. */
	INVALID_PATTERN = 3
};

/**
 * Read an UTF-8 character from string, and move pointer to the
 * next character. Save current character and its length to output
 * params - they are served as arguments of coll->cmp() call.
 *
 * @param[out] str Ptr to string.
 * @param str_end Ptr the last symbol in @str.
 * @param[out] char_ptr Ptr to the UTF-8 character.
 * @param[out] char_len Ptr to length of the UTF-8 character in
 * bytes.
 *
 * @retval UTF-8 character.
 */
static UChar32
step_utf8_char(const char **str, const char *str_end, const char **char_ptr,
	       size_t *char_len)
{
	UErrorCode status = U_ZERO_ERROR;
	*char_ptr = *str;
	UChar32 next_utf8 = Utf8Read(*str, str_end);
	*char_len = *str - *char_ptr;
	return next_utf8;
}

/**
 * Compare two UTF-8 strings for equality where the first string
 * is a LIKE expression.
 *
 * Like matching rules:
 *
 *      '%'       Matches any sequence of zero or more
 *                characters.
 *
 *      '_'       Matches any one character.
 *
 *      Ec        Where E is the "esc" character and c is any
 *                other character, including '%', '_', and esc,
 *                match exactly c.
 *
 * This routine is usually quick, but can be N**2 in the worst
 * case.
 *
 * 'pattern_end' and 'string_end' params are used to determine
 * the end of strings, because '\0' is not supposed to be
 * end-of-string signal.
 *
 * @param pattern String containing comparison pattern.
 * @param string String being compared.
 * @param pattern_end Ptr to pattern last symbol.
 * @param string_end Ptr to string last symbol.
 * @param coll Pointer to collation.
 * @param match_other The escape char for LIKE.
 *
 * @retval One of pattern_match_status values.
 */
static int
sql_utf8_pattern_compare(const char *pattern,
			 const char *string,
			 const char *pattern_end,
			 const char *string_end,
			 struct coll *coll,
			 UChar32 match_other)
{
	/* Next pattern and input string chars. */
	UChar32 c, c2;
	/* One past the last escaped input char. */
	const char *zEscaped = 0;
	UErrorCode status = U_ZERO_ERROR;
	const char *pat_char_ptr = NULL;
	const char *str_char_ptr = NULL;
	size_t pat_char_len = 0;
	size_t str_char_len = 0;

	while (pattern < pattern_end) {
		c = step_utf8_char(&pattern, pattern_end, &pat_char_ptr,
				   &pat_char_len);
		if (c == SQL_INVALID_UTF8_SYMBOL)
			return INVALID_PATTERN;
		if (c == MATCH_ALL_WILDCARD) {
			/*
			 * Skip over multiple "%" characters in
			 * the pattern. If there are also "_"
			 * characters, skip those as well, but
			 * consume a single character of the
			 * input string for each "_" skipped.
			 */
			while ((c = step_utf8_char(&pattern, pattern_end,
						   &pat_char_ptr,
						   &pat_char_len)) !=
			       SQL_END_OF_STRING) {
				if (c == SQL_INVALID_UTF8_SYMBOL)
					return INVALID_PATTERN;
				if (c == MATCH_ONE_WILDCARD) {
					c2 = Utf8Read(string, string_end);
					if (c2 == SQL_INVALID_UTF8_SYMBOL)
						return NO_MATCH;
					if (c2 == SQL_END_OF_STRING)
						return NO_WILDCARD_MATCH;
				} else if (c != MATCH_ALL_WILDCARD) {
					break;
				}
			}
			/*
			 * "%" at the end of the pattern matches.
			 */
			if (c == SQL_END_OF_STRING) {
				return MATCH;
			}
			if (c == match_other) {
				c = step_utf8_char(&pattern, pattern_end,
						   &pat_char_ptr,
						   &pat_char_len);
				if (c == SQL_INVALID_UTF8_SYMBOL)
					return INVALID_PATTERN;
				if (c == SQL_END_OF_STRING)
					return NO_WILDCARD_MATCH;
			}

			/*
			 * At this point variable c contains the
			 * first character of the pattern string
			 * past the "%". Search in the input
			 * string for the first matching
			 * character and recursively continue the
			 * match from that point.
			 *
			 * For a case-insensitive search, set
			 * variable cx to be the same as c but in
			 * the other case and search the input
			 * string for either c or cx.
			 */

			int bMatch;
			while (string < string_end){
				/*
				 * This loop could have been
				 * implemented without if
				 * converting c2 to lower case
				 * by holding c_upper and
				 * c_lower,however it is
				 * implemented this way because
				 * lower works better with German
				 * and Turkish languages.
				 */
				c2 = step_utf8_char(&string, string_end,
						    &str_char_ptr,
						    &str_char_len);
				if (c2 == SQL_INVALID_UTF8_SYMBOL)
					return NO_MATCH;
				if (coll->cmp(pat_char_ptr, pat_char_len,
					      str_char_ptr, str_char_len, coll)
					!= 0)
					continue;
				bMatch = sql_utf8_pattern_compare(pattern,
								  string,
								  pattern_end,
								  string_end,
								  coll,
								  match_other);
				if (bMatch != NO_MATCH)
					return bMatch;
			}
			return NO_WILDCARD_MATCH;
		}
		if (c == match_other) {
			c = step_utf8_char(&pattern, pattern_end, &pat_char_ptr,
					   &pat_char_len);
			if (c == SQL_INVALID_UTF8_SYMBOL)
				return INVALID_PATTERN;
			if (c == SQL_END_OF_STRING)
				return NO_MATCH;
			zEscaped = pattern;
		}
		c2 = step_utf8_char(&string, string_end, &str_char_ptr,
				    &str_char_len);
		if (c2 == SQL_INVALID_UTF8_SYMBOL)
			return NO_MATCH;
		if (coll->cmp(pat_char_ptr, pat_char_len, str_char_ptr,
			      str_char_len, coll) == 0)
			continue;
		if (c == MATCH_ONE_WILDCARD && pattern != zEscaped &&
		    c2 != SQL_END_OF_STRING)
			continue;
		return NO_MATCH;
	}
	return string == string_end ? MATCH : NO_MATCH;
}

/**
 * Implementation of the like() SQL function. This function
 * implements the built-in LIKE operator. The first argument to
 * the function is the pattern and the second argument is the
 * string. So, the SQL statements of the following type:
 *
 *       A LIKE B
 *
 * are implemented as like(B,A).
 *
 * Both arguments (A and B) must be of type TEXT. If one arguments
 * is NULL then result is NULL as well.
 */
static int
sql_func_like(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	u32 escape = SQL_END_OF_STRING;
	int nPat;
	if (argc != 2 && argc != 3) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT,
			 "LIKE", "2 or 3", argc);
		return -1;
	}
	sql *db = sql_get();
	int rhs_type = sql_value_type(&argv[0]);
	int lhs_type = sql_value_type(&argv[1]);

	if (lhs_type != MP_STR || rhs_type != MP_STR) {
		if (lhs_type == MP_NIL || rhs_type == MP_NIL) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		char *inconsistent_type = rhs_type != MP_STR ?
					  mem_type_to_str(&argv[0]) :
					  mem_type_to_str(&argv[1]);
		diag_set(ClientError, ER_INCONSISTENT_TYPES, "text",
			 inconsistent_type);
		return -1;
	}
	const char *zB = (const char *) sql_value_text(&argv[0]);
	const char *zA = (const char *) sql_value_text(&argv[1]);
	const char *zB_end = zB + sql_value_bytes(&argv[0]);
	const char *zA_end = zA + sql_value_bytes(&argv[1]);

	/*
	 * Limit the length of the LIKE pattern to avoid problems
	 * of deep recursion and N*N behavior in
	 * sql_utf8_pattern_compare().
	 */
	nPat = sql_value_bytes(&argv[0]);
	testcase(nPat == db->aLimit[SQL_LIMIT_LIKE_PATTERN_LENGTH]);
	testcase(nPat == db->aLimit[SQL_LIMIT_LIKE_PATTERN_LENGTH] + 1);
	if (nPat > db->aLimit[SQL_LIMIT_LIKE_PATTERN_LENGTH]) {
		diag_set(ClientError, ER_SQL_EXECUTE, "LIKE pattern is too "\
			 "complex");
		return -1;
	}
	/* Encoding did not change */
	assert(zB == (const char *) sql_value_text(&argv[0]));

	if (argc == 3) {
		/*
		 * The escape character string must consist of a
		 * single UTF-8 character. Otherwise, return an
		 * error.
		 */
		const unsigned char *zEsc = sql_value_text(&argv[2]);
		if (zEsc == 0) {
			port_vdbemem_create(ret, NULL, 0);
			return 0;
		}
		if (sql_utf8_char_count(zEsc, sql_value_bytes(&argv[2])) != 1) {
			diag_set(ClientError, ER_SQL_EXECUTE, "ESCAPE "\
				 "expression must be a single character");
			return -1;
		}
		escape = sqlUtf8Read(&zEsc);
	}
	if (zA == NULL || zB == NULL) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	int res;
	struct coll *coll = sqlGetFuncCollSeq(func->context);
	assert(coll != NULL);
	res = sql_utf8_pattern_compare(zB, zA, zB_end, zA_end, coll, escape);

	if (res == INVALID_PATTERN) {
		diag_set(ClientError, ER_SQL_EXECUTE, "LIKE pattern can only "\
			 "contain UTF-8 characters");
		return -1;
	}

	struct Mem *result = sqlValueNew(sql_get());
	mem_set_bool(result, res == MATCH);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Implementation of the NULLIF(x,y) function.  The result is the first
 * argument if the arguments are different.  The result is NULL if the
 * arguments are equal to each other.
 */
static int
sql_func_nullif(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 2);

	struct Mem *result = sqlValueNew(sql_get());
	struct coll *pColl = sqlGetFuncCollSeq(func->context);
	if (sqlMemCompare(&argv[0], &argv[1], pColl) != 0) {
		sqlVdbeMemCopy(result, &argv[0]);
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

/**
 * Implementation of the version() function.  The result is the
 * version of the Tarantool that is running.
 */
static int
sql_func_version(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	const char *version = tarantool_version();
	struct Mem *result = sqlValueNew(sql_get());
	if (sqlVdbeMemSetStr(result, version, -1, 1, SQL_STATIC) != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/* Array for converting from half-bytes (nybbles) into ASCII hex
 * digits.
 */
static const char hexdigits[] = {
	'0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

/*
 * Implementation of the QUOTE() function.  This function takes a single
 * argument.  If the argument is numeric, the return value is the same as
 * the argument.  If the argument is NULL, the return value is the string
 * "NULL".  Otherwise, the argument is enclosed in single quotes with
 * single-quote escapes.
 */
static int
sql_func_quote(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	struct Mem *result = sqlValueNew(sql_get());

	switch (sql_value_type(&argv[0])) {
	case MP_DOUBLE:{
			double r1, r2;
			char zBuf[50];
			r1 = sql_value_double(&argv[0]);
			sql_snprintf(sizeof(zBuf), zBuf, "%!.15g", r1);
			sqlAtoF(zBuf, &r2, 20);
			if (r1 != r2) {
				sql_snprintf(sizeof(zBuf), zBuf, "%!.20e",
						 r1);
			}
			if (sqlVdbeMemSetStr(result, zBuf, -1, 1,
					     SQL_TRANSIENT) != 0)
				return -1;
			break;
		}
	case MP_UINT:
	case MP_INT:{
			sqlVdbeMemCopy(result, &argv[0]);
			break;
		}
	case MP_BIN:
	case MP_ARRAY:
	case MP_MAP: {
			char *zText = 0;
			char const *zBlob = sql_value_blob(&argv[0]);
			int nBlob = sql_value_bytes(&argv[0]);
			assert(zBlob == sql_value_blob(&argv[0]));	/* No encoding change */
			zText = (char *)contextMalloc(func->context,
						      (2 * (i64) nBlob) + 4);
			if (zText) {
				int i;
				for (i = 0; i < nBlob; i++) {
					zText[(i * 2) + 2] =
					    hexdigits[(zBlob[i] >> 4) & 0x0F];
					zText[(i * 2) + 3] =
					    hexdigits[(zBlob[i]) & 0x0F];
				}
				zText[(nBlob * 2) + 2] = '\'';
				zText[(nBlob * 2) + 3] = '\0';
				zText[0] = 'X';
				zText[1] = '\'';
				if (sqlVdbeMemSetStr(result, zText, -1, 1,
						     SQL_TRANSIENT) != 0)
					return -1;
				sql_free(zText);
			}
			break;
		}
	case MP_STR:{
			int i, j;
			u64 n;
			const unsigned char *zArg = sql_value_text(&argv[0]);
			char *z;

			if (zArg == 0) {
				port_vdbemem_create(ret, NULL, 0);
				return 0;
			}
			for (i = 0, n = 0; zArg[i]; i++) {
				if (zArg[i] == '\'')
					n++;
			}
			z = contextMalloc(func->context, i + ((i64) n) + 3);
			if (z) {
				z[0] = '\'';
				for (i = 0, j = 1; zArg[i]; i++) {
					z[j++] = zArg[i];
					if (zArg[i] == '\'') {
						z[j++] = '\'';
					}
				}
				z[j++] = '\'';
				z[j] = 0;
				if (sqlVdbeMemSetStr(result, z, j, 1,
						     sql_free) != 0)
					return -1;
			}
			break;
		}
	case MP_BOOL: {
		const char *z = SQL_TOKEN_BOOLEAN(sql_value_boolean(&argv[0]));
		if (sqlVdbeMemSetStr(result, z, -1, 1, SQL_STATIC) != 0)
			return -1;
		break;
	}
	default:{
			assert(sql_value_is_null(&argv[0]));
			if (sqlVdbeMemSetStr(result, "NULL", 4, 1,
					     SQL_STATIC) != 0)
				return -1;
			break;
		}
	}
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * The unicode() function.  Return the integer unicode code-point value
 * for the first character of the input string.
 */
static int
sql_func_unicode(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	struct Mem *result = sqlValueNew(sql_get());
	const unsigned char *z = sql_value_text(&argv[0]);
	if (z != NULL && z[0] != '\0') {
		mem_set_u64(result, sqlUtf8Read(&z));
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

/*
 * The char() function takes zero or more arguments, each of which is
 * an integer.  It constructs a string where each character of the string
 * is the unicode character for the corresponding integer argument.
 */
static int
sql_func_char(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	unsigned char *z, *zOut;
	zOut = z = sql_malloc64(argc * 4 + 1);
	if (z == NULL)
		return -1;
	for (uint32_t i = 0; i < argc; i++) {
		uint64_t x;
		unsigned c;
		if (sql_value_type(&argv[i]) == MP_INT)
			x = 0xfffd;
		else
			x = sql_value_uint64(&argv[i]);
		if (x > 0x10ffff)
			x = 0xfffd;
		c = (unsigned)(x & 0x1fffff);
		if (c < 0x00080) {
			*zOut++ = (u8) (c & 0xFF);
		} else if (c < 0x00800) {
			*zOut++ = 0xC0 + (u8) ((c >> 6) & 0x1F);
			*zOut++ = 0x80 + (u8) (c & 0x3F);
		} else if (c < 0x10000) {
			*zOut++ = 0xE0 + (u8) ((c >> 12) & 0x0F);
			*zOut++ = 0x80 + (u8) ((c >> 6) & 0x3F);
			*zOut++ = 0x80 + (u8) (c & 0x3F);
		} else {
			*zOut++ = 0xF0 + (u8) ((c >> 18) & 0x07);
			*zOut++ = 0x80 + (u8) ((c >> 12) & 0x3F);
			*zOut++ = 0x80 + (u8) ((c >> 6) & 0x3F);
			*zOut++ = 0x80 + (u8) (c & 0x3F);
		}
	}

	struct Mem *result = sqlValueNew(sql_get());
	if (sqlVdbeMemSetStr(result, (char *)z, zOut - z, 1, sql_free) != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * The hex() function.  Interpret the argument as a blob.  Return
 * a hexadecimal rendering as text.
 */
static int
sql_func_hex(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	int i, n;
	const unsigned char *pBlob;
	char *zHex, *z;
	pBlob = sql_value_blob(&argv[0]);
	n = sql_value_bytes(&argv[0]);
	assert(pBlob == sql_value_blob(&argv[0]));	/* No encoding change */
	z = zHex = contextMalloc(func->context, ((i64) n) * 2 + 1);
	if (zHex) {
		for (i = 0; i < n; i++, pBlob++) {
			unsigned char c = *pBlob;
			*(z++) = hexdigits[(c >> 4) & 0xf];
			*(z++) = hexdigits[c & 0xf];
		}
		*z = 0;

		struct Mem *result = sqlValueNew(sql_get());
		if (sqlVdbeMemSetStr(result, zHex, n * 2, 1, sql_free) != 0)
			return -1;
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

/*
 * The zeroblob(N) function returns a zero-filled blob of size N bytes.
 */
static int
sql_func_zeroblob(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	i64 n;
	n = sql_value_int64(&argv[0]);
	if (n < 0)
		n = 0;
	if (n > sql_get()->aLimit[SQL_LIMIT_LENGTH]) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or binary string"\
			 "is too big");
		return -1;
	}
	struct Mem *result = sqlValueNew(sql_get());
	sqlVdbeMemSetZeroBlob(result, n);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * The replace() function.  Three arguments are all strings: call
 * them A, B, and C. The result is also a string which is derived
 * from A by replacing every occurrence of B with C.  The match
 * must be exact.  Collating sequences are not used.
 */
static int
sql_func_replace(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 3);

	const unsigned char *zStr;	/* The input string A */
	const unsigned char *zPattern;	/* The pattern string B */
	const unsigned char *zRep;	/* The replacement string C */
	unsigned char *zOut;	/* The output */
	int nStr;		/* Size of zStr */
	int nPattern;		/* Size of zPattern */
	int nRep;		/* Size of zRep */
	i64 nOut;		/* Maximum size of zOut */
	int loopLimit;		/* Last zStr[] that might match zPattern[] */
	int i, j;		/* Loop counters */

	zStr = sql_value_text(&argv[0]);
	if (zStr == NULL) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	nStr = sql_value_bytes(&argv[0]);
	assert(zStr == sql_value_text(&argv[0]));	/* No encoding change */
	zPattern = sql_value_text(&argv[1]);
	if (zPattern == 0) {
		assert(sql_value_is_null(&argv[1]) || sql_get()->mallocFailed);
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}

	struct Mem *result = sqlValueNew(sql_get());
	nPattern = sql_value_bytes(&argv[1]);
	if (nPattern == 0) {
		assert(! sql_value_is_null(&argv[1]));
		sqlVdbeMemCopy(result, &argv[0]);
		port_vdbemem_create(ret, result, 1);
		return 0;
	}
	assert(zPattern == sql_value_text(&argv[1]));	/* No encoding change */

	zRep = sql_value_text(&argv[2]);
	if (zRep == 0) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}

	nRep = sql_value_bytes(&argv[2]);
	assert(zRep == sql_value_text(&argv[2]));
	nOut = nStr + 1;
	assert(nOut < SQL_MAX_LENGTH);
	zOut = contextMalloc(func->context, (i64) nOut);
	if (zOut == 0) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	loopLimit = nStr - nPattern;
	for (i = j = 0; i <= loopLimit; i++) {
		if (zStr[i] != zPattern[0]
		    || memcmp(&zStr[i], zPattern, nPattern)) {
			zOut[j++] = zStr[i];
		} else {
			u8 *zOld;
			struct sql *db = sql_get();
			nOut += nRep - nPattern;
			testcase(nOut - 1 == db->aLimit[SQL_LIMIT_LENGTH]);
			testcase(nOut - 2 == db->aLimit[SQL_LIMIT_LENGTH]);
			if (nOut - 1 > db->aLimit[SQL_LIMIT_LENGTH]) {
				diag_set(ClientError, ER_SQL_EXECUTE, "string "\
					 "or binary string is too big");
				sql_free(zOut);
				return -1;
			}
			zOld = zOut;
			zOut = sql_realloc64(zOut, (int)nOut);
			if (zOut == 0) {
				sql_free(zOld);
				return -1;
			}
			memcpy(&zOut[j], zRep, nRep);
			j += nRep;
			i += nPattern - 1;
		}
	}
	assert(j + nStr - i + 1 == nOut);
	memcpy(&zOut[j], &zStr[i], nStr - i);
	j += nStr - i;
	assert(j <= nOut);
	zOut[j] = 0;

	if (sqlVdbeMemSetStr(result, (char *)zOut, j, 1, sql_free) != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/**
 * Remove characters included in @a trim_set from @a input_str
 * until encounter a character that doesn't belong to @a trim_set.
 * Remove from the side specified by @a flags.
 * @param result Mem that contains the result.
 * @param flags Trim specification: left, right or both.
 * @param trim_set The set of characters for trimming.
 * @param char_len Lengths of each UTF-8 character in @a trim_set.
 * @param char_cnt A number of UTF-8 characters in @a trim_set.
 * @param input_str Input string for trimming.
 * @param input_str_sz Input string size in bytes.
 */
static int
trim_procedure(struct Mem *result, enum trim_side_mask flags,
	       const unsigned char *trim_set, const uint8_t *char_len,
	       int char_cnt, const unsigned char *input_str, int input_str_sz)
{
	if (char_cnt == 0)
		goto finish;
	int i, len;
	const unsigned char *z;
	if ((flags & TRIM_LEADING) != 0) {
		while (input_str_sz > 0) {
			z = trim_set;
			for (i = 0; i < char_cnt; ++i, z += len) {
				len = char_len[i];
				if (len <= input_str_sz
				    && memcmp(input_str, z, len) == 0)
					break;
			}
			if (i >= char_cnt)
				break;
			input_str += len;
			input_str_sz -= len;
		}
	}
	if ((flags & TRIM_TRAILING) != 0) {
		while (input_str_sz > 0) {
			z = trim_set;
			for (i = 0; i < char_cnt; ++i, z += len) {
				len = char_len[i];
				if (len <= input_str_sz
				    && memcmp(&input_str[input_str_sz - len],
					      z, len) == 0)
					break;
			}
			if (i >= char_cnt)
				break;
			input_str_sz -= len;
		}
	}
finish:
	if (sqlVdbeMemSetStr(result, (char *)input_str, input_str_sz, 1,
			     SQL_TRANSIENT) != 0)
		return -1;
	return 0;
}

/**
 * Prepare arguments for trimming procedure. Allocate memory for
 * @a char_len (array of lengths each character in @a trim_set)
 * and fill it.
 *
 * @param context SQL context.
 * @param trim_set The set of characters for trimming.
 * @param[out] char_len Lengths of each character in @ trim_set.
 * @retval >=0 A number of UTF-8 characters in @a trim_set.
 * @retval -1 Memory allocation error.
 */
static int
trim_prepare_char_len(struct sql_context *context,
		      const unsigned char *trim_set, int trim_set_sz,
		      uint8_t **char_len)
{
	/*
	 * Count the number of UTF-8 characters passing through
	 * the entire char set, but not up to the '\0' or X'00'
	 * character. This allows to handle trimming set
	 * containing such characters.
	 */
	int char_cnt = sql_utf8_char_count(trim_set, trim_set_sz);
	if (char_cnt == 0) {
		*char_len = NULL;
		return 0;
	}

	if ((*char_len = (uint8_t *)contextMalloc(context, char_cnt)) == NULL)
		return -1;

	int i = 0, j = 0;
	while(j < char_cnt) {
		int old_i = i;
		SQL_UTF8_FWD_1(trim_set, i, trim_set_sz);
		(*char_len)[j++] = i - old_i;
	}

	return char_cnt;
}

/**
 * Normalize args from @a argv input array when it has one arg
 * only.
 *
 * Case: TRIM(<str>)
 * Call trimming procedure with TRIM_BOTH as the flags and " " as
 * the trimming set.
 */
static int
trim_func_one_arg(sql_value *arg, struct Mem *result)
{
	/* In case of VARBINARY type default trim octet is X'00'. */
	const unsigned char *default_trim;
	enum mp_type val_type = sql_value_type(arg);
	if (val_type == MP_NIL) {
		sqlVdbeMemSetNull(result);
		return 0;
	}
	if (mp_type_is_bloblike(val_type))
		default_trim = (const unsigned char *) "\0";
	else
		default_trim = (const unsigned char *) " ";
	int input_str_sz = sql_value_bytes(arg);
	const unsigned char *input_str = sql_value_text(arg);
	uint8_t trim_char_len[1] = { 1 };
	return trim_procedure(result, TRIM_BOTH, default_trim, trim_char_len, 1,
			      input_str, input_str_sz);
}

/**
 * Normalize args from @a argv input array when it has two args.
 *
 * Case: TRIM(<character_set> FROM <str>)
 * If user has specified <character_set> only, call trimming
 * procedure with TRIM_BOTH as the flags and that trimming set.
 *
 * Case: TRIM(LEADING/TRAILING/BOTH FROM <str>)
 * If user has specified side keyword only, then call trimming
 * procedure with the specified side and " " as the trimming set.
 */
static int
trim_func_two_args(struct sql_context *context, sql_value *arg1,
		   sql_value *arg2, struct Mem *result)
{
	const unsigned char *input_str, *trim_set;
	if ((input_str = sql_value_text(arg2)) == NULL) {
		sqlVdbeMemSetNull(result);
		return 0;
	}

	int input_str_sz = sql_value_bytes(arg2);
	if (sql_value_type(arg1) == MP_INT || sql_value_type(arg1) == MP_UINT) {
		uint8_t len_one = 1;
		return trim_procedure(result, sql_value_int(arg1),
				      (const unsigned char *) " ", &len_one, 1,
				      input_str, input_str_sz);
	} else if ((trim_set = sql_value_text(arg1)) != NULL) {
		int trim_set_sz = sql_value_bytes(arg1);
		uint8_t *char_len;
		int char_cnt = trim_prepare_char_len(context, trim_set,
						     trim_set_sz, &char_len);
		if (char_cnt == -1)
			return -1;
		int rc = trim_procedure(result, TRIM_BOTH, trim_set, char_len,
					char_cnt, input_str, input_str_sz);
		sql_free(char_len);
		return rc;
	}
	sqlVdbeMemSetNull(result);
	return 0;
}

/**
 * Normalize args from @a argv input array when it has three args.
 *
 * Case: TRIM(LEADING/TRAILING/BOTH <character_set> FROM <str>)
 * If user has specified side keyword and <character_set>, then
 * call trimming procedure with that args.
 */
static int
trim_func_three_args(struct sql_context *context, sql_value *arg1,
		     sql_value *arg2, sql_value *arg3, struct Mem *result)
{
	assert(sql_value_type(arg1) == MP_INT || sql_value_type(arg1) == MP_UINT);
	const unsigned char *input_str, *trim_set;
	if ((input_str = sql_value_text(arg3)) == NULL ||
	    (trim_set = sql_value_text(arg2)) == NULL) {
		sqlVdbeMemSetNull(result);
		return 0;
	}

	int trim_set_sz = sql_value_bytes(arg2);
	int input_str_sz = sql_value_bytes(arg3);
	uint8_t *char_len;
	int char_cnt = trim_prepare_char_len(context, trim_set, trim_set_sz,
					     &char_len);
	if (char_cnt == -1)
		return -1;
	int rc = trim_procedure(result, sql_value_int(arg1), trim_set, char_len,
				char_cnt, input_str, input_str_sz);
	sql_free(char_len);
	return rc;
}

/**
 * Normalize args from @a argv input array when it has one,
 * two or three args.
 *
 * This is a dispatcher function that calls corresponding
 * implementation depending on the number of arguments.
*/
static int
sql_func_trim(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	struct Mem *result = sqlValueNew(sql_get());
	int rc = 0;
	switch (argc) {
	case 1:
		rc = trim_func_one_arg(&argv[0], result);
		break;
	case 2:
		rc = trim_func_two_args(func->context, &argv[0], &argv[1],
					result);
		break;
	case 3:
		rc = trim_func_three_args(func->context, &argv[0], &argv[1],
					  &argv[2], result);
		break;
	default:
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT, "TRIM",
			 "1 or 2 or 3", argc);
		return -1;
	}
	if (rc != 0)
		return -1;
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * Compute the soundex encoding of a word.
 *
 * IMP: R-59782-00072 The soundex(X) function returns a string that is the
 * soundex encoding of the string X.
 */
static int
sql_func_soundex(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	char zResult[8];
	const u8 *zIn;
	int i, j;
	static const unsigned char iCode[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
		1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
		0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
		1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
	};
	enum mp_type mp_type = sql_value_type(&argv[0]);
	if (mp_type_is_bloblike(mp_type)) {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 sql_value_to_diag_str(&argv[0]), "text");
		return -1;
	}
	zIn = (u8 *) sql_value_text(&argv[0]);
	if (zIn == 0)
		zIn = (u8 *) "";
	for (i = 0; zIn[i] && !sqlIsalpha(zIn[i]); i++) {
	}
	struct Mem *result = sqlValueNew(sql_get());
	if (zIn[i]) {
		u8 prevcode = iCode[zIn[i] & 0x7f];
		zResult[0] = sqlToupper(zIn[i]);
		for (j = 1; j < 4 && zIn[i]; i++) {
			int code = iCode[zIn[i] & 0x7f];
			if (code > 0) {
				if (code != prevcode) {
					prevcode = code;
					zResult[j++] = code + '0';
				}
			} else {
				prevcode = 0;
			}
		}
		while (j < 4) {
			zResult[j++] = '0';
		}
		zResult[j] = 0;
		if (sqlVdbeMemSetStr(result, zResult, 4, 1, SQL_TRANSIENT) != 0)
			return -1;
	} else {
		/* IMP: R-64894-50321 The string "?000" is returned if the argument
		 * is NULL or contains no ASCII alphabetic characters.
		 */
		if (sqlVdbeMemSetStr(result, "?000", 4, 1, SQL_STATIC) != 0)
			return -1;
	}
	port_vdbemem_create(ret, result, 1);
	return 0;
}

/*
 * An instance of the following structure holds the context of a
 * sum() or avg() aggregate computation.
 */
typedef struct SumCtx SumCtx;
struct SumCtx {
	double rSum;		/* Floating point sum */
	int64_t iSum;		/* Integer sum */
	/** True if iSum < 0. */
	bool is_neg;
	i64 cnt;		/* Number of elements summed */
	u8 overflow;		/* True if integer overflow seen */
	u8 approx;		/* True if non-integer value was input to the sum */
};

/*
 * Routines used to compute the sum, average, and total.
 *
 * The SUM() function follows the (broken) SQL standard which means
 * that it returns NULL if it sums over no inputs.  TOTAL returns
 * 0.0 in that case.  In addition, TOTAL always returns a float where
 * SUM might return an integer if it never encounters a floating point
 * value.  TOTAL never fails, but SUM might through an exception if
 * it overflows an integer.
 */
static int
sql_func_sum_step(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	struct SumCtx *p = sql_aggregate_context(func->context, sizeof(*p));
	int type = sql_value_type(&argv[0]);
	if (type == MP_NIL || p == NULL) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	if (type != MP_DOUBLE && type != MP_INT && type != MP_UINT) {
		if (mem_apply_numeric_type(&argv[0]) != 0) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 sql_value_to_diag_str(&argv[0]), "number");
			return -1;
		}
		type = sql_value_type(&argv[0]);
	}
	p->cnt++;
	if (type == MP_INT || type == MP_UINT) {
		int64_t v = sql_value_int64(&argv[0]);
		if (type == MP_INT)
			p->rSum += v;
		else
			p->rSum += (uint64_t) v;
		if ((p->approx | p->overflow) == 0 &&
		    sql_add_int(p->iSum, p->is_neg, v, type == MP_INT, &p->iSum,
				&p->is_neg) != 0) {
			p->overflow = 1;
		}
	} else {
		p->rSum += sql_value_double(&argv[0]);
		p->approx = 1;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

static void
sumFinalize(sql_context * context)
{
	SumCtx *p;
	p = sql_aggregate_context(context, 0);
	if (p && p->cnt > 0) {
		if (p->overflow) {
			diag_set(ClientError, ER_SQL_EXECUTE, "integer "\
				 "overflow");
			context->is_aborted = true;
		} else if (p->approx) {
			sql_result_double(context, p->rSum);
		} else {
			mem_set_int(context->pOut, p->iSum, p->is_neg);
		}
	}
}

static void
avgFinalize(sql_context * context)
{
	SumCtx *p;
	p = sql_aggregate_context(context, 0);
	if (p && p->cnt > 0) {
		sql_result_double(context, p->rSum / (double)p->cnt);
	}
}

static void
totalFinalize(sql_context * context)
{
	SumCtx *p;
	p = sql_aggregate_context(context, 0);
	sql_result_double(context, p ? p->rSum : (double)0);
}

/*
 * The following structure keeps track of state information for the
 * count() aggregate function.
 */
typedef struct CountCtx CountCtx;
struct CountCtx {
	i64 n;
};

/*
 * Routines to implement the count() aggregate function.
 */
static int
sql_func_count_step(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	CountCtx *p;
	if (argc != 0 && argc != 1) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT,
			 "COUNT", "0 or 1", argc);
		return -1;
	}
	p = sql_aggregate_context(func->context, sizeof(*p));
	if ((argc == 0 || ! sql_value_is_null(&argv[0])) && p) {
		p->n++;
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

static void
countFinalize(sql_context * context)
{
	CountCtx *p;
	p = sql_aggregate_context(context, 0);
	sql_result_uint(context, p ? p->n : 0);
}

/*
 * Routines to implement min() and max() aggregate functions.
 */
static int
sql_func_min_max_step(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);
	assert(argc == 1);

	Mem *pArg = &argv[0];
	Mem *pBest;

	pBest = (Mem *) sql_aggregate_context(func->context, sizeof(*pBest));
	if (!pBest) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}

	if (sql_value_is_null(&argv[0])) {
		if (pBest->flags)
			sqlSkipAccumulatorLoad(func->context);
	} else if (pBest->flags) {
		int cmp;
		struct coll *pColl = sqlGetFuncCollSeq(func->context);
		/*
		 * This step function is used for both the min()
		 * and max() aggregates, the only difference
		 * between the two being that the sense of the
		 * comparison is inverted.
		 */
		bool is_max = (func->flags & SQL_FUNC_MAX) != 0;
		cmp = sqlMemCompare(pBest, pArg, pColl);
		if ((is_max && cmp < 0) || (!is_max && cmp > 0)) {
			sqlVdbeMemCopy(pBest, pArg);
		} else {
			sqlSkipAccumulatorLoad(func->context);
		}
	} else {
		pBest->db = sql_get();
		sqlVdbeMemCopy(pBest, pArg);
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

static void
minMaxFinalize(sql_context * context)
{
	sql_value *pRes;
	pRes = (sql_value *) sql_aggregate_context(context, 0);
	if (pRes) {
		if (pRes->flags) {
			sql_result_value(context, pRes);
		}
		sqlVdbeMemRelease(pRes);
	}
}

/*
 * group_concat(EXPR, ?SEPARATOR?)
 */
static int
sql_func_group_concat_step(struct func *base, struct port *args,
			   struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	struct func_sql_builtin *func = (struct func_sql_builtin *)base;
	uint32_t argc;
	struct Mem *argv = port_get_vdbemem(args, &argc);

	const char *zVal;
	StrAccum *pAccum;
	const char *zSep;
	int nVal, nSep;
	if (argc != 1 && argc != 2) {
		diag_set(ClientError, ER_FUNC_WRONG_ARG_COUNT,
			 "GROUP_CONCAT", "1 or 2", argc);
		return -1;
	}
	if (sql_value_is_null(&argv[0])) {
		port_vdbemem_create(ret, NULL, 0);
		return 0;
	}
	pAccum =
	    (StrAccum *) sql_aggregate_context(func->context, sizeof(*pAccum));

	if (pAccum) {
		sql *db = sql_get();
		int firstTerm = pAccum->mxAlloc == 0;
		pAccum->mxAlloc = db->aLimit[SQL_LIMIT_LENGTH];
		if (!firstTerm) {
			if (argc == 2) {
				zSep = (char *)sql_value_text(&argv[1]);
				nSep = sql_value_bytes(&argv[1]);
			} else {
				zSep = ",";
				nSep = 1;
			}
			if (zSep)
				sqlStrAccumAppend(pAccum, zSep, nSep);
		}
		zVal = (char *)sql_value_text(&argv[0]);
		nVal = sql_value_bytes(&argv[0]);
		if (zVal)
			sqlStrAccumAppend(pAccum, zVal, nVal);
	}
	port_vdbemem_create(ret, NULL, 0);
	return 0;
}

static void
groupConcatFinalize(sql_context * context)
{
	StrAccum *pAccum;
	pAccum = sql_aggregate_context(context, 0);
	if (pAccum) {
		if (pAccum->accError == STRACCUM_TOOBIG) {
			diag_set(ClientError, ER_SQL_EXECUTE, "string or binary"\
				 "string is too big");
			context->is_aborted = true;
		} else if (pAccum->accError == STRACCUM_NOMEM) {
			context->is_aborted = true;
		} else {
			sql_result_text(context,
					    sqlStrAccumFinish(pAccum),
					    pAccum->nChar, sql_free);
		}
	}
}

/**
 * Return the number of affected rows in the last SQL statement.
 */
static int
sql_func_row_count(struct func *base, struct port *args, struct port *ret)
{
	if (args->vtab != &port_vdbemem_vtab) {
		diag_set(ClientError, ER_UNSUPPORTED, "sql builtin function",
			 "Lua frontend");
		return -1;
	}
	(void)base;

	struct Mem *result = sqlValueNew(sql_get());
	mem_set_u64(result, sql_get()->nChange);
	port_vdbemem_create(ret, result, 1);
	return 0;
}

int
sql_is_like_func(struct Expr *expr)
{
	if (expr->op != TK_FUNCTION || !expr->x.pList ||
	    expr->x.pList->nExpr != 2)
		return 0;
	assert(!ExprHasProperty(expr, EP_xIsSelect));
	struct func *func = sql_func_by_signature(expr->u.zToken, 2);
	if (func == NULL || !sql_func_flag_is_set(func, SQL_FUNC_LIKE))
		return 0;
	return 1;
}

struct func *
sql_func_by_signature(const char *name, int argc)
{
	struct func *base = func_by_name(name, strlen(name));
	if (base == NULL || !base->def->exports.sql)
		return NULL;

	if (base->def->param_count != -1 && base->def->param_count != argc)
		return NULL;
	return base;
}

static int
sql_func_stub(struct func *base, struct port *args, struct port *ret)
{
	(void)args;
	(void)ret;
	diag_set(ClientError, ER_SQL_EXECUTE,
		 tt_sprintf("function '%s' is not implemented",
			    base->def->name));
	return -1;
}

static void
func_sql_builtin_destroy(struct func *func)
{
	assert(func->def->language == FUNC_LANGUAGE_SQL_BUILTIN);
	free(func);
}

/**
 * A sequence of SQL builtins definitions in
 * lexicographic order.
 */
static struct {
	/**
	 * Name is used to find corresponding entry in array
	 * sql_builtins applying binary search.
	 */
	const char *name;
	/** Members below are related to struct func_sql_builtin. */
	uint16_t flags;
	struct func_vtab vtab;
	void (*finalize)(sql_context *ctx);
	/** Members below are related to struct func_def. */
	bool is_deterministic;
	int param_count;
	enum field_type returns;
	enum func_aggregate aggregate;
	bool export_to_sql;
} sql_builtins[] = {
	{.name = "ABS",
	 .param_count = 1,
	 .returns = FIELD_TYPE_NUMBER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_abs, func_sql_builtin_destroy},
	}, {
	 .name = "AVG",
	 .param_count = 1,
	 .returns = FIELD_TYPE_NUMBER,
	 .is_deterministic = false,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .flags = 0,
	 .finalize = avgFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_sum_step, func_sql_builtin_destroy},
	}, {
	 .name = "CEIL",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "CEILING",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "CHAR",
	 .param_count = -1,
	 .returns = FIELD_TYPE_STRING,
	 .is_deterministic = true,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_char, func_sql_builtin_destroy},
	 }, {
	 .name = "CHARACTER_LENGTH",
	 .param_count = 1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_length, func_sql_builtin_destroy},
	}, {
	 .name = "CHAR_LENGTH",
	 .param_count = 1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_length, func_sql_builtin_destroy},
	}, {
	 .name = "COALESCE",
	 .param_count = -1,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_COALESCE,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	}, {
	 .name = "COUNT",
	 .param_count = -1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = countFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_count_step, func_sql_builtin_destroy},
	}, {
	 .name = "CURRENT_DATE",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "CURRENT_TIME",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "CURRENT_TIMESTAMP",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "DATE",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "DATETIME",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "EVERY",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "EXISTS",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "EXP",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "EXTRACT",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "FLOOR",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "GREATER",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "GREATEST",
	 .param_count = -1,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_NEEDCOLL | SQL_FUNC_MAX,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_least_greatest, func_sql_builtin_destroy},
	}, {
	 .name = "GROUP_CONCAT",
	 .param_count = -1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = groupConcatFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_group_concat_step, func_sql_builtin_destroy},
	}, {
	 .name = "HEX",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_hex, func_sql_builtin_destroy},
	}, {
	 .name = "IFNULL",
	 .param_count = 2,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_COALESCE,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	}, {
	 .name = "JULIANDAY",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "LEAST",
	 .param_count = -1,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_NEEDCOLL | SQL_FUNC_MIN,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_least_greatest, func_sql_builtin_destroy},
	}, {
	 .name = "LENGTH",
	 .param_count = 1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_LENGTH,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_length, func_sql_builtin_destroy},
	}, {
	 .name = "LESSER",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "LIKE",
	 .param_count = -1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_NEEDCOLL | SQL_FUNC_LIKE,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_like, func_sql_builtin_destroy},
	}, {
	 .name = "LIKELIHOOD",
	 .param_count = 2,
	 .returns = FIELD_TYPE_BOOLEAN,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_UNLIKELY,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	}, {
	 .name = "LIKELY",
	 .param_count = 1,
	 .returns = FIELD_TYPE_BOOLEAN,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_UNLIKELY,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	}, {
	 .name = "LN",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "LOWER",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_DERIVEDCOLL | SQL_FUNC_NEEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_lower, func_sql_builtin_destroy},
	}, {
	 .name = "MAX",
	 .param_count = 1,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = SQL_FUNC_NEEDCOLL | SQL_FUNC_MAX,
	 .finalize = minMaxFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_min_max_step, func_sql_builtin_destroy},
	}, {
	 .name = "MIN",
	 .param_count = 1,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = SQL_FUNC_NEEDCOLL | SQL_FUNC_MIN,
	 .finalize = minMaxFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_min_max_step, func_sql_builtin_destroy},
	}, {
	 .name = "MOD",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "NULLIF",
	 .param_count = 2,
	 .returns = FIELD_TYPE_SCALAR,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_NEEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_nullif, func_sql_builtin_destroy},
	}, {
	 .name = "OCTET_LENGTH",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "POSITION",
	 .param_count = 2,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_NEEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_position, func_sql_builtin_destroy},
	}, {
	 .name = "POWER",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "PRINTF",
	 .param_count = -1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_printf, func_sql_builtin_destroy},
	}, {
	 .name = "QUOTE",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_quote, func_sql_builtin_destroy},
	}, {
	 .name = "RANDOM",
	 .param_count = 0,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_random, func_sql_builtin_destroy},
	}, {
	 .name = "RANDOMBLOB",
	 .param_count = 1,
	 .returns = FIELD_TYPE_VARBINARY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_randomblob, func_sql_builtin_destroy},
	}, {
	 .name = "REPLACE",
	 .param_count = 3,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_DERIVEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_replace, func_sql_builtin_destroy},
	}, {
	 .name = "ROUND",
	 .param_count = -1,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_round, func_sql_builtin_destroy},
	}, {
	 .name = "ROW_COUNT",
	 .param_count = 0,
	 .returns = FIELD_TYPE_INTEGER,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_row_count, func_sql_builtin_destroy},
	}, {
	 .name = "SOME",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "SOUNDEX",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_soundex, func_sql_builtin_destroy},
	}, {
	 .name = "SQRT",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "STRFTIME",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "SUBSTR",
	 .param_count = -1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_DERIVEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_substr, func_sql_builtin_destroy},
	}, {
	 .name = "SUM",
	 .param_count = 1,
	 .returns = FIELD_TYPE_NUMBER,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = sumFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_sum_step, func_sql_builtin_destroy},
	}, {
	 .name = "TIME",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "TOTAL",
	 .param_count = 1,
	 .returns = FIELD_TYPE_NUMBER,
	 .aggregate = FUNC_AGGREGATE_GROUP,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = totalFinalize,
	 .export_to_sql = true,
	 .vtab = {sql_func_sum_step, func_sql_builtin_destroy},
	}, {
	 .name = "TRIM",
	 .param_count = -1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_DERIVEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_trim, func_sql_builtin_destroy},
	}, {
	 .name = "TYPEOF",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_TYPEOF,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_typeof, func_sql_builtin_destroy},
	}, {
	 .name = "UNICODE",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_unicode, func_sql_builtin_destroy},
	}, {
	 .name = "UNLIKELY",
	 .param_count = 1,
	 .returns = FIELD_TYPE_BOOLEAN,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_UNLIKELY,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	}, {
	 .name = "UPPER",
	 .param_count = 1,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = SQL_FUNC_DERIVEDCOLL | SQL_FUNC_NEEDCOLL,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_upper, func_sql_builtin_destroy},
	}, {
	 .name = "VERSION",
	 .param_count = 0,
	 .returns = FIELD_TYPE_STRING,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_version, func_sql_builtin_destroy},
	}, {
	 .name = "ZEROBLOB",
	 .param_count = 1,
	 .returns = FIELD_TYPE_VARBINARY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = true,
	 .flags = 0,
	 .finalize = NULL,
	 .export_to_sql = true,
	 .vtab = {sql_func_zeroblob, func_sql_builtin_destroy},
	}, {
	 .name = "_sql_stat_get",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "_sql_stat_init",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	}, {
	 .name = "_sql_stat_push",
	 .export_to_sql = false,
	 .vtab = {sql_func_stub, func_sql_builtin_destroy},
	 .param_count = -1,
	 .returns = FIELD_TYPE_ANY,
	 .aggregate = FUNC_AGGREGATE_NONE,
	 .is_deterministic = false,
	 .flags = 0,
	 .finalize = NULL,
	},
};

struct func *
func_sql_builtin_new(struct func_def *def)
{
	assert(def->language == FUNC_LANGUAGE_SQL_BUILTIN);
	/** Binary search for corresponding builtin entry. */
	int idx = -1, left = 0, right = nelem(sql_builtins) - 1;
	while (left <= right) {
		uint32_t mid = (left + right) / 2;
		int rc = strcmp(def->name, sql_builtins[mid].name);
		if (rc == 0) {
			idx = mid;
			break;
		}
		if (rc < 0)
			right = mid - 1;
		else
			left = mid + 1;
	}
	/*
	 * All SQL built-in(s) (stubs) are defined in a snapshot.
	 * Implementation-specific metadata is defined in
	 * sql_builtins list. When a definition were not found
	 * above, the function name is invalid, i.e. it is
	 * not built-in function.
	 */
	if (idx == -1) {
		diag_set(ClientError, ER_CREATE_FUNCTION, def->name,
			 "given built-in is not predefined");
		return NULL;
	}
	struct func_sql_builtin *func =
		(struct func_sql_builtin *) calloc(1, sizeof(*func));
	if (func == NULL) {
		diag_set(OutOfMemory, sizeof(*func), "malloc", "func");
		return NULL;
	}
	func->base.def = def;
	func->base.vtab = &sql_builtins[idx].vtab;
	func->flags = sql_builtins[idx].flags;
	func->finalize = sql_builtins[idx].finalize;
	def->param_count = sql_builtins[idx].param_count;
	def->is_deterministic = sql_builtins[idx].is_deterministic;
	def->returns = sql_builtins[idx].returns;
	def->aggregate = sql_builtins[idx].aggregate;
	def->exports.sql = sql_builtins[idx].export_to_sql;
	return &func->base;
}
