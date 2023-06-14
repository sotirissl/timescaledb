/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <math.h>
#include <postgres.h>
#include <catalog/pg_operator.h>
#include <miscadmin.h>
#include <nodes/bitmapset.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/cost.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <parser/parsetree.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/typcache.h>

#include <planner.h>

#include "compat/compat.h"
#include "debug_assert.h"
#include "ts_catalog/hypertable_compression.h"
#include "import/planner.h"
#include "import/allpaths.h"
#include "compression/create.h"
#include "nodes/decompress_chunk/sorted_merge.h"
#include "nodes/decompress_chunk/decompress_chunk.h"
#include "nodes/decompress_chunk/planner.h"
#include "nodes/decompress_chunk/qual_pushdown.h"
#include "utils.h"

#define DECOMPRESS_CHUNK_CPU_TUPLE_COST 0.01

#define DECOMPRESS_CHUNK_BATCH_SIZE 1000

static CustomPathMethods decompress_chunk_path_methods = {
	.CustomName = "DecompressChunk",
	.PlanCustomPath = decompress_chunk_plan_create,
};

typedef struct SortInfo
{
	List *compressed_pathkeys;
	bool needs_sequence_num;
	bool can_pushdown_sort; /* sort can be pushed below DecompressChunk */
	bool reverse;
} SortInfo;

typedef enum MergeBatchResult
{
	MERGE_NOT_POSSIBLE,
	SCAN_FORWARD,
	SCAN_BACKWARD
} MergeBatchResult;

static RangeTblEntry *decompress_chunk_make_rte(Oid compressed_relid, LOCKMODE lockmode);
static void create_compressed_scan_paths(PlannerInfo *root, RelOptInfo *compressed_rel,
										 CompressionInfo *info, SortInfo *sort_info);

static DecompressChunkPath *decompress_chunk_path_create(PlannerInfo *root, CompressionInfo *info,
														 int parallel_workers,
														 Path *compressed_path);

static void decompress_chunk_add_plannerinfo(PlannerInfo *root, CompressionInfo *info, Chunk *chunk,
											 RelOptInfo *chunk_rel, bool needs_sequence_num);

static SortInfo build_sortinfo(Chunk *chunk, RelOptInfo *chunk_rel, CompressionInfo *info,
							   List *pathkeys);

static bool
is_compressed_column(CompressionInfo *info, AttrNumber attno)
{
	char *column_name = get_attname(info->compressed_rte->relid, attno, false);
	FormData_hypertable_compression *column_info =
		get_column_compressioninfo(info->hypertable_compression_info, column_name);

	return column_info->algo_id != 0;
}

/*
 * Like ts_make_pathkey_from_sortop but passes down the compressed relid so that existing
 * equivalence members that are marked as children are properly checked.
 */
