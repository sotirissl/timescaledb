/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <utils/guc.h>
#include <miscadmin.h>

#include "guc.h"
#include "license_guc.h"
#include "config.h"
#include "hypertable_cache.h"
#ifdef USE_TELEMETRY
#include "telemetry/telemetry.h"
#endif

#ifdef USE_TELEMETRY
/* Define which level means on. We use this object to have at least one object
 * of type TelemetryLevel in the code, otherwise pgindent won't work for the
 * type */
static const TelemetryLevel on_level = TELEMETRY_NO_FUNCTIONS;

bool
ts_telemetry_on()
{
	return ts_guc_telemetry_level >= on_level;
}

bool
ts_function_telemetry_on()
{
	return ts_guc_telemetry_level > TELEMETRY_NO_FUNCTIONS;
}

static const struct config_enum_entry telemetry_level_options[] = {
	{ "off", TELEMETRY_OFF, false },
	{ "no_functions", TELEMETRY_NO_FUNCTIONS, false },
	{ "basic", TELEMETRY_BASIC, false },
	{ NULL, 0, false }
};
#endif

static const struct config_enum_entry remote_data_fetchers[] = {
	{ "auto", AutoFetcherType, false },
	{ "copy", CopyFetcherType, false },
	{ "cursor", CursorFetcherType, false },
	{ "prepared", PreparedStatementFetcherType, false },
	{ NULL, 0, false }
};

static const struct config_enum_entry hypertable_distributed_types[] = {
	{ "auto", HYPERTABLE_DIST_AUTO, false },
	{ "local", HYPERTABLE_DIST_LOCAL, false },
	{ "distributed", HYPERTABLE_DIST_DISTRIBUTED, false },
	{ NULL, 0, false }
};

static const struct config_enum_entry dist_copy_transfer_formats[] = {
	{ "auto", DCTF_Auto, false },
	{ "binary", DCTF_Binary, false },
	{ "text", DCTF_Text, false },
	{ NULL, 0, false }
};

bool ts_guc_enable_optimizations = true;
bool ts_guc_restoring = false;
bool ts_guc_enable_constraint_aware_append = true;
bool ts_guc_enable_ordered_append = true;
bool ts_guc_enable_chunk_append = true;
bool ts_guc_enable_parallel_chunk_append = true;
bool ts_guc_enable_runtime_exclusion = true;
bool ts_guc_enable_constraint_exclusion = true;
bool ts_guc_enable_qual_propagation = true;
bool ts_guc_enable_cagg_reorder_groupby = true;
bool ts_guc_enable_now_constify = true;
bool ts_guc_enable_osm_reads = true;
TSDLLEXPORT bool ts_guc_enable_dml_decompression = true;
TSDLLEXPORT bool ts_guc_enable_transparent_decompression = true;
TSDLLEXPORT bool ts_guc_enable_decompression_sorted_merge = true;
bool ts_guc_enable_per_data_node_queries = true;
bool ts_guc_enable_parameterized_data_node_scan = true;
bool ts_guc_enable_async_append = true;
TSDLLEXPORT bool ts_guc_enable_compression_indexscan = true;
TSDLLEXPORT bool ts_guc_enable_bulk_decompression = true;
TSDLLEXPORT bool ts_guc_enable_skip_scan = true;
/* default value of ts_guc_max_open_chunks_per_insert and ts_guc_max_cached_chunks_per_hypertable
 * will be set as their respective boot-value when the GUC mechanism starts up */
int ts_guc_max_open_chunks_per_insert;
int ts_guc_max_cached_chunks_per_hypertable;
#ifdef USE_TELEMETRY
TelemetryLevel ts_guc_telemetry_level = TELEMETRY_DEFAULT;
char *ts_telemetry_cloud = NULL;
#endif

TSDLLEXPORT char *ts_guc_license = TS_LICENSE_DEFAULT;
char *ts_last_tune_time = NULL;
char *ts_last_tune_version = NULL;
TSDLLEXPORT bool ts_guc_enable_2pc = true;
TSDLLEXPORT int ts_guc_max_insert_batch_size = 1000;
TSDLLEXPORT bool ts_guc_enable_connection_binary_data = true;
TSDLLEXPORT DistCopyTransferFormat ts_guc_dist_copy_transfer_format = DCTF_Auto;
TSDLLEXPORT bool ts_guc_enable_client_ddl_on_data_nodes = false;
TSDLLEXPORT char *ts_guc_ssl_dir = NULL;
TSDLLEXPORT char *ts_guc_passfile = NULL;
TSDLLEXPORT bool ts_guc_enable_remote_explain = false;
TSDLLEXPORT DataFetcherType ts_guc_remote_data_fetcher = AutoFetcherType;
TSDLLEXPORT HypertableDistType ts_guc_hypertable_distributed_default = HYPERTABLE_DIST_AUTO;
TSDLLEXPORT int ts_guc_hypertable_replication_factor_default = 1;

