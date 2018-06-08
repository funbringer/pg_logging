/*
 * pg_logging.c
 *      PostgreSQL logging interface.
 *
 * Copyright (c) 2018, Postgres Professional
 */
#include "postgres.h"
#include "tsearch/ts_locale.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "access/htup_details.h"

#include "pg_logging.h"

PG_FUNCTION_INFO_V1( get_logged_data );
PG_FUNCTION_INFO_V1( flush_logged_data );
PG_FUNCTION_INFO_V1( test_ereport );
PG_FUNCTION_INFO_V1( errlevel_in );
PG_FUNCTION_INFO_V1( errlevel_out );
PG_FUNCTION_INFO_V1( errlevel_eq );

typedef struct {
	uint32		until;
	uint32		reading_pos;
	bool		wraparound;
} logged_data_ctx;

static char *
get_errlevel_name(int code)
{
	int i;
	for (i = 0; i <= 21 /* MAX_HASH_VALUE */; i++)
	{
		struct ErrorLevel	*el = &errlevel_wordlist[i];
		if (el->text != NULL && el->code == code)
			return el->text;
	}
	elog(ERROR, "Invalid error level name");
}

void
reset_counters_in_shmem(int buffer_size)
{
	LWLockAcquire(&hdr->hdr_lock, LW_EXCLUSIVE);
	hdr->endpos = 0;
	hdr->readpos = 0;
	hdr->wraparound = false;
	if (buffer_size > 0)
		hdr->buffer_size = buffer_size;

	LWLockRelease(&hdr->hdr_lock);
}

Datum
flush_logged_data(PG_FUNCTION_ARGS)
{
	reset_counters_in_shmem(0);
	PG_RETURN_VOID();
}