static PathKey *
make_pathkey_from_compressed(PlannerInfo *root, Index compressed_relid, Expr *expr, Oid ordering_op,
							 bool nulls_first)
{
	Oid opfamily, opcintype, collation;
	int16 strategy;

	/* Find the operator in pg_amop --- failure shouldn't happen */
	if (!get_ordering_op_properties(ordering_op, &opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator", ordering_op);

	/* Because SortGroupClause doesn't carry collation, consult the expr */
	collation = exprCollation((Node *) expr);

	Assert(compressed_relid < (Index) root->simple_rel_array_size);
	return ts_make_pathkey_from_sortinfo(root,
										 expr,
										 NULL,
										 opfamily,
										 opcintype,
										 collation,
										 (strategy == BTGreaterStrategyNumber),
										 nulls_first,
										 0,
										 bms_make_singleton(compressed_relid),
										 true);
}

static void
prepend_ec_for_seqnum(PlannerInfo *root, CompressionInfo *info, SortInfo *sort_info, Var *var,
					  Oid sortop, bool nulls_first)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	Oid opfamily, opcintype, equality_op;
	int16 strategy;
	List *opfamilies;
	EquivalenceClass *newec = makeNode(EquivalenceClass);
	EquivalenceMember *em = makeNode(EquivalenceMember);

	/* Find the operator in pg_amop --- failure shouldn't happen */
	if (!get_ordering_op_properties(sortop, &opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator", sortop);

	/*
	 * EquivalenceClasses need to contain opfamily lists based on the family
	 * membership of mergejoinable equality operators, which could belong to
	 * more than one opfamily.  So we have to look up the opfamily's equality
	 * operator and get its membership.
	 */
	equality_op = get_opfamily_member(opfamily, opcintype, opcintype, BTEqualStrategyNumber);
	if (!OidIsValid(equality_op)) /* shouldn't happen */
		elog(ERROR,
			 "missing operator %d(%u,%u) in opfamily %u",
			 BTEqualStrategyNumber,
			 opcintype,
			 opcintype,
			 opfamily);
	opfamilies = get_mergejoin_opfamilies(equality_op);
	if (!opfamilies) /* certainly should find some */
		elog(ERROR, "could not find opfamilies for equality operator %u", equality_op);

	em->em_expr = (Expr *) var;
	em->em_relids = bms_make_singleton(info->compressed_rel->relid);
#if PG16_LT
	em->em_nullable_relids = NULL;
#endif
	em->em_is_const = false;
	em->em_is_child = false;
	em->em_datatype = INT4OID;

	newec->ec_opfamilies = list_copy(opfamilies);
	newec->ec_collation = 0;
	newec->ec_members = list_make1(em);
	newec->ec_sources = NIL;
	newec->ec_derives = NIL;
	newec->ec_relids = bms_make_singleton(info->compressed_rel->relid);
	newec->ec_has_const = false;
	newec->ec_has_volatile = false;
#if PG16_LT
	newec->ec_below_outer_join = false;
#endif
	newec->ec_broken = false;
	newec->ec_sortref = 0;
	newec->ec_min_security = UINT_MAX;
	newec->ec_max_security = 0;
	newec->ec_merged = NULL;

	/* Prepend the ec */
#if PG13_GE
	root->eq_classes = lappend(root->eq_classes, newec);
#else
	root->eq_classes = lcons(newec, root->eq_classes);
#endif

	MemoryContextSwitchTo(oldcontext);
}

static void
build_compressed_scan_pathkeys(SortInfo *sort_info, PlannerInfo *root, List *chunk_pathkeys,
							   CompressionInfo *info)
{
	Var *var;
	int varattno;
	List *compressed_pathkeys = NIL;
	PathKey *pk;

	/*
	 * all segmentby columns need to be prefix of pathkeys
	 * except those with equality constraint in baserestrictinfo
	 */
	if (info->num_segmentby_columns > 0)
	{
		Bitmapset *segmentby_columns = bms_copy(info->chunk_segmentby_ri);
		ListCell *lc;
		char *column_name;
		Oid sortop;
		Oid opfamily, opcintype;
		int16 strategy;

		for (lc = list_head(chunk_pathkeys);
			 lc != NULL && bms_num_members(segmentby_columns) < info->num_segmentby_columns;
			 lc = lnext_compat(chunk_pathkeys, lc))
		{
			PathKey *pk = lfirst(lc);
			var = (Var *) find_em_expr_for_rel(pk->pk_eclass, info->chunk_rel);

			if (var == NULL || !IsA(var, Var))
				/* this should not happen because we validated the pathkeys when creating the path
				 */
				elog(ERROR, "Invalid pathkey for compressed scan");

			/* not a segmentby column, rest of pathkeys should be handled by compress_orderby */
			if (!bms_is_member(var->varattno, info->chunk_segmentby_attnos))
				break;

			/* skip duplicate references */
			if (bms_is_member(var->varattno, segmentby_columns))
				continue;

			column_name = get_attname(info->chunk_rte->relid, var->varattno, false);
			segmentby_columns = bms_add_member(segmentby_columns, var->varattno);
			varattno = get_attnum(info->compressed_rte->relid, column_name);
			var = makeVar(info->compressed_rel->relid,
						  varattno,
						  var->vartype,
						  var->vartypmod,
						  var->varcollid,
						  0);

			sortop =
				get_opfamily_member(pk->pk_opfamily, var->vartype, var->vartype, pk->pk_strategy);
			if (!get_ordering_op_properties(sortop, &opfamily, &opcintype, &strategy))
			{
				if (type_is_enum(var->vartype))
				{
					sortop = get_opfamily_member(pk->pk_opfamily,
												 ANYENUMOID,
												 ANYENUMOID,
												 pk->pk_strategy);
				}
				else
				{
					elog(ERROR, "sort operator lookup failed for column \"%s\"", column_name);
				}
			}
			pk = make_pathkey_from_compressed(root,
											  info->compressed_rel->relid,
											  (Expr *) var,
											  sortop,
											  pk->pk_nulls_first);
			compressed_pathkeys = lappend(compressed_pathkeys, pk);
		}

		/* we validated this when we created the Path so only asserting here */
		Assert(bms_num_members(segmentby_columns) == info->num_segmentby_columns ||
			   list_length(compressed_pathkeys) == list_length(chunk_pathkeys));
	}

	/*
	 * If pathkeys contains non-segmentby columns the rest of the ordering
	 * requirements will be satisfied by ordering by sequence_num
	 */
	if (sort_info->needs_sequence_num)
	{
		bool nulls_first;
		Oid sortop;
		varattno =
			get_attnum(info->compressed_rte->relid, COMPRESSION_COLUMN_METADATA_SEQUENCE_NUM_NAME);
		var = makeVar(info->compressed_rel->relid, varattno, INT4OID, -1, InvalidOid, 0);

		if (sort_info->reverse)
		{
			sortop = get_commutator(Int4LessOperator);
			nulls_first = true;
		}
		else
		{
			sortop = Int4LessOperator;
			nulls_first = false;
		}

		/* Prepend the ec class for the sequence number. We are prepending
		 * the ec for efficiency in finding it. We are more likely to look for it
		 * then other ec classes */
		prepend_ec_for_seqnum(root, info, sort_info, var, sortop, nulls_first);

		pk = make_pathkey_from_compressed(root,
										  info->compressed_rel->relid,
										  (Expr *) var,
										  sortop,
										  nulls_first);

		compressed_pathkeys = lappend(compressed_pathkeys, pk);
	}
	sort_info->compressed_pathkeys = compressed_pathkeys;
}

static DecompressChunkPath *
copy_decompress_chunk_path(DecompressChunkPath *src)
{
	DecompressChunkPath *dst = palloc(sizeof(DecompressChunkPath));
	memcpy(dst, src, sizeof(DecompressChunkPath));

	return dst;
}

static CompressionInfo *
build_compressioninfo(PlannerInfo *root, Hypertable *ht, RelOptInfo *chunk_rel)
{
	ListCell *lc;
	AppendRelInfo *appinfo;
	CompressionInfo *info = palloc0(sizeof(CompressionInfo));

	info->chunk_rel = chunk_rel;
	info->chunk_rte = planner_rt_fetch(chunk_rel->relid, root);

	if (chunk_rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
	{
		appinfo = ts_get_appendrelinfo(root, chunk_rel->relid, false);
		info->ht_rte = planner_rt_fetch(appinfo->parent_relid, root);
	}
	else
	{
		Assert(chunk_rel->reloptkind == RELOPT_BASEREL);
		info->single_chunk = true;
		info->ht_rte = info->chunk_rte;
	}

	info->hypertable_id = ht->fd.id;

	info->hypertable_compression_info = ts_hypertable_compression_get(ht->fd.id);

	foreach (lc, info->hypertable_compression_info)
	{
		FormData_hypertable_compression *fd = lfirst(lc);
		if (fd->orderby_column_index > 0)
			info->num_orderby_columns++;
		if (fd->segmentby_column_index > 0)
		{
			AttrNumber chunk_attno = get_attnum(info->chunk_rte->relid, NameStr(fd->attname));
			info->chunk_segmentby_attnos =
				bms_add_member(info->chunk_segmentby_attnos, chunk_attno);
			info->num_segmentby_columns++;
		}
	}

	return info;
}

/*
 * calculate cost for DecompressChunkPath
 *
 * since we have to read whole batch before producing tuple
 * we put cost of 1 tuple of compressed_scan as startup cost
 */
static void
cost_decompress_chunk(Path *path, Path *compressed_path)
{
	/* startup_cost is cost before fetching first tuple */
	if (compressed_path->rows > 0)
		path->startup_cost = compressed_path->total_cost / compressed_path->rows;

	/* total_cost is cost for fetching all tuples */
	path->total_cost = compressed_path->total_cost + path->rows * DECOMPRESS_CHUNK_CPU_TUPLE_COST;
	path->rows = compressed_path->rows * DECOMPRESS_CHUNK_BATCH_SIZE;
}

/*
 * Calculate the costs for retrieving the decompressed in-order using
 * a binary heap.
 */
static void
cost_decompress_sorted_merge_append(PlannerInfo *root, DecompressChunkPath *dcpath,
									Path *child_path)
{
	Path sort_path; /* dummy for result of cost_sort */

	cost_sort(&sort_path,
			  root,
			  dcpath->compressed_pathkeys,
			  child_path->total_cost,
			  child_path->rows,
			  child_path->pathtarget->width,
			  0.0,
			  work_mem,
			  -1);

	/* startup_cost is cost before fetching first tuple */
	dcpath->cpath.path.startup_cost = sort_path.total_cost;

	/*
	 * The cost model for the normal chunk decompression produces the following total
	 * costs.
	 *
	 * Segments  Total costs
	 * 10         711.84
	 * 50        4060.91
	 * 100       8588.32
	 * 10000   119281.84
	 *
	 * The cost model of the regular decompression is roughly linear. Opening multiple batches in
	 * parallel needs resources and merging a high amount of batches becomes inefficient at some
	 * point. So, we use a quadratic cost model here to have higher costs than the normal
	 * decompression when more than ~100 batches are used. We set
	 * DECOMPRESS_CHUNK_HEAP_MERGE_CPU_TUPLE_COST to 0.8 to become most costly as soon as we have to
	 * process more than 120 batches.
	 *
	 * Note: To behave similarly to the cost model of the regular decompression path, this cost
	 * model does not consider the number of tuples.
	 */
	dcpath->cpath.path.total_cost =
		sort_path.total_cost + pow(sort_path.rows, 2) * DECOMPRESS_CHUNK_HEAP_MERGE_CPU_TUPLE_COST;

	dcpath->cpath.path.rows = sort_path.rows * DECOMPRESS_CHUNK_BATCH_SIZE;
}

/*
 * If the query 'order by' is prefix of the compression 'order by' (or equal), we can exploit
 * the ordering of the individual batches to create a total ordered result without resorting
 * the tuples. This speeds up all queries that use this ordering (because no sort node is
 * needed). In particular, queries that use a LIMIT are speed-up because only the top elements
 * of the affected batches needs to be decompressed. Without the optimization, the entire batches
 * are decompressed, sorted, and then the top elements are taken from the result.
 *
 * The idea is to do something similar to the MergeAppend node; a BinaryHeap is used
 * to merge the per segment by column sorted individual batches into a sorted result. So, we end
 * up which a data flow which looks as follows:
 *
 * DecompressChunk
 *   * Decompress Batch 1
 *   * Decompress Batch 2
 *   * Decompress Batch 3
 *       [....]
 *   * Decompress Batch N
 *
 * Using the presorted batches, we are able to open these batches dynamically. If we don't presort
 * them, we would have to open all batches at the same time. This would be similar to the work the
 * MergeAppend does, but this is not needed in our case and we could reduce the size of the heap and
 * the amount of parallel open batches.
 *
 * The algorithm works as follows:
 *
 *   (1) A sort node is placed below the decompress scan node and on top of the scan
 *       on the compressed chunk. This sort node uses the min/max values of the 'order by'
 *       columns from the metadata of the batch to get them into an order which can be
 *       used to merge them.
 *
 *       [Scan on compressed chunk] -> [Sort on min/max values] -> [Decompress and merge]
 *
 *       For example, the batches are sorted on the min value of the 'order by' metadata
 *       column: [0, 3] [0, 5] [3, 7] [6, 10]
 *
 *   (2) The decompress chunk node initializes a binary heap, opens the first batch and
 *       decompresses the first tuple from the batch. The tuple is put on the heap. In addition
 *       the opened batch is marked as the most recent batch (MRB).
 *
 *   (3) As soon as a tuple is requested from the heap, the following steps are performed:
 *       (3a) If the heap is empty, we are done.
 *       (3b) The top tuple from the heap is taken. It is checked if this tuple is from the
 *            MRB. If this is the case, the next batch is opened, the first tuple is decompressed,
 *            placed on the heap and this batch is marked as MRB. This is repeated until the
 *            top tuple from the heap is not from the MRB. After the top tuple is not from the
 *            MRB, all batches (and one ahead) which might contain the most recent tuple are
 *            opened and placed on the heap.
 *
 *            In the example above, the first three batches are opened because the first two
 *            batches might contain tuples with a value of 0.
 *       (3c) The top element from the heap is removed, the next tuple from the batch is
 *            decompressed (if present) and placed on the heap.
 *       (3d) The former top tuple of the heap is returned.
 *
 * This function checks if the compression 'order by' and the query 'order by' are
 * compatible and the optimization can be used.
 */
static MergeBatchResult
can_sorted_merge_append(PlannerInfo *root, CompressionInfo *info, Chunk *chunk)
{
	PathKey *pk;
	Var *var;
	Expr *expr;
	char *column_name;
	List *pathkeys = root->query_pathkeys;
	FormData_hypertable_compression *ci;
	MergeBatchResult merge_result = SCAN_FORWARD;

	/* Ensure that we have path keys and the chunk is ordered */
	if (pathkeys == NIL || ts_chunk_is_unordered(chunk))
		return MERGE_NOT_POSSIBLE;

	int nkeys = list_length(pathkeys);

	/*
	 * Loop over the pathkeys of the query. These pathkeys need to match the
	 * configured compress_orderby pathkeys.
	 */
	for (int pk_index = 0; pk_index < nkeys; pk_index++)
	{
		pk = list_nth(pathkeys, pk_index);
		expr = find_em_expr_for_rel(pk->pk_eclass, info->chunk_rel);

		if (expr == NULL || !IsA(expr, Var))
			return MERGE_NOT_POSSIBLE;

		var = castNode(Var, expr);

		if (var->varattno <= 0)
			return MERGE_NOT_POSSIBLE;

		column_name = get_attname(info->chunk_rte->relid, var->varattno, false);
		ci = get_column_compressioninfo(info->hypertable_compression_info, column_name);

		if (ci->orderby_column_index != pk_index + 1)
			return MERGE_NOT_POSSIBLE;

		/* Check order, if the order of the first column do not match, switch to backward scan */
		Assert(pk->pk_strategy == BTLessStrategyNumber ||
			   pk->pk_strategy == BTGreaterStrategyNumber);

		if (pk->pk_strategy != BTLessStrategyNumber)
		{
			/* Test that ORDER BY and NULLS first/last do match in forward scan */
			if (!ci->orderby_asc && ci->orderby_nullsfirst == pk->pk_nulls_first &&
				merge_result == SCAN_FORWARD)
				continue;
			/* Exact opposite in backward scan */
			else if (ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first &&
					 merge_result == SCAN_BACKWARD)
				continue;
			/* Switch scan direction on exact opposite order for first attribute */
			else if (ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first &&
					 pk_index == 0)
				merge_result = SCAN_BACKWARD;
			else
				return MERGE_NOT_POSSIBLE;
		}
		else
		{
			/* Test that ORDER BY and NULLS first/last do match in forward scan */
			if (ci->orderby_asc && ci->orderby_nullsfirst == pk->pk_nulls_first &&
				merge_result == SCAN_FORWARD)
				continue;
			/* Exact opposite in backward scan */
			else if (!ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first &&
					 merge_result == SCAN_BACKWARD)
				continue;
			/* Switch scan direction on exact opposite order for first attribute */
			else if (!ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first &&
					 pk_index == 0)
				merge_result = SCAN_BACKWARD;
			else
				return MERGE_NOT_POSSIBLE;
		}
	}

	return merge_result;
}

void
ts_decompress_chunk_generate_paths(PlannerInfo *root, RelOptInfo *chunk_rel, Hypertable *ht,
								   Chunk *chunk)
{
	RelOptInfo *compressed_rel;
	ListCell *lc;
	double new_row_estimate;
	Index ht_relid = 0;

	CompressionInfo *info = build_compressioninfo(root, ht, chunk_rel);

	/* double check we don't end up here on single chunk queries with ONLY */
	Assert(info->chunk_rel->reloptkind == RELOPT_OTHER_MEMBER_REL ||
		   (info->chunk_rel->reloptkind == RELOPT_BASEREL &&
			ts_rte_is_marked_for_expansion(info->chunk_rte)));

	SortInfo sort_info = build_sortinfo(chunk, chunk_rel, info, root->query_pathkeys);

	Assert(chunk->fd.compressed_chunk_id > 0);

	List *initial_pathlist = chunk_rel->pathlist;
	List *initial_partial_pathlist = chunk_rel->partial_pathlist;
	chunk_rel->pathlist = NIL;
	chunk_rel->partial_pathlist = NIL;

	/* add RangeTblEntry and RelOptInfo for compressed chunk */
	decompress_chunk_add_plannerinfo(root, info, chunk, chunk_rel, sort_info.needs_sequence_num);
	compressed_rel = info->compressed_rel;

	compressed_rel->consider_parallel = chunk_rel->consider_parallel;
	/* translate chunk_rel->baserestrictinfo */
	pushdown_quals(root,
				   chunk_rel,
				   compressed_rel,
				   info->hypertable_compression_info,
				   ts_chunk_is_partial(chunk));
	set_baserel_size_estimates(root, compressed_rel);
	new_row_estimate = compressed_rel->rows * DECOMPRESS_CHUNK_BATCH_SIZE;

	if (!info->single_chunk)
	{
		/* adjust the parent's estimate by the diff of new and old estimate */
		AppendRelInfo *chunk_info = ts_get_appendrelinfo(root, chunk_rel->relid, false);
		Assert(chunk_info->parent_reloid == ht->main_table_relid);
		ht_relid = chunk_info->parent_relid;
		RelOptInfo *hypertable_rel = root->simple_rel_array[ht_relid];
		hypertable_rel->rows += (new_row_estimate - chunk_rel->rows);
	}

	chunk_rel->rows = new_row_estimate;

	create_compressed_scan_paths(root, compressed_rel, info, &sort_info);

	/* compute parent relids of the chunk and use it to filter paths*/
	Relids parent_relids = NULL;
	if (!info->single_chunk)
		parent_relids = find_childrel_parents(root, chunk_rel);

	/* create non-parallel paths */
	foreach (lc, compressed_rel->pathlist)
	{
		Path *child_path = lfirst(lc);
		Path *path;

		/*
		 * We skip any BitmapScan parameterized paths here as supporting
		 * those would require fixing up the internal scan. Since we
		 * currently do not do this BitmapScans would be generated
		 * when we have a parameterized path on a compressed column
		 * that would have invalid references due to our
		 * EquivalenceClasses.
		 */
		if (IsA(child_path, BitmapHeapPath) && child_path->param_info)
			continue;

		/*
		 * Filter out all paths that try to JOIN the compressed chunk on the
		 * hypertable or the uncompressed chunk
		 * Ideally, we wouldn't create these paths in the first place.
		 * However, create_join_clause code is called by PG while generating paths for the
		 * compressed_rel via generate_implied_equalities_for_column.
		 * create_join_clause ends up creating rinfo's between compressed_rel and ht because
		 * PG does not know that compressed_rel is related to ht in anyway.
		 * The parent-child relationship between chunk_rel and ht is known
		 * to PG and so it does not try to create meaningless rinfos for that case.
		 */
		if (child_path->param_info != NULL)
		{
			if (bms_is_member(chunk_rel->relid, child_path->param_info->ppi_req_outer))
				continue;
			/* check if this is path made with references between
			 * compressed_rel + hypertable or a nesting subquery.
			 * The latter can happen in the case of UNION queries. see github 2917. This
			 * happens since PG is not aware that the nesting
			 * subquery that references the hypertable is a parent of compressed_rel as well.
			 */
			if (bms_overlap(parent_relids, child_path->param_info->ppi_req_outer))
				continue;

			ListCell *lc_ri;
			bool references_compressed = false;
			/*
			 * Check if this path is parameterized on a compressed
			 * column. Ideally those paths wouldn't be generated
			 * in the first place but since we create compressed
			 * EquivalenceMembers for all EquivalenceClasses these
			 * Paths can happen and will fail at execution since
			 * the left and right side of the expression are not
			 * compatible. Therefore we skip any Path that is
			 * parameterized on a compressed column here.
			 */
			foreach (lc_ri, child_path->param_info->ppi_clauses)
			{
				RestrictInfo *ri = lfirst_node(RestrictInfo, lc_ri);

				if (ri->right_em && IsA(ri->right_em->em_expr, Var) &&
					(Index) castNode(Var, ri->right_em->em_expr)->varno ==
						info->compressed_rel->relid)
				{
					Var *var = castNode(Var, ri->right_em->em_expr);
					if (is_compressed_column(info, var->varattno))
					{
						references_compressed = true;
						break;
					}
				}
				if (ri->left_em && IsA(ri->left_em->em_expr, Var) &&
					(Index) castNode(Var, ri->left_em->em_expr)->varno ==
						info->compressed_rel->relid)
				{
					Var *var = castNode(Var, ri->left_em->em_expr);
					if (is_compressed_column(info, var->varattno))
					{
						references_compressed = true;
						break;
					}
				}
			}
			if (references_compressed)
				continue;
		}

		path = (Path *) decompress_chunk_path_create(root, info, 0, child_path);

		/*
		 * Create a path for the sorted merge append optimization. This optimization performs a
		 * merge append of the involved batches by using a binary heap and preserving the
		 * compression order. This optimization is only taken into consideration if we can't push
		 * down the sort to the compressed chunk. If we can push down the sort, the batches can be
		 * directly consumed in this order and we don't need to use this optimization.
		 */
		DecompressChunkPath *batch_merge_path = NULL;

		if (ts_guc_enable_decompression_sorted_merge && !sort_info.can_pushdown_sort)
		{
			MergeBatchResult merge_result = can_sorted_merge_append(root, info, chunk);
			if (merge_result != MERGE_NOT_POSSIBLE)
			{
				batch_merge_path = copy_decompress_chunk_path((DecompressChunkPath *) path);

				batch_merge_path->reverse = (merge_result != SCAN_FORWARD);
				batch_merge_path->sorted_merge_append = true;

				/* The segment by optimization is only enabled if it can deliver the tuples in the
				 * same order as the query requested it. So, we can just copy the pathkeys of the
				 * query here.
				 */
				batch_merge_path->cpath.path.pathkeys = root->query_pathkeys;
				cost_decompress_sorted_merge_append(root, batch_merge_path, child_path);

				/* If the chunk is partially compressed, prepare the path only and add it later
				 * to a merge append path when we are able to generate the ordered result for the
				 * compressed and uncompressed part of the chunk.
				 */
				if (!ts_chunk_is_partial(chunk))
					add_path(chunk_rel, &batch_merge_path->cpath.path);
			}
		}

		/* If we can push down the sort below the DecompressChunk node, we set the pathkeys of
		 * the decompress node to the query pathkeys, while remembering the compressed_pathkeys
		 * corresponding to those query_pathkeys. We will determine whether to put a sort
		 * between the decompression node and the scan during plan creation */
		if (sort_info.can_pushdown_sort)
		{
			DecompressChunkPath *dcpath = copy_decompress_chunk_path((DecompressChunkPath *) path);
			dcpath->reverse = sort_info.reverse;
			dcpath->needs_sequence_num = sort_info.needs_sequence_num;
			dcpath->compressed_pathkeys = sort_info.compressed_pathkeys;
			dcpath->cpath.path.pathkeys = root->query_pathkeys;

			/*
			 * Add costing for a sort. The standard Postgres pattern is to add the cost during
			 * path creation, but not add the sort path itself, that's done during plan
			 * creation. Examples of this in: create_merge_append_path &
			 * create_merge_append_plan
			 */
			if (!pathkeys_contained_in(dcpath->compressed_pathkeys, child_path->pathkeys))
			{
				Path sort_path; /* dummy for result of cost_sort */

				cost_sort(&sort_path,
						  root,
						  dcpath->compressed_pathkeys,
						  child_path->total_cost,
						  child_path->rows,
						  child_path->pathtarget->width,
						  0.0,
						  work_mem,
						  -1);

				cost_decompress_chunk(&dcpath->cpath.path, &sort_path);
			}
			/*
			 * if chunk is partially compressed don't add this now but add an append path later
			 * combining the uncompressed and compressed parts of the chunk
			 */
			if (!ts_chunk_is_partial(chunk))
				add_path(chunk_rel, &dcpath->cpath.path);
			else
				path = &dcpath->cpath.path;
		}

		/*
		 * If this is a partially compressed chunk we have to combine data
		 * from compressed and uncompressed chunk.
		 */
		if (ts_chunk_is_partial(chunk))
		{
			Bitmapset *req_outer = PATH_REQ_OUTER(path);
			Path *uncompressed_path =
				get_cheapest_path_for_pathkeys(initial_pathlist, NIL, req_outer, TOTAL_COST, false);

			/*
			 * All children of an append path are required to have the same parameterization
			 * so we reparameterize here when we couldn't get a path with the parameterization
			 * we need. Reparameterization should always succeed here since uncompressed_path
			 * should always be a scan.
			 */
			if (!bms_equal(req_outer, PATH_REQ_OUTER(uncompressed_path)))
			{
				uncompressed_path = reparameterize_path(root, uncompressed_path, req_outer, 1.0);
				if (!uncompressed_path)
					continue;
			}

			/* If we were able to generate a batch merge path, create a merge append path
			 * that combines the result of the compressed and uncompressed part of the chunk. The
			 * uncompressed part will be sorted, the batch_merge_path is already properly sorted.
			 */
			if (batch_merge_path != NULL)
			{
				Path *merge_append_path =
					(Path *) create_merge_append_path_compat(root,
															 chunk_rel,
															 list_make2(batch_merge_path,
																		uncompressed_path),
															 root->query_pathkeys,
															 req_outer,
															 NIL);

				add_path(chunk_rel, merge_append_path);
			}

			/*
			 * Ideally, we would like for this to be a MergeAppend path.
			 * However, accumulate_append_subpath will cut out MergeAppend
			 * and directly add its children, so we have to combine the children
			 * into a MergeAppend node later, at the chunk append level.
			 */
			path = (Path *) create_append_path_compat(root,
													  chunk_rel,
													  list_make2(path, uncompressed_path),
													  NIL /* partial paths */,
													  NIL /* pathkeys */,
													  req_outer,
													  0,
													  false,
													  false,
													  path->rows + uncompressed_path->rows);
		}

		/* this has to go after the path is copied for the ordered path since path can get freed in
		 * add_path */
		add_path(chunk_rel, path);
	}

	/* the chunk_rel now owns the paths, remove them from the compressed_rel so they can't be freed
	 * if it's planned */
	compressed_rel->pathlist = NIL;
	/* create parallel paths */
	if (compressed_rel->consider_parallel)
	{
		foreach (lc, compressed_rel->partial_pathlist)
		{
			Path *child_path = lfirst(lc);
			Path *path;
			if (child_path->param_info != NULL &&
				(bms_is_member(chunk_rel->relid, child_path->param_info->ppi_req_outer) ||
				 (!info->single_chunk &&
				  bms_is_member(ht_relid, child_path->param_info->ppi_req_outer))))
				continue;

			/*
			 * If this is a partially compressed chunk we have to combine data
			 * from compressed and uncompressed chunk.
			 */
			path = (Path *)
				decompress_chunk_path_create(root, info, child_path->parallel_workers, child_path);

			if (ts_chunk_is_partial(chunk))
			{
				Bitmapset *req_outer = PATH_REQ_OUTER(path);
				Path *uncompressed_path = NULL;

				if (initial_partial_pathlist)
					uncompressed_path = get_cheapest_path_for_pathkeys(initial_partial_pathlist,
																	   NIL,
																	   req_outer,
																	   TOTAL_COST,
																	   true);

				if (!uncompressed_path)
					uncompressed_path = get_cheapest_path_for_pathkeys(initial_pathlist,
																	   NIL,
																	   req_outer,
																	   TOTAL_COST,
																	   true);

				/*
				 * All children of an append path are required to have the same parameterization
				 * so we reparameterize here when we couldn't get a path with the parameterization
				 * we need. Reparameterization should always succeed here since uncompressed_path
				 * should always be a scan.
				 */
				if (!bms_equal(req_outer, PATH_REQ_OUTER(uncompressed_path)))
				{
					uncompressed_path =
						reparameterize_path(root, uncompressed_path, req_outer, 1.0);
					if (!uncompressed_path)
						continue;
				}

				path = (Path *) create_append_path_compat(root,
														  chunk_rel,
														  NIL,
														  list_make2(path, uncompressed_path),
														  NIL /* pathkeys */,
														  req_outer,
														  Max(path->parallel_workers,
															  uncompressed_path->parallel_workers),
														  false,
														  NIL,
														  path->rows + uncompressed_path->rows);
			}
			add_partial_path(chunk_rel, path);
		}
		/* the chunk_rel now owns the paths, remove them from the compressed_rel so they can't be
		 * freed if it's planned */
		compressed_rel->partial_pathlist = NIL;
	}
	/* set reloptkind to RELOPT_DEADREL to prevent postgresql from replanning this relation */
	compressed_rel->reloptkind = RELOPT_DEADREL;

	/* We should never get in the situation with no viable paths. */
	Ensure(chunk_rel->pathlist, "could not create decompression path");
}

/*
 * Add a var for a particular column to the reltarget. attrs_used is a bitmap
 * of which columns we already have in reltarget. We do not add the columns that
 * are already there, and update it after adding something.
 */
static void
compressed_reltarget_add_var_for_column(RelOptInfo *compressed_rel, Oid compressed_relid,
										const char *column_name, Bitmapset **attrs_used)
{
	AttrNumber attnum = get_attnum(compressed_relid, column_name);
	Assert(attnum > 0);

	if (bms_is_member(attnum, *attrs_used))
	{
		/* This column is already in reltarget, we don't need duplicates. */
		return;
	}

	*attrs_used = bms_add_member(*attrs_used, attnum);

	Oid typid, collid;
	int32 typmod;
	get_atttypetypmodcoll(compressed_relid, attnum, &typid, &typmod, &collid);
	compressed_rel->reltarget->exprs =
		lappend(compressed_rel->reltarget->exprs,
				makeVar(compressed_rel->relid, attnum, typid, typmod, collid, 0));
}

/* copy over the vars from the chunk_rel->reltarget to the compressed_rel->reltarget
 * altering the fields that need it
 */
static void
compressed_rel_setup_reltarget(RelOptInfo *compressed_rel, CompressionInfo *info,
							   bool needs_sequence_num)
{
	bool have_whole_row_var = false;
	Bitmapset *attrs_used = NULL;

	Oid compressed_relid = info->compressed_rte->relid;

	/*
	 * We have to decompress three kinds of columns:
	 * 1) output targetlist of the relation,
	 * 2) columns required for the quals (WHERE),
	 * 3) columns required for joins.
	 */
	List *exprs = list_copy(info->chunk_rel->reltarget->exprs);
	ListCell *lc;
	foreach (lc, info->chunk_rel->baserestrictinfo)
	{
		exprs = lappend(exprs, ((RestrictInfo *) lfirst(lc))->clause);
	}
	foreach (lc, info->chunk_rel->joininfo)
	{
		exprs = lappend(exprs, ((RestrictInfo *) lfirst(lc))->clause);
	}

	/*
	 * Now go over the required expressions we prepared above, and add the
	 * required columns to the compressed reltarget.
	 */
	info->compressed_rel->reltarget->exprs = NIL;
	foreach (lc, exprs)
	{
		ListCell *lc2;
		List *chunk_vars = pull_var_clause(lfirst(lc), PVC_RECURSE_PLACEHOLDERS);
		foreach (lc2, chunk_vars)
		{
			FormData_hypertable_compression *column_info;
			char *column_name;
			Var *chunk_var = castNode(Var, lfirst(lc2));

			/* skip vars that aren't from the uncompressed chunk */
			if ((Index) chunk_var->varno != info->chunk_rel->relid)
			{
				continue;
			}

			/*
			 * If there's a system column or whole-row reference, add a whole-
			 * row reference, and we're done.
			 */
			if (chunk_var->varattno <= 0)
			{
				have_whole_row_var = true;
				continue;
			}

			column_name = get_attname(info->chunk_rte->relid, chunk_var->varattno, false);
			column_info =
				get_column_compressioninfo(info->hypertable_compression_info, column_name);

			Assert(column_info != NULL);

			compressed_reltarget_add_var_for_column(compressed_rel,
													compressed_relid,
													column_name,
													&attrs_used);

			/* if the column is an orderby, add it's metadata columns too */
			if (column_info->orderby_column_index > 0)
			{
				compressed_reltarget_add_var_for_column(compressed_rel,
														compressed_relid,
														compression_column_segment_min_name(
															column_info),
														&attrs_used);
				compressed_reltarget_add_var_for_column(compressed_rel,
														compressed_relid,
														compression_column_segment_max_name(
															column_info),
														&attrs_used);
			}
		}
	}

	/* always add the count column */
	compressed_reltarget_add_var_for_column(compressed_rel,
											compressed_relid,
											COMPRESSION_COLUMN_METADATA_COUNT_NAME,
											&attrs_used);

	/* add the segment order column if we may try to order by it */
	if (needs_sequence_num)
	{
		compressed_reltarget_add_var_for_column(compressed_rel,
												compressed_relid,
												COMPRESSION_COLUMN_METADATA_SEQUENCE_NUM_NAME,
												&attrs_used);
	}

	/*
	 * It doesn't make sense to request a whole-row var from the compressed
	 * chunk scan. If it is requested, just fetch the rest of columns. The
	 * whole-row var will be created by the projection of DecompressChunk node.
	 */
	if (have_whole_row_var)
	{
		for (int i = 1; i <= info->chunk_rel->max_attr; i++)
		{
			char *column_name = get_attname(info->chunk_rte->relid,
											i,
											/* missing_ok = */ false);
			AttrNumber chunk_attno = get_attnum(info->chunk_rte->relid, column_name);
			if (chunk_attno == InvalidAttrNumber)
			{
				/* Skip the dropped column. */
				continue;
			}

			AttrNumber compressed_attno = get_attnum(info->compressed_rte->relid, column_name);
			if (compressed_attno == InvalidAttrNumber)
			{
				elog(ERROR,
					 "column '%s' not found in the compressed chunk '%s'",
					 column_name,
					 get_rel_name(info->compressed_rte->relid));
			}

			if (bms_is_member(compressed_attno, attrs_used))
			{
				continue;
			}

			compressed_reltarget_add_var_for_column(compressed_rel,
													compressed_relid,
													column_name,
													&attrs_used);
		}
	}
}

static Bitmapset *
decompress_chunk_adjust_child_relids(Bitmapset *src, int chunk_relid, int compressed_chunk_relid)
{
	Bitmapset *result = NULL;
	if (src != NULL)
	{
		result = bms_copy(src);
		result = bms_del_member(result, chunk_relid);
		result = bms_add_member(result, compressed_chunk_relid);
	}
	return result;
}

/* based on adjust_appendrel_attrs_mutator handling of RestrictInfo */
static Node *
chunk_joininfo_mutator(Node *node, CompressionInfo *context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);
		Var *compress_var = copyObject(var);
		char *column_name;
		AttrNumber compressed_attno;
		FormData_hypertable_compression *compressioninfo;
		if ((Index) var->varno != context->chunk_rel->relid)
			return (Node *) var;

		column_name = get_attname(context->chunk_rte->relid, var->varattno, false);
		compressioninfo =
			get_column_compressioninfo(context->hypertable_compression_info, column_name);

		compressed_attno =
			get_attnum(context->compressed_rte->relid, compressioninfo->attname.data);
		compress_var->varno = context->compressed_rel->relid;
		compress_var->varattno = compressed_attno;

		return (Node *) compress_var;
	}
	else if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* Copy all flat-copiable fields */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* Recursively fix the clause itself */
		newinfo->clause = (Expr *) chunk_joininfo_mutator((Node *) oldinfo->clause, context);

		/* and the modified version, if an OR clause */
		newinfo->orclause = (Expr *) chunk_joininfo_mutator((Node *) oldinfo->orclause, context);

		/* adjust relid sets too */
		newinfo->clause_relids =
			decompress_chunk_adjust_child_relids(oldinfo->clause_relids,
												 context->chunk_rel->relid,
												 context->compressed_rel->relid);
		newinfo->required_relids =
			decompress_chunk_adjust_child_relids(oldinfo->required_relids,
												 context->chunk_rel->relid,
												 context->compressed_rel->relid);
		newinfo->outer_relids =
			decompress_chunk_adjust_child_relids(oldinfo->outer_relids,
												 context->chunk_rel->relid,
												 context->compressed_rel->relid);
#if PG16_LT
		newinfo->nullable_relids =
			decompress_chunk_adjust_child_relids(oldinfo->nullable_relids,
												 context->chunk_rel->relid,
												 context->compressed_rel->relid);
#endif
		newinfo->left_relids = decompress_chunk_adjust_child_relids(oldinfo->left_relids,
																	context->chunk_rel->relid,
																	context->compressed_rel->relid);
		newinfo->right_relids =
			decompress_chunk_adjust_child_relids(oldinfo->right_relids,
												 context->chunk_rel->relid,
												 context->compressed_rel->relid);

		newinfo->eval_cost.startup = -1;
		newinfo->norm_selec = -1;
		newinfo->outer_selec = -1;
		newinfo->left_em = NULL;
		newinfo->right_em = NULL;
		newinfo->scansel_cache = NIL;
		newinfo->left_bucketsize = -1;
		newinfo->right_bucketsize = -1;
		newinfo->left_mcvfreq = -1;
		newinfo->right_mcvfreq = -1;
		return (Node *) newinfo;
	}
	return expression_tree_mutator(node, chunk_joininfo_mutator, context);
}

/* translate chunk_rel->joininfo for compressed_rel
 * this is necessary for create_index_path which gets join clauses from
 * rel->joininfo and sets up parameterized paths (in rel->ppilist).
 * ppi_clauses is finally used to add any additional filters on the
 * indexpath when creating a plan in create_indexscan_plan.
 * Otherwise we miss additional filters that need to be applied after
 * the index plan is executed (github issue 1558)
 */
static void
compressed_rel_setup_joininfo(RelOptInfo *compressed_rel, CompressionInfo *info)
{
	RelOptInfo *chunk_rel = info->chunk_rel;
	ListCell *lc;
	List *compress_joininfo = NIL;
	foreach (lc, chunk_rel->joininfo)
	{
		RestrictInfo *compress_ri;
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		Node *result = chunk_joininfo_mutator((Node *) ri, info);
		Assert(IsA(result, RestrictInfo));
		compress_ri = (RestrictInfo *) result;
		compress_joininfo = lappend(compress_joininfo, compress_ri);
	}
	compressed_rel->joininfo = compress_joininfo;
}

typedef struct EMCreationContext
{
	List *compression_info;
	Oid uncompressed_relid;
	Oid compressed_relid;
	Index uncompressed_relid_idx;
	Index compressed_relid_idx;
	FormData_hypertable_compression *current_col_info;
} EMCreationContext;

/* get the compression info for an EquivalenceMember (EM) expr,
 * or return NULL if it's not one we can create an EM for
 * This is applicable to segment by and compressed columns
 * of the compressed table.
 */
static FormData_hypertable_compression *
get_compression_info_for_em(Node *node, EMCreationContext *context)
{
	/* based on adjust_appendrel_attrs_mutator */
	if (node == NULL)
		return NULL;

	Assert(!IsA(node, Query));

	if (IsA(node, Var))
	{
		FormData_hypertable_compression *col_info;
		char *column_name;
		Var *var = castNode(Var, node);
		if ((Index) var->varno != context->uncompressed_relid_idx)
			return NULL;

		/* we can't add an EM for system attributes or whole-row refs */
		if (var->varattno <= 0)
			return NULL;

		column_name = get_attname(context->uncompressed_relid, var->varattno, true);
		if (column_name == NULL)
			return NULL;

		col_info = get_column_compressioninfo(context->compression_info, column_name);

		if (col_info == NULL)
			return NULL;

		return col_info;
	}

	/*
	 * we currently ignore non-Var expressions; the EC we care about
	 * (the one relating Hypertable columns to chunk columns)
	 * should not have any
	 */
	return NULL;
}

static Node *
create_var_for_compressed_equivalence_member(Var *var, const EMCreationContext *context)
{
	/* based on adjust_appendrel_attrs_mutator */
	Assert(context->current_col_info != NULL);
	Assert((Index) var->varno == context->uncompressed_relid_idx);
	Assert(var->varattno > 0);

	var = (Var *) copyObject(var);

	if (var->varlevelsup == 0)
	{
		var->varno = context->compressed_relid_idx;
		var->varattno =
			get_attnum(context->compressed_relid, NameStr(context->current_col_info->attname));
#if PG13_GE
		var->varnosyn = var->varno;
		var->varattnosyn = var->varattno;
#else
		var->varnoold = var->varno;
		var->varoattno = var->varattno;
#endif

		return (Node *) var;
	}

	return NULL;
}

static bool
add_segmentby_to_equivalence_class(EquivalenceClass *cur_ec, CompressionInfo *info,
								   EMCreationContext *context)
{
	Relids uncompressed_chunk_relids = info->chunk_rel->relids;
	ListCell *lc;
	foreach (lc, cur_ec->ec_members)
	{
		Expr *child_expr;
		Relids new_relids;
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		Var *var;
		Assert(!bms_overlap(cur_em->em_relids, info->compressed_rel->relids));

		/* only consider EquivalenceMembers that are Vars, possibly with RelabelType, of the
		 * uncompressed chunk */
		var = (Var *) cur_em->em_expr;
		while (var && IsA(var, RelabelType))
			var = (Var *) ((RelabelType *) var)->arg;
		if (!(var && IsA(var, Var)))
			continue;

		if ((Index) var->varno != info->chunk_rel->relid)
			continue;

		/* given that the em is a var of the uncompressed chunk, the relid of the chunk should
		 * be set on the em */
		Assert(bms_overlap(cur_em->em_relids, uncompressed_chunk_relids));

		context->current_col_info = get_compression_info_for_em((Node *) var, context);
		if (context->current_col_info == NULL)
			continue;

		child_expr = (Expr *) create_var_for_compressed_equivalence_member(var, context);
		if (child_expr == NULL)
			continue;

		/*
		 * Transform em_relids to match.  Note we do *not* do
		 * pull_varnos(child_expr) here, as for example the
		 * transformation might have substituted a constant, but we
		 * don't want the child member to be marked as constant.
		 */
		new_relids = bms_difference(cur_em->em_relids, uncompressed_chunk_relids);
		new_relids = bms_add_members(new_relids, info->compressed_rel->relids);

#if PG16_LT
		/*
		 * And likewise for nullable_relids.  Note this code assumes
		 * parent and child relids are singletons.
		 */
		Relids new_nullable_relids = cur_em->em_nullable_relids;
		if (bms_overlap(new_nullable_relids, uncompressed_chunk_relids))
		{
			new_nullable_relids = bms_difference(new_nullable_relids, uncompressed_chunk_relids);
			new_nullable_relids =
				bms_add_members(new_nullable_relids, info->compressed_rel->relids);
		}
#endif

		/* copied from add_eq_member */
		{
			EquivalenceMember *em = makeNode(EquivalenceMember);

			em->em_expr = child_expr;
			em->em_relids = new_relids;
#if PG16_LT
			em->em_nullable_relids = new_nullable_relids;
#endif
			em->em_is_const = false;
			em->em_is_child = true;
			em->em_datatype = cur_em->em_datatype;
			cur_ec->ec_relids = bms_add_members(cur_ec->ec_relids, info->compressed_rel->relids);
#if PG13_GE
			cur_ec->ec_members = lappend(cur_ec->ec_members, em);
#else
			/* Prepend the ec member because it's likely to be accessed soon */
			cur_ec->ec_members = lcons(em, cur_ec->ec_members);
#endif

			return true;
		}
	}
	return false;
}

static void
compressed_rel_setup_equivalence_classes(PlannerInfo *root, CompressionInfo *info)
{
	EMCreationContext context = {
		.compression_info = info->hypertable_compression_info,

		.uncompressed_relid = info->chunk_rte->relid,
		.compressed_relid = info->compressed_rte->relid,

		.uncompressed_relid_idx = info->chunk_rel->relid,
		.compressed_relid_idx = info->compressed_rel->relid,
	};

	Assert(info->chunk_rte->relid != info->compressed_rel->relid);
	Assert(info->chunk_rel->relid != info->compressed_rel->relid);
	/* based on add_child_rel_equivalences */
#if PG13_GE
	int i = -1;
	bool ec_added = false;
	Assert(root->ec_merging_done);
	/* use chunk rel's eclass_indexes to avoid traversing all
	 * the root's eq_classes
	 */
	while ((i = bms_next_member(info->chunk_rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) list_nth(root->eq_classes, i);
#else
	ListCell *lc;
	foreach (lc, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);
#endif
		/*
		 * If this EC contains a volatile expression, then generating child
		 * EMs would be downright dangerous, so skip it.  We rely on a
		 * volatile EC having only one EM.
		 */
		if (cur_ec->ec_has_volatile)
			continue;

		/* if the compressed rel is already part of this EC,
		 * we don't need to re-add it
		 */
		if (bms_overlap(cur_ec->ec_relids, info->compressed_rel->relids))
			continue;
#if PG13_LT
		add_segmentby_to_equivalence_class(cur_ec, info, &context);
#else
		ec_added = add_segmentby_to_equivalence_class(cur_ec, info, &context);
		/* Record this EC index for the compressed rel */
		if (ec_added)
			info->compressed_rel->eclass_indexes =
				bms_add_member(info->compressed_rel->eclass_indexes, i);
#endif
	}
	info->compressed_rel->has_eclass_joins = info->chunk_rel->has_eclass_joins;
}

/*
 * create RangeTblEntry and RelOptInfo for the compressed chunk
 * and add it to PlannerInfo
 */
static void
decompress_chunk_add_plannerinfo(PlannerInfo *root, CompressionInfo *info, Chunk *chunk,
								 RelOptInfo *chunk_rel, bool needs_sequence_num)
{
	ListCell *lc;
	Index compressed_index = root->simple_rel_array_size;
	Chunk *compressed_chunk = ts_chunk_get_by_id(chunk->fd.compressed_chunk_id, true);
	Oid compressed_relid = compressed_chunk->table_id;
	RelOptInfo *compressed_rel;

	expand_planner_arrays(root, 1);
	info->compressed_rte = decompress_chunk_make_rte(compressed_relid, AccessShareLock);
	root->simple_rte_array[compressed_index] = info->compressed_rte;

	root->parse->rtable = lappend(root->parse->rtable, info->compressed_rte);

	root->simple_rel_array[compressed_index] = NULL;

	compressed_rel = build_simple_rel(root, compressed_index, NULL);
	/* github issue :1558
	 * set up top_parent_relids for this rel as the same as the
	 * original hypertable, otherwise eq classes are not computed correctly
	 * in generate_join_implied_equalities (called by
	 * get_baserel_parampathinfo <- create_index_paths)
	 */
	Assert(info->single_chunk || chunk_rel->top_parent_relids != NULL);
	compressed_rel->top_parent_relids = bms_copy(chunk_rel->top_parent_relids);

	root->simple_rel_array[compressed_index] = compressed_rel;
	info->compressed_rel = compressed_rel;
	foreach (lc, info->hypertable_compression_info)
	{
		FormData_hypertable_compression *fd = lfirst(lc);
		if (fd->segmentby_column_index <= 0)
		{
			/* store attnos for the compressed chunk here */
			AttrNumber compressed_chunk_attno =
				get_attnum(info->compressed_rte->relid, NameStr(fd->attname));
			info->compressed_chunk_compressed_attnos =
				bms_add_member(info->compressed_chunk_compressed_attnos, compressed_chunk_attno);
		}
	}
	compressed_rel_setup_reltarget(compressed_rel, info, needs_sequence_num);
	compressed_rel_setup_equivalence_classes(root, info);
	/* translate chunk_rel->joininfo for compressed_rel */
	compressed_rel_setup_joininfo(compressed_rel, info);
}

static DecompressChunkPath *
decompress_chunk_path_create(PlannerInfo *root, CompressionInfo *info, int parallel_workers,
							 Path *compressed_path)
{
	DecompressChunkPath *path;

	path = (DecompressChunkPath *) newNode(sizeof(DecompressChunkPath), T_CustomPath);

	path->info = info;

	path->cpath.path.pathtype = T_CustomScan;
	path->cpath.path.parent = info->chunk_rel;
	path->cpath.path.pathtarget = info->chunk_rel->reltarget;

	path->cpath.path.param_info = compressed_path->param_info;

	path->cpath.flags = 0;
	path->cpath.methods = &decompress_chunk_path_methods;
	path->sorted_merge_append = false;

	/* To prevent a non-parallel path with this node appearing
	 * in a parallel plan we only set parallel_safe to true
	 * when parallel_workers is greater than 0 which is only
	 * the case when creating partial paths. */
	path->cpath.path.parallel_safe = parallel_workers > 0;
	path->cpath.path.parallel_workers = parallel_workers;
	path->cpath.path.parallel_aware = false;

	path->cpath.custom_paths = list_make1(compressed_path);
	path->reverse = false;
	path->compressed_pathkeys = NIL;
	cost_decompress_chunk(&path->cpath.path, compressed_path);

	return path;
}

/* NOTE: this needs to be called strictly after all restrictinfos have been added
 *       to the compressed rel
 */

static void
create_compressed_scan_paths(PlannerInfo *root, RelOptInfo *compressed_rel, CompressionInfo *info,
							 SortInfo *sort_info)
{
	Path *compressed_path;

	/* clamp total_table_pages to 10 pages since this is the
	 * minimum estimate for number of pages.
	 * Add the value to any existing estimates
	 */
	root->total_table_pages += Max(compressed_rel->pages, 10);

	/* create non parallel scan path */
	compressed_path = create_seqscan_path(root, compressed_rel, NULL, 0);
	add_path(compressed_rel, compressed_path);

	/* create parallel scan path */
	if (compressed_rel->consider_parallel)
		ts_create_plain_partial_paths(root, compressed_rel);

	/*
	 * We set enable_bitmapscan to false here to ensure any pathes with bitmapscan do not
	 * displace other pathes. Note that setting the postgres GUC will not actually disable
	 * the bitmapscan path creation but will instead create them with very high cost.
	 * If bitmapscan were the dominant path after postgres planning we could end up
	 * in a situation where we have no valid plan for this relation because we remove
	 * bitmapscan pathes from the pathlist.
	 */

	bool old_bitmapscan = enable_bitmapscan;
	enable_bitmapscan = false;

	if (sort_info->can_pushdown_sort)
	{
		/*
		 * If we can push down sort below decompression we temporarily switch
		 * out root->query_pathkeys to allow matching to pathkeys produces by
		 * decompression
		 */
		List *orig_pathkeys = root->query_pathkeys;
		build_compressed_scan_pathkeys(sort_info, root, root->query_pathkeys, info);
		root->query_pathkeys = sort_info->compressed_pathkeys;
		check_index_predicates(root, compressed_rel);
		create_index_paths(root, compressed_rel);
		root->query_pathkeys = orig_pathkeys;
	}
	else
	{
		check_index_predicates(root, compressed_rel);
		create_index_paths(root, compressed_rel);
	}

	enable_bitmapscan = old_bitmapscan;
}

/*
 * create RangeTblEntry for compressed chunk
 */
static RangeTblEntry *
decompress_chunk_make_rte(Oid compressed_relid, LOCKMODE lockmode)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Relation r = table_open(compressed_relid, lockmode);
	int varattno;

	rte->rtekind = RTE_RELATION;
	rte->relid = compressed_relid;
	rte->relkind = r->rd_rel->relkind;
	rte->rellockmode = lockmode;
	rte->eref = makeAlias(RelationGetRelationName(r), NULL);

	/*
	 * inlined from buildRelationAliases()
	 * alias handling has been stripped because we won't
	 * need alias handling at this level
	 */
	for (varattno = 0; varattno < r->rd_att->natts; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(r->rd_att, varattno);
		/* Always insert an empty string for a dropped column */
		const char *attrname = attr->attisdropped ? "" : NameStr(attr->attname);
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(pstrdup(attrname)));
	}

	/*
	 * Drop the rel refcount, but keep the access lock till end of transaction
	 * so that the table can't be deleted or have its schema modified
	 * underneath us.
	 */
	table_close(r, NoLock);

	/*
	 * Set flags and access permissions.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 */
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = false;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid; /* not set-uid by default, either */
	rte->selectedCols = NULL;
	rte->insertedCols = NULL;
	rte->updatedCols = NULL;

	return rte;
}