#ifdef TS_DEBUG
bool ts_shutdown_bgw = false;
char *ts_current_timestamp_mock = NULL;
#endif

/*
 * We have to understand if we have finished initializing the GUCs, so that we
 * know when it's OK to check their values for mutual consistency.
 */
static bool gucs_are_initialized = false;

/* Hook for plugins to allow additional SSL options */
set_ssl_options_hook_type ts_set_ssl_options_hook = NULL;

/* Assign the hook to the passed in function argument */
void
ts_assign_ssl_options_hook(void *fn)
{
	ts_set_ssl_options_hook = (set_ssl_options_hook_type) fn;
}

/*
 * Warn about the mismatched cache sizes that can lead to cache thrashing.
 */
static void
validate_chunk_cache_sizes(int hypertable_chunks, int insert_chunks)
{
	/*
	 * Note that this callback is also called when the individual GUCs are
	 * initialized, so we are going to see temporary mismatched values here.
	 * That's why we also have to check that the GUC initialization have
	 * finished.
	 */
	if (gucs_are_initialized && insert_chunks > hypertable_chunks)
	{
		ereport(WARNING,
				(errmsg("insert cache size is larger than hypertable chunk cache size"),
				 errdetail("insert cache size is %d, hypertable chunk cache size is %d",
						   insert_chunks,
						   hypertable_chunks),
				 errhint("This is a configuration problem. Either increase "
						 "timescaledb.max_cached_chunks_per_hypertable (preferred) or decrease "
						 "timescaledb.max_open_chunks_per_insert.")));
	}
}

static void
assign_max_cached_chunks_per_hypertable_hook(int newval, void *extra)
{
	/* invalidate the hypertable cache to reset */
	ts_hypertable_cache_invalidate_callback();

	validate_chunk_cache_sizes(newval, ts_guc_max_open_chunks_per_insert);
}

static void
assign_max_open_chunks_per_insert_hook(int newval, void *extra)
{
	validate_chunk_cache_sizes(ts_guc_max_cached_chunks_per_hypertable, newval);
}