Datum
get_logged_data(PG_FUNCTION_ARGS)
{
	MemoryContext		old_mcxt;
	FuncCallContext	   *funccxt;
	logged_data_ctx	   *usercxt;

	bool	flush = PG_GETARG_BOOL(0);

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;

		funccxt = SRF_FIRSTCALL_INIT();

		old_mcxt = MemoryContextSwitchTo(funccxt->multi_call_memory_ctx);

		/*
		 * Reader will block only other readers if it's fast enough.
		 * Writer could take this lock if readpos wasn't changed.
		 */
		LWLockAcquire(&hdr->hdr_lock, LW_EXCLUSIVE);
		usercxt = (logged_data_ctx *) palloc(sizeof(logged_data_ctx));
		usercxt->until = hdr->endpos;
		usercxt->reading_pos = hdr->readpos;
		usercxt->wraparound = hdr->wraparound;
		hdr->wraparound = false;

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		funccxt->tuple_desc = BlessTupleDesc(tupdesc);
		funccxt->user_fctx = (void *) usercxt;

		MemoryContextSwitchTo(old_mcxt);
	}

	funccxt = SRF_PERCALL_SETUP();
	usercxt = (logged_data_ctx *) funccxt->user_fctx;

	pg_read_barrier();

	while ((!usercxt->wraparound && usercxt->reading_pos < usercxt->until) ||
			(usercxt->wraparound && usercxt->reading_pos > usercxt->until))
	{
		CollectedItem  *item;
		char		   *data;
		HeapTuple		htup;
		Datum			values[Natts_pg_logging_data];
		bool			isnull[Natts_pg_logging_data];

		if (usercxt->reading_pos + ITEM_HDR_LEN > hdr->buffer_size)
		{
			usercxt->reading_pos = 0;
			usercxt->wraparound = false;
			continue;
		}

		data = (char *) (hdr->data + usercxt->reading_pos);
		AssertPointerAlignment(data, 4);

		/*
		 * careful here, we point to the buffer first, then allocate a
		 * block using information from buffer
		 */
		item = (CollectedItem *) data;
		Assert(item->totallen < hdr->buffer_size);

		item = (CollectedItem *) palloc0(item->totallen);
		memcpy(item, data, offsetof(CollectedItem, data));
		data += ITEM_HDR_LEN;

		if (usercxt->reading_pos + item->totallen >= hdr->buffer_size)
		{
			/* two parts */
			int	taillen = hdr->buffer_size - usercxt->reading_pos - ITEM_HDR_LEN;
			memcpy(item->data, data, taillen);
			usercxt->wraparound = false;
			usercxt->reading_pos += item->totallen;
			usercxt->reading_pos = usercxt->reading_pos - hdr->buffer_size;
			memcpy(item->data + taillen, hdr->data, usercxt->reading_pos);
		}
		else
		{
			/* one part */
			memcpy(item->data, data, item->totallen - ITEM_HDR_LEN);
			usercxt->reading_pos += item->totallen;
		}

		MemSet(values, 0, sizeof(values));
		MemSet(isnull, 0, sizeof(isnull));

		values[Anum_pg_logging_logtime - 1] = TimestampTzGetDatum(item->logtime);

		if (item->session_start_time)
			values[Anum_pg_logging_start_time - 1] = TimestampTzGetDatum(item->session_start_time);
		else
			isnull[Anum_pg_logging_start_time - 1] = true;

		values[Anum_pg_logging_level - 1] = Int32GetDatum(item->elevel);
		values[Anum_pg_logging_errno - 1] = Int32GetDatum(item->saved_errno);
		values[Anum_pg_logging_errcode - 1] = Int32GetDatum(item->sqlerrcode);
		values[Anum_pg_logging_datid - 1] = Int32GetDatum(item->database_id);
		values[Anum_pg_logging_pid - 1] = Int32GetDatum(item->ppid);
		values[Anum_pg_logging_line_num - 1] = Int64GetDatum(item->log_line_number);
		values[Anum_pg_logging_internalpos - 1] = Int32GetDatum(item->internalpos);
		values[Anum_pg_logging_query_pos - 1] = Int32GetDatum(item->query_pos);

		if (TransactionIdIsValid(item->txid))
			values[Anum_pg_logging_txid - 1] = TransactionIdGetDatum(item->txid);
		else
			isnull[Anum_pg_logging_txid - 1] = true;

		if (OidIsValid(item->user_id))
			values[Anum_pg_logging_userid - 1] = ObjectIdGetDatum(item->user_id);
		else
			isnull[Anum_pg_logging_userid - 1] = true;

		data = item->data;
#define	EXTRACT_VAL_TO(attnum, len)								\
do {															\
	if (len) {													\
		text *ct = cstring_to_text_with_len(data, len);			\
		values[(attnum) - 1] = PointerGetDatum(ct);				\
		data += (len);											\
	}															\
	else isnull[(attnum) - 1] = true;							\
} while (0);

		/* ordering is important !! */
		EXTRACT_VAL_TO(Anum_pg_logging_message, item->message_len);
		EXTRACT_VAL_TO(Anum_pg_logging_detail, item->detail_len);
		EXTRACT_VAL_TO(Anum_pg_logging_detail_log, item->detail_log_len);
		EXTRACT_VAL_TO(Anum_pg_logging_hint, item->hint_len);
		EXTRACT_VAL_TO(Anum_pg_logging_context, item->context_len);
		EXTRACT_VAL_TO(Anum_pg_logging_domain, item->domain_len);
		EXTRACT_VAL_TO(Anum_pg_logging_context_domain, item->context_domain_len);
		EXTRACT_VAL_TO(Anum_pg_logging_internalquery, item->internalquery_len);
		EXTRACT_VAL_TO(Anum_pg_logging_appname, item->appname_len);
		EXTRACT_VAL_TO(Anum_pg_logging_remote_host, item->remote_host_len);
		EXTRACT_VAL_TO(Anum_pg_logging_command_tag, item->command_tag_len);
		EXTRACT_VAL_TO(Anum_pg_logging_vxid, item->vxid_len);
		EXTRACT_VAL_TO(Anum_pg_logging_query, item->query_len);

		/* form output tuple */
		htup = heap_form_tuple(funccxt->tuple_desc, values, isnull);
		pfree(item);

		SRF_RETURN_NEXT(funccxt, HeapTupleGetDatum(htup));
	}

	if (flush)
		hdr->readpos = usercxt->reading_pos;

	LWLockRelease(&hdr->hdr_lock);
	SRF_RETURN_DONE(funccxt);
}

Datum
test_ereport(PG_FUNCTION_ARGS)
{
	ereport(PG_GETARG_INT32(0),
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s", PG_GETARG_CSTRING(1)),
			 errdetail("%s", PG_GETARG_CSTRING(2)),
			 errhint("%s", PG_GETARG_CSTRING(3))));
	PG_RETURN_VOID();
}

Datum
errlevel_out(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(get_errlevel_name(PG_GETARG_INT32(0)));
}

Datum
errlevel_in(PG_FUNCTION_ARGS)
{
	char   *str = lowerstr(PG_GETARG_CSTRING(0));
	int		len = strlen(str);
	struct ErrorLevel *el;

	if (len == 0)
		elog(ERROR, "Empty status name");

	el = get_errlevel(str, len);
	if (!el)
		elog(ERROR, "Unknown level name: %s", str);

	PG_RETURN_INT32(el->code);
}