FormData_hypertable_compression *
get_column_compressioninfo(List *hypertable_compression_info, char *column_name)
{
	ListCell *lc;

	foreach (lc, hypertable_compression_info)
	{
		FormData_hypertable_compression *fd = lfirst(lc);
		if (namestrcmp(&fd->attname, column_name) == 0)
			return fd;
	}
	elog(ERROR, "No compression information for column \"%s\" found.", column_name);

	pg_unreachable();
}

/*
 * Find toplevel equality constraints of segmentby columns in baserestrictinfo
 *
 * This will detect Var = Const and Var = Param and set the corresponding bit
 * in CompressionInfo->chunk_segmentby_ri
 */
static void
find_restrictinfo_equality(RelOptInfo *chunk_rel, CompressionInfo *info)
{
	Bitmapset *segmentby_columns = NULL;

	if (chunk_rel->baserestrictinfo != NIL)
	{
		ListCell *lc_ri;
		foreach (lc_ri, chunk_rel->baserestrictinfo)
		{
			RestrictInfo *ri = lfirst(lc_ri);

			if (IsA(ri->clause, OpExpr) && list_length(castNode(OpExpr, ri->clause)->args) == 2)
			{
				OpExpr *op = castNode(OpExpr, ri->clause);
				Var *var;
				Expr *other;

				if (op->opretset)
					continue;

				if (IsA(linitial(op->args), Var))
				{
					var = castNode(Var, linitial(op->args));
					other = lsecond(op->args);
				}
				else if (IsA(lsecond(op->args), Var))
				{
					var = castNode(Var, lsecond(op->args));
					other = linitial(op->args);
				}
				else
					continue;

				if ((Index) var->varno != chunk_rel->relid || var->varattno <= 0)
					continue;

				if (IsA(other, Const) || IsA(other, Param))
				{
					TypeCacheEntry *tce = lookup_type_cache(var->vartype, TYPECACHE_EQ_OPR);

					if (op->opno != tce->eq_opr)
						continue;

					if (bms_is_member(var->varattno, info->chunk_segmentby_attnos))
						segmentby_columns = bms_add_member(segmentby_columns, var->varattno);
				}
			}
		}
	}
	info->chunk_segmentby_ri = segmentby_columns;
}