void
_guc_init(void)
{
	/* Main database to connect to. */
	DefineCustomBoolVariable("timescaledb.enable_optimizations",
							 "Enable TimescaleDB query optimizations",
							 NULL,
							 &ts_guc_enable_optimizations,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.restoring",
							 "Install timescale in restoring mode",
							 "Used for running pg_restore",
							 &ts_guc_restoring,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_constraint_aware_append",
							 "Enable constraint-aware append scans",
							 "Enable constraint exclusion at execution time",
							 &ts_guc_enable_constraint_aware_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_ordered_append",
							 "Enable ordered append scans",
							 "Enable ordered append optimization for queries that are ordered by "
							 "the time dimension",
							 &ts_guc_enable_ordered_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_chunk_append",
							 "Enable chunk append node",
							 "Enable using chunk append node",
							 &ts_guc_enable_chunk_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_parallel_chunk_append",
							 "Enable parallel chunk append node",
							 "Enable using parallel aware chunk append node",
							 &ts_guc_enable_parallel_chunk_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_runtime_exclusion",
							 "Enable runtime chunk exclusion",
							 "Enable runtime chunk exclusion in ChunkAppend node",
							 &ts_guc_enable_runtime_exclusion,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_constraint_exclusion",
							 "Enable constraint exclusion",
							 "Enable planner constraint exclusion",
							 &ts_guc_enable_constraint_exclusion,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_qual_propagation",
							 "Enable qualifier propagation",
							 "Enable propagation of qualifiers in JOINs",
							 &ts_guc_enable_qual_propagation,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_dml_decompression",
							 "Enable DML decompression",
							 "Enable DML decompression when modifying compressed hypertable",
							 &ts_guc_enable_dml_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_transparent_decompression",
							 "Enable transparent decompression",
							 "Enable transparent decompression when querying hypertable",
							 &ts_guc_enable_transparent_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_skipscan",
							 "Enable SkipScan",
							 "Enable SkipScan for DISTINCT queries",
							 &ts_guc_enable_skip_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_decompression_sorted_merge",
							 "Enable compressed batches heap merge",
							 "Enable the merge of compressed batches to preserve the compression "
							 "order by",
							 &ts_guc_enable_decompression_sorted_merge,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_cagg_reorder_groupby",
							 "Enable group by reordering",
							 "Enable group by clause reordering for continuous aggregates",
							 &ts_guc_enable_cagg_reorder_groupby,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_now_constify",
							 "Enable now() constify",
							 "Enable constifying now() in query constraints",
							 &ts_guc_enable_now_constify,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_2pc",
							 "Enable two-phase commit",
							 "Enable two-phase commit on distributed hypertables",
							 &ts_guc_enable_2pc,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_per_data_node_queries",
							 "Enable the per data node query optimization for hypertables",
							 "Enable the optimization that combines different chunks belonging to "
							 "the same hypertable into a single query per data_node",
							 &ts_guc_enable_per_data_node_queries,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_parameterized_data_node_scan",
							 "Enable parameterized data node scans",
							 "Disable this as a workaround in case these plans are incorrectly "
							 "chosen by the query planner when they are suboptimal",
							 &ts_guc_enable_parameterized_data_node_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_tiered_reads",
							 "Enable tiered data reads",
							 "Enable reading of tiered data by including a foreign table "
							 "representing the data in the object storage into the query plan",
							 &ts_guc_enable_osm_reads,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("timescaledb.max_insert_batch_size",
							"The max number of tuples to batch before sending to a data node",
							"When acting as a access node, TimescaleDB splits batches of "
							"inserted tuples across multiple data nodes. It will batch up to the "
							"configured batch size tuples per data node before flushing. "
							"Setting this to 0 disables batching, reverting to tuple-by-tuple "
							"inserts",
							&ts_guc_max_insert_batch_size,
							1000,
							0,
							65536,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("timescaledb.enable_connection_binary_data",
							 "Enable binary format for connection",
							 "Enable binary format for data exchanged between nodes in the cluster",
							 &ts_guc_enable_connection_binary_data,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * The default is 'auto', so that the dist COPY could use text transfer
	 * format for text input. It has a passthrough optimization for this case,
	 * which greatly reduces the CPU usage. Ideally we would implement the same
	 * optimization for binary, but the Postgres COPY code doesn't provide
	 * enough APIs for that.
	 */
	DefineCustomEnumVariable("timescaledb.dist_copy_transfer_format",
							 "Data format used by distributed COPY to send data to data nodes",
							 "auto, binary or text",
							 (int *) &ts_guc_dist_copy_transfer_format,
							 DCTF_Auto,
							 dist_copy_transfer_formats,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_client_ddl_on_data_nodes",
							 "Enable DDL operations on data nodes by a client",
							 "Do not restrict execution of DDL operations only by access node",
							 &ts_guc_enable_client_ddl_on_data_nodes,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_async_append",
							 "Enable async query execution on data nodes",
							 "Enable optimization that runs remote queries asynchronously"
							 "across data nodes",
							 &ts_guc_enable_async_append,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_remote_explain",
							 "Show explain from remote nodes when using VERBOSE flag",
							 "Enable getting and showing EXPLAIN output from remote nodes",
							 &ts_guc_enable_remote_explain,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_compression_indexscan",
							 "Enable compression to take indexscan path",
							 "Enable indexscan during compression, if matching index is found",
							 &ts_guc_enable_compression_indexscan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("timescaledb.enable_bulk_decompression",
							 "Enable decompression of the entire compressed batches",
							 "Increases throughput of decompression, but might increase query "
							 "memory usage",
							 &ts_guc_enable_bulk_decompression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("timescaledb.remote_data_fetcher",
							 "Set remote data fetcher type",
							 "Pick data fetcher type based on type of queries you plan to run "
							 "(copy or cursor)",
							 (int *) &ts_guc_remote_data_fetcher,
							 AutoFetcherType,
							 remote_data_fetchers,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("timescaledb.ssl_dir",
							   "TimescaleDB user certificate directory",
							   "Determines a path which is used to search user certificates and "
							   "private keys",
							   &ts_guc_ssl_dir,
							   NULL,
							   PGC_SIGHUP,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("timescaledb.passfile",
							   "TimescaleDB password file path",
							   "Specifies the name of the file used to store passwords used for "
							   "data node connections",
							   &ts_guc_passfile,
							   NULL,
							   PGC_SIGHUP,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("timescaledb.max_open_chunks_per_insert",
							"Maximum open chunks per insert",
							"Maximum number of open chunk tables per insert",
							&ts_guc_max_open_chunks_per_insert,
							1024,
							0,
							PG_INT16_MAX,
							PGC_USERSET,
							0,
							NULL,
							assign_max_open_chunks_per_insert_hook,
							NULL);

	DefineCustomIntVariable("timescaledb.max_cached_chunks_per_hypertable",
							"Maximum cached chunks",
							"Maximum number of chunks stored in the cache",
							&ts_guc_max_cached_chunks_per_hypertable,
							1024,
							0,
							65536,
							PGC_USERSET,
							0,
							NULL,
							assign_max_cached_chunks_per_hypertable_hook,
							NULL);
#ifdef USE_TELEMETRY
	DefineCustomEnumVariable("timescaledb.telemetry_level",
							 "Telemetry settings level",
							 "Level used to determine which telemetry to send",
							 (int *) &ts_guc_telemetry_level,
							 TELEMETRY_DEFAULT,
							 telemetry_level_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
#endif

	DefineCustomStringVariable(/* name= */ "timescaledb.license",
							   /* short_desc= */ "TimescaleDB license type",
							   /* long_desc= */ "Determines which features are enabled",
							   /* valueAddr= */ &ts_guc_license,
							   /* bootValue= */ TS_LICENSE_DEFAULT,
							   /* context= */ PGC_SUSET,
							   /* flags= */ 0,
							   /* check_hook= */ ts_license_guc_check_hook,
							   /* assign_hook= */ ts_license_guc_assign_hook,
							   /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.last_tuned",
							   /* short_desc= */ "last tune run",
							   /* long_desc= */ "records last time timescaledb-tune ran",
							   /* valueAddr= */ &ts_last_tune_time,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.last_tuned_version",
							   /* short_desc= */ "version of timescaledb-tune",
							   /* long_desc= */ "version of timescaledb-tune used to tune",
							   /* valueAddr= */ &ts_last_tune_version,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);

#ifdef USE_TELEMETRY
	DefineCustomStringVariable(/* name= */ "timescaledb_telemetry.cloud",
							   /* short_desc= */ "cloud provider",
							   /* long_desc= */ "cloud provider used for this instance",
							   /* valueAddr= */ &ts_telemetry_cloud,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_SIGHUP,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);
#endif

#ifdef TS_DEBUG
	DefineCustomBoolVariable(/* name= */ "timescaledb.shutdown_bgw_scheduler",
							 /* short_desc= */ "immediately shutdown the bgw scheduler",
							 /* long_desc= */ "this is for debugging purposes",
							 /* valueAddr= */ &ts_shutdown_bgw,
							 /* bootValue= */ false,
							 /* context= */ PGC_SIGHUP,
							 /* flags= */ 0,
							 /* check_hook= */ NULL,
							 /* assign_hook= */ NULL,
							 /* show_hook= */ NULL);

	DefineCustomStringVariable(/* name= */ "timescaledb.current_timestamp_mock",
							   /* short_desc= */ "set the current timestamp",
							   /* long_desc= */ "this is for debugging purposes",
							   /* valueAddr= */ &ts_current_timestamp_mock,
							   /* bootValue= */ NULL,
							   /* context= */ PGC_USERSET,
							   /* flags= */ 0,
							   /* check_hook= */ NULL,
							   /* assign_hook= */ NULL,
							   /* show_hook= */ NULL);
#endif

	DefineCustomEnumVariable("timescaledb.hypertable_distributed_default",
							 "Set distributed hypertables default creation policy",
							 "Set default policy to create local or distributed hypertables "
							 "(auto, local or distributed)",
							 (int *) &ts_guc_hypertable_distributed_default,
							 HYPERTABLE_DIST_AUTO,
							 hypertable_distributed_types,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("timescaledb.hypertable_replication_factor_default",
							"Default replication factor value to use with a hypertables",
							"Global default value for replication factor to use with hypertables "
							"when the `replication_factor` argument is not provided",
							&ts_guc_hypertable_replication_factor_default,
							1,
							1,
							65536,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	gucs_are_initialized = true;

	validate_chunk_cache_sizes(ts_guc_max_cached_chunks_per_hypertable,
							   ts_guc_max_open_chunks_per_insert);
}

void
_guc_fini(void)
{
}