/*
 * Check if we can push down the sort below the DecompressChunk node and fill
 * SortInfo accordingly
 *
 * The following conditions need to be true for pushdown:
 *  - all segmentby columns need to be prefix of pathkeys or have equality constraint
 *  - the rest of pathkeys needs to match compress_orderby
 *
 * If query pathkeys is shorter than segmentby + compress_orderby pushdown can still be done
 */
static SortInfo
build_sortinfo(Chunk *chunk, RelOptInfo *chunk_rel, CompressionInfo *info, List *pathkeys)
{
	int pk_index;
	PathKey *pk;
	Var *var;
	Expr *expr;
	char *column_name;
	FormData_hypertable_compression *ci;
	ListCell *lc = list_head(pathkeys);
	SortInfo sort_info = { .can_pushdown_sort = false, .needs_sequence_num = false };

	if (pathkeys == NIL || ts_chunk_is_unordered(chunk))
		return sort_info;

	/* all segmentby columns need to be prefix of pathkeys */
	if (info->num_segmentby_columns > 0)
	{
		Bitmapset *segmentby_columns;

		/*
		 * initialize segmentby with equality constraints from baserestrictinfo because
		 * those columns dont need to be prefix of pathkeys
		 */
		find_restrictinfo_equality(chunk_rel, info);
		segmentby_columns = bms_copy(info->chunk_segmentby_ri);

		/*
		 * loop over pathkeys until we find one that is not a segmentby column
		 * we keep looping even if we found all segmentby columns in case a
		 * columns appears both in baserestrictinfo and in ORDER BY clause
		 */
		for (; lc != NULL; lc = lnext_compat(pathkeys, lc))
		{
			Assert(bms_num_members(segmentby_columns) <= info->num_segmentby_columns);
			pk = lfirst(lc);
			expr = find_em_expr_for_rel(pk->pk_eclass, info->chunk_rel);

			if (expr == NULL || !IsA(expr, Var))
				break;
			var = castNode(Var, expr);

			if (var->varattno <= 0)
				break;

			column_name = get_attname(info->chunk_rte->relid, var->varattno, false);
			ci = get_column_compressioninfo(info->hypertable_compression_info, column_name);

			if (ci->segmentby_column_index <= 0)
				break;
			segmentby_columns = bms_add_member(segmentby_columns, var->varattno);
		}

		/*
		 * if pathkeys still has items but we didnt find all segmentby columns
		 * we cannot push down sort
		 */
		if (lc != NULL && bms_num_members(segmentby_columns) != info->num_segmentby_columns)
			return sort_info;
	}

	/*
	 * if pathkeys includes columns past segmentby columns
	 * we need sequence_num in the targetlist for ordering
	 */
	if (lc != NULL)
		sort_info.needs_sequence_num = true;

	/*
	 * loop over the rest of pathkeys
	 * this needs to exactly match the configured compress_orderby
	 */
	for (pk_index = 1; lc != NULL; lc = lnext_compat(pathkeys, lc), pk_index++)
	{
		bool reverse = false;
		pk = lfirst(lc);
		expr = find_em_expr_for_rel(pk->pk_eclass, info->chunk_rel);

		if (expr == NULL || !IsA(expr, Var))
			return sort_info;

		var = castNode(Var, expr);

		if (var->varattno <= 0)
			return sort_info;

		column_name = get_attname(info->chunk_rte->relid, var->varattno, false);
		ci = get_column_compressioninfo(info->hypertable_compression_info, column_name);

		if (ci->orderby_column_index != pk_index)
			return sort_info;

		/*
		 * pk_strategy is either BTLessStrategyNumber (for ASC) or
		 * BTGreaterStrategyNumber (for DESC)
		 */
		if (pk->pk_strategy == BTLessStrategyNumber)
		{
			if (ci->orderby_asc && ci->orderby_nullsfirst == pk->pk_nulls_first)
				reverse = false;
			else if (!ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first)
				reverse = true;
			else
				return sort_info;
		}
		else if (pk->pk_strategy == BTGreaterStrategyNumber)
		{
			if (!ci->orderby_asc && ci->orderby_nullsfirst == pk->pk_nulls_first)
				reverse = false;
			else if (ci->orderby_asc && ci->orderby_nullsfirst != pk->pk_nulls_first)
				reverse = true;
			else
				return sort_info;
		}

		/*
		 * first pathkey match determines if this is forward or backward scan
		 * any further pathkey items need to have same direction
		 */
		if (pk_index == 1)
			sort_info.reverse = reverse;
		else if (reverse != sort_info.reverse)
			return sort_info;
	}

	/* all pathkeys should be processed */
	Assert(lc == NULL);

	sort_info.can_pushdown_sort = true;
	return sort_info;
}
