/*-------------------------------------------------------------------------
 *
 * createas.c
 *	  Execution of CREATE TABLE ... AS, a/k/a SELECT INTO.
 *	  Since CREATE MATERIALIZED VIEW shares syntax and most behaviors,
 *	  we implement that here, too.
 *
 * We implement this by diverting the query's normal output to a
 * specialized DestReceiver type.
 *
 * Formerly, CTAS was implemented as a variant of SELECT, which led
 * to assorted legacy behaviors that we still try to preserve, notably that
 * we must return a tuples-processed count in the QueryCompletion.  (We no
 * longer do that for CTAS ... WITH NO DATA, however.)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/createas.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/index.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "commands/createas.h"
#include "commands/defrem.h"
#include "commands/matview.h"
#include "commands/prepare.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	IntoClause *into;			/* target relation specification */
	/* These fields are filled by intorel_startup: */
	Relation	rel;			/* relation to write to */
	ObjectAddress reladdr;		/* address of rel, for ExecCreateTableAs */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	int			ti_options;		/* table_tuple_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_intorel;

typedef struct
{
	bool	has_agg;
	bool	has_outerjoin;
	bool	has_subquery;
	bool	in_exists_subquery;	/* true, if it is in a exists subquery */
	List	*join_quals;
	List	*exists_qual_vars;
	int		sublevels_up;
} check_ivm_restriction_context;

/* utility functions for CTAS definition creation */
static ObjectAddress create_ctas_internal(List *attrList, IntoClause *into);
static ObjectAddress create_ctas_nodata(List *tlist, IntoClause *into);

/* DestReceiver routines for collecting data */
static void intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool intorel_receive(TupleTableSlot *slot, DestReceiver *self);
static void intorel_shutdown(DestReceiver *self);
static void intorel_destroy(DestReceiver *self);

static void CreateIvmTriggersOnBaseTablesRecurse(Query *qry, Node *node, Oid matviewOid,
									 Relids *relids, bool ex_lock);
static void CreateIvmTrigger(Oid relOid, Oid viewOid, int16 type, int16 timing, bool ex_lock);
static void check_ivm_restriction(Node *node);
static bool check_ivm_restriction_walker(Node *node, check_ivm_restriction_context *context);
static Bitmapset *get_primary_key_attnos_from_query(Query *query, List **constraintList, bool is_create);
static bool is_equijoin_condition(OpExpr *op);
static bool check_aggregate_supports_ivm(Oid aggfnoid);

/*
 * create_ctas_internal
 *
 * Internal utility used for the creation of the definition of a relation
 * created via CREATE TABLE AS or a materialized view.  Caller needs to
 * provide a list of attributes (ColumnDef nodes).
 */
static ObjectAddress
create_ctas_internal(List *attrList, IntoClause *into)
{
	CreateStmt *create = makeNode(CreateStmt);
	bool		is_matview;
	char		relkind;
	Datum		toast_options;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	ObjectAddress intoRelationAddr;

	/* This code supports both CREATE TABLE AS and CREATE MATERIALIZED VIEW */
	is_matview = (into->viewQuery != NULL);
	relkind = is_matview ? RELKIND_MATVIEW : RELKIND_RELATION;

	/*
	 * Create the target relation by faking up a CREATE TABLE parsetree and
	 * passing it to DefineRelation.
	 */
	create->relation = into->rel;
	create->tableElts = attrList;
	create->inhRelations = NIL;
	create->ofTypename = NULL;
	create->constraints = NIL;
	create->options = into->options;
	create->oncommit = into->onCommit;
	create->tablespacename = into->tableSpaceName;
	create->if_not_exists = false;
	/* Using Materialized view only */
	create->ivm = into->ivm;
	create->accessMethod = into->accessMethod;

	/*
	 * Create the relation.  (This will error out if there's an existing view,
	 * so we don't need more code to complain if "replace" is false.)
	 */
	intoRelationAddr = DefineRelation(create, relkind, InvalidOid, NULL, NULL);

	/*
	 * If necessary, create a TOAST table for the target table.  Note that
	 * NewRelationCreateToastTable ends with CommandCounterIncrement(), so
	 * that the TOAST table will be visible for insertion.
	 */
	CommandCounterIncrement();

	/* parse and validate reloptions for the toast table */
	toast_options = transformRelOptions((Datum) 0,
										create->options,
										"toast",
										validnsps,
										true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	NewRelationCreateToastTable(intoRelationAddr.objectId, toast_options);

	/* Create the "view" part of a materialized view. */
	if (is_matview)
	{
		/* StoreViewQuery scribbles on tree, so make a copy */
		Query	   *query = (Query *) copyObject(into->viewQuery);

		StoreViewQuery(intoRelationAddr.objectId, query, false);
		CommandCounterIncrement();
	}

	return intoRelationAddr;
}


/*
 * create_ctas_nodata
 *
 * Create CTAS or materialized view when WITH NO DATA is used, starting from
 * the targetlist of the SELECT or view definition.
 */
static ObjectAddress
create_ctas_nodata(List *tlist, IntoClause *into)
{
	List	   *attrList;
	ListCell   *t,
			   *lc;

	/*
	 * Build list of ColumnDefs from non-junk elements of the tlist.  If a
	 * column name list was specified in CREATE TABLE AS, override the column
	 * names in the query.  (Too few column names are OK, too many are not.)
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *col;
			char	   *colname;

			if (lc)
			{
				colname = strVal(lfirst(lc));
				lc = lnext(into->colNames, lc);
			}
			else
				colname = tle->resname;

			col = makeColumnDef(colname,
								exprType((Node *) tle->expr),
								exprTypmod((Node *) tle->expr),
								exprCollation((Node *) tle->expr));

			/*
			 * It's possible that the column is of a collatable type but the
			 * collation could not be resolved, so double-check.  (We must
			 * check this here because DefineRelation would adopt the type's
			 * default collation rather than complaining.)
			 */
			if (!OidIsValid(col->collOid) &&
				type_is_collatable(col->typeName->typeOid))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("no collation was derived for column \"%s\" with collatable type %s",
								col->colname,
								format_type_be(col->typeName->typeOid)),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));

			attrList = lappend(attrList, col);
		}
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/* Create the relation definition using the ColumnDef list */
	return create_ctas_internal(attrList, into);
}


/*
 * ExecCreateTableAs -- execute a CREATE TABLE AS command
 */
ObjectAddress
ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
				  ParamListInfo params, QueryEnvironment *queryEnv,
				  QueryCompletion *qc)
{
	Query	   *query = castNode(Query, stmt->query);
	IntoClause *into = stmt->into;
	bool		is_matview = (into->viewQuery != NULL);
	DestReceiver *dest;
	Oid			save_userid = InvalidOid;
	int			save_sec_context = 0;
	int			save_nestlevel = 0;
	ObjectAddress address;
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *query_immv = NULL;

	/* Check if the relation exists or not */
	if (CreateTableAsRelExists(stmt))
		return InvalidObjectAddress;

	/*
	 * Create the tuple receiver object and insert info it will need
	 */
	dest = CreateIntoRelDestReceiver(into);

	/*
	 * The contained Query could be a SELECT, or an EXECUTE utility command.
	 * If the latter, we just pass it off to ExecuteQuery.
	 */
	if (query->commandType == CMD_UTILITY &&
		IsA(query->utilityStmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = castNode(ExecuteStmt, query->utilityStmt);

		Assert(!is_matview);	/* excluded by syntax */
		ExecuteQuery(pstate, estmt, into, params, dest, qc);

		/* get object address that intorel_startup saved for us */
		address = ((DR_intorel *) dest)->reladdr;

		return address;
	}
	Assert(query->commandType == CMD_SELECT);

	/*
	 * For materialized views, lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.  This is
	 * not necessary for security, but this keeps the behavior similar to
	 * REFRESH MATERIALIZED VIEW.  Otherwise, one could create a materialized
	 * view not possible to refresh.
	 */
	if (is_matview)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(save_userid,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}

	if (is_matview && into->ivm)
	{
		/* check if the query is supported in IMMV definition */
		if (contain_mutable_functions((Node *) query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("mutable function is not supported on incrementally maintainable materialized view"),
					 errhint("functions must be marked IMMUTABLE")));

		check_ivm_restriction((Node *) query);

		/* For IMMV, we need to rewrite matview query */
		query = rewriteQueryForIMMV(query, into->colNames);
		query_immv = copyObject(query);
	}

	if (into->skipData)
	{
		/*
		 * If WITH NO DATA was specified, do not go through the rewriter,
		 * planner and executor.  Just define the relation using a code path
		 * similar to CREATE VIEW.  This avoids dump/restore problems stemming
		 * from running the planner before all dependencies are set up.
		 */
		address = create_ctas_nodata(query->targetList, into);
	}
	else
	{
		/*
		 * Parse analysis was done already, but we still have to run the rule
		 * rewriter.  We do not do AcquireRewriteLocks: we assume the query
		 * either came straight from the parser, or suitable locks were
		 * acquired by plancache.c.
		 */
		rewritten = QueryRewrite(query);

		/* SELECT should never rewrite to more or less than one SELECT query */
		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result for %s",
				 is_matview ? "CREATE MATERIALIZED VIEW" :
				 "CREATE TABLE AS SELECT");
		query = linitial_node(Query, rewritten);
		Assert(query->commandType == CMD_SELECT);

		/* plan the query */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, params);

		/*
		 * Use a snapshot with an updated command ID to ensure this query sees
		 * results of any previously executed queries.  (This could only
		 * matter if the planner executed an allegedly-stable function that
		 * changed the database contents, but let's do it anyway to be
		 * parallel to the EXPLAIN code path.)
		 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* Create a QueryDesc, redirecting output to our tuple receiver */
		queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
									GetActiveSnapshot(), InvalidSnapshot,
									dest, params, queryEnv, 0);

		/* call ExecutorStart to prepare the plan for execution */
		ExecutorStart(queryDesc, GetIntoRelEFlags(into));

		/* run the plan to completion */
		ExecutorRun(queryDesc, ForwardScanDirection, 0L, true);

		/* save the rowcount if we're given a qc to fill */
		if (qc)
			SetQueryCompletion(qc, CMDTAG_SELECT, queryDesc->estate->es_processed);

		/* get object address that intorel_startup saved for us */
		address = ((DR_intorel *) dest)->reladdr;

		/* and clean up */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);

		FreeQueryDesc(queryDesc);

		PopActiveSnapshot();
	}

	if (is_matview)
	{
		/* Roll back any GUC changes */
		AtEOXact_GUC(false, save_nestlevel);

		/* Restore userid and security context */
		SetUserIdAndSecContext(save_userid, save_sec_context);

		if (into->ivm)
		{
			Oid matviewOid = address.objectId;
			Relation matviewRel = table_open(matviewOid, NoLock);

			/*
			 * Mark relisivm field, if it's a matview and into->ivm is true.
			 */
			SetMatViewIVMState(matviewRel, true);

			if (!into->skipData)
			{
				/* Create an index on incremental maintainable materialized view, if possible */
				CreateIndexOnIMMV((Query *) into->viewQuery, matviewRel, true);

				/* Create triggers on incremental maintainable materialized view */
				Assert(query_immv != NULL);
				CreateIvmTriggersOnBaseTables(query_immv, matviewOid, true);
			}
			table_close(matviewRel, NoLock);
		}
	}

	return address;
}

/*
 * rewriteQueryForIMMV -- rewrite view definition query for IMMV
 *
 * count(*) is added for counting distinct tuples in views.
 * Also, additional hidden columns are added for aggregate values.
 *
 * EXISTS sublink is rewritten to LATERAL subquery with HAVING
 * clause to check count(*) > 0. In addition, a counting column
 * referring to count(*) in this subquery is added to the original
 * target list.
 */
Query *
rewriteQueryForIMMV(Query *query, List *colNames)
{
	Query *rewritten;

	TargetEntry *tle;
	Node *node;
	ParseState *pstate = make_parsestate(NULL);
	FuncCall *fn;

	rewritten = copyObject(query);
	pstate->p_expr_kind = EXPR_KIND_SELECT_TARGET;

	/*
	 * If this query has EXISTS clause, rewrite query and
	 * add __ivm_exists_count_X__ column.
	 */
	if (rewritten->hasSubLinks)
	{
		ListCell *lc;
		RangeTblEntry *rte;
		int varno = 0;

		/* rewrite EXISTS sublink to LATERAL subquery */
		rewrite_query_for_exists_subquery(rewritten);

		/* Add counting column referring to count(*) in EXISTS clause */
		foreach(lc, rewritten->rtable)
		{
			char *columnName;
			int attnum;
			Node *countCol = NULL;
			varno++;

			rte = (RangeTblEntry *) lfirst(lc);
			if (!rte->subquery || !rte->lateral)
				continue;
			pstate->p_rtable = rewritten->rtable;

			columnName = getColumnNameStartWith(rte, "__ivm_exists", &attnum);
			if (columnName == NULL)
				continue;
			countCol = (Node *)makeVar(varno, attnum,
						INT8OID, -1, InvalidOid, 0);


			if (countCol != NULL)
			{
				tle = makeTargetEntry((Expr *) countCol,
											list_length(rewritten->targetList) + 1,
											pstrdup(columnName),
											false);
				rewritten->targetList = list_concat(rewritten->targetList, list_make1(tle));
			}
		}
	}

	/* group keys must be in targetlist */
	if (rewritten->groupClause)
	{
		ListCell *lc;
		foreach(lc, rewritten->groupClause)
		{
			SortGroupClause *scl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(scl, rewritten->targetList);

			if (tle->resjunk)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("GROUP BY expression not appearing in select list is not supported on incrementally maintainable materialized view")));
		}
	}
	/* Convert DISTINCT to GROUP BY.  count(*) will be added afterward. */
	else if (!rewritten->hasAggs && rewritten->distinctClause)
		rewritten->groupClause = transformDistinctClause(NULL, &rewritten->targetList, rewritten->sortClause, false);

	/* Add additional columns for aggregate values */
	if (rewritten->hasAggs)
	{
		ListCell *lc;
		List *aggs = NIL;
		AttrNumber next_resno = list_length(rewritten->targetList) + 1;

		foreach(lc, rewritten->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			char *resname = (colNames == NIL ? tle->resname : strVal(list_nth(colNames, tle->resno - 1)));

			if (IsA(tle->expr, Aggref))
				makeIvmAggColumn(pstate, (Aggref *)tle->expr, resname, &next_resno, &aggs);
		}
		rewritten->targetList = list_concat(rewritten->targetList, aggs);
	}

	/* Add count(*) for counting distinct tuples in views */
	if (rewritten->distinctClause || rewritten->hasAggs)
	{
		fn = makeFuncCall(list_make1(makeString("count")), NIL, COERCE_EXPLICIT_CALL, -1);
		fn->agg_star = true;

		node = ParseFuncOrColumn(pstate, fn->funcname, NIL, NULL, fn, false, -1);

		tle = makeTargetEntry((Expr *) node,
								list_length(rewritten->targetList) + 1,
								pstrdup("__ivm_count__"),
								false);
		rewritten->targetList = lappend(rewritten->targetList, tle);
		rewritten->hasAggs = true;
	}

	return rewritten;
}

/*
 * makeIvmAggColumn -- make additional aggregate columns for IVM
 *
 * For an aggregate column specified by aggref, additional aggregate columns
 * are added, which are used to calculate the new aggregate value in IMMV.
 * An additional aggregate columns has a name based on resname
 * (ex. ivm_count_resname), and resno specified by next_resno. The created
 * columns are returned to aggs, and the resno for the next column is also
 * returned to next_resno.
 *
 * Currently, an additional count() is created for aggref other than count.
 * In addition, sum() is created for avg aggregate column.
 */
void
makeIvmAggColumn(ParseState *pstate, Aggref *aggref, char *resname, AttrNumber *next_resno, List **aggs)
{
	TargetEntry *tle_count;
	Node *node;
	FuncCall *fn;
	Const	*dmy_arg = makeConst(INT4OID,
								 -1,
								 InvalidOid,
								 sizeof(int32),
								 Int32GetDatum(1),
								 false,
								 true); /* pass by value */
	const char *aggname = get_func_name(aggref->aggfnoid);

	/*
	 * For aggregate functions except count, add count() func with the same arg parameters.
	 * This count result is used for determining if the aggregate value should be NULL or not.
	 * Also, add sum() func for avg because we need to calculate an average value as sum/count.
	 *
	 * XXX: If there are same expressions explicitly in the target list, we can use this instead
	 * of adding new duplicated one.
	 */
	if (strcmp(aggname, "count") != 0)
	{
		fn = makeFuncCall(list_make1(makeString("count")), NIL, COERCE_EXPLICIT_CALL, -1);

		/* Make a Func with a dummy arg, and then override this by the original agg's args. */
		node = ParseFuncOrColumn(pstate, fn->funcname, list_make1(dmy_arg), NULL, fn, false, -1);
		((Aggref *)node)->args = aggref->args;

		tle_count = makeTargetEntry((Expr *) node,
									*next_resno,
									pstrdup(makeObjectName("__ivm_count",resname, "_")),
									false);
		*aggs = lappend(*aggs, tle_count);
		(*next_resno)++;
	}
	if (strcmp(aggname, "avg") == 0)
	{
		List *dmy_args = NIL;
		ListCell *lc;
		foreach(lc, aggref->aggargtypes)
		{
			Oid		typeid = lfirst_oid(lc);
			Type	type = typeidType(typeid);

			Const *con = makeConst(typeid,
								   -1,
								   typeTypeCollation(type),
								   typeLen(type),
								   (Datum) 0,
								   true,
								   typeByVal(type));
			dmy_args = lappend(dmy_args, con);
			ReleaseSysCache(type);
		}
		fn = makeFuncCall(list_make1(makeString("sum")), NIL, COERCE_EXPLICIT_CALL, -1);

		/* Make a Func with dummy args, and then override this by the original agg's args. */
		node = ParseFuncOrColumn(pstate, fn->funcname, dmy_args, NULL, fn, false, -1);
		((Aggref *)node)->args = aggref->args;

		tle_count = makeTargetEntry((Expr *) node,
									*next_resno,
									pstrdup(makeObjectName("__ivm_sum",resname, "_")),
									false);
		*aggs = lappend(*aggs, tle_count);
		(*next_resno)++;
	}
}

/*
 * GetIntoRelEFlags --- compute executor flags needed for CREATE TABLE AS
 *
 * This is exported because EXPLAIN and PREPARE need it too.  (Note: those
 * callers still need to deal explicitly with the skipData flag; since they
 * use different methods for suppressing execution, it doesn't seem worth
 * trying to encapsulate that part.)
 */
int
GetIntoRelEFlags(IntoClause *intoClause)
{
	int			flags = 0;

	if (intoClause->skipData)
		flags |= EXEC_FLAG_WITH_NO_DATA;

	return flags;
}

/*
 * CreateTableAsRelExists --- check existence of relation for CreateTableAsStmt
 *
 * Utility wrapper checking if the relation pending for creation in this
 * CreateTableAsStmt query already exists or not.  Returns true if the
 * relation exists, otherwise false.
 */
bool
CreateTableAsRelExists(CreateTableAsStmt *ctas)
{
	Oid			nspid;
	IntoClause *into = ctas->into;

	nspid = RangeVarGetCreationNamespace(into->rel);

	if (get_relname_relid(into->rel->relname, nspid))
	{
		if (!ctas->if_not_exists)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists",
							into->rel->relname)));

		/* The relation exists and IF NOT EXISTS has been specified */
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						into->rel->relname)));
		return true;
	}

	/* Relation does not exist, it can be created */
	return false;
}

/*
 * CreateIntoRelDestReceiver -- create a suitable DestReceiver object
 *
 * intoClause will be NULL if called from CreateDestReceiver(), in which
 * case it has to be provided later.  However, it is convenient to allow
 * self->into to be filled in immediately for other callers.
 */
DestReceiver *
CreateIntoRelDestReceiver(IntoClause *intoClause)
{
	DR_intorel *self = (DR_intorel *) palloc0(sizeof(DR_intorel));

	self->pub.receiveSlot = intorel_receive;
	self->pub.rStartup = intorel_startup;
	self->pub.rShutdown = intorel_shutdown;
	self->pub.rDestroy = intorel_destroy;
	self->pub.mydest = DestIntoRel;
	self->into = intoClause;
	/* other private fields will be set during intorel_startup */

	return (DestReceiver *) self;
}

/*
 * intorel_startup --- executor startup
 */
static void
intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;
	bool		is_matview;
	List	   *attrList;
	ObjectAddress intoRelationAddr;
	Relation	intoRelationDesc;
	ListCell   *lc;
	int			attnum;

	Assert(into != NULL);		/* else somebody forgot to set it */

	/* This code supports both CREATE TABLE AS and CREATE MATERIALIZED VIEW */
	is_matview = (into->viewQuery != NULL);

	/*
	 * Build column definitions using "pre-cooked" type and collation info. If
	 * a column name list was specified in CREATE TABLE AS, override the
	 * column names derived from the query.  (Too few column names are OK, too
	 * many are not.)
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	for (attnum = 0; attnum < typeinfo->natts; attnum++)
	{
		Form_pg_attribute attribute = TupleDescAttr(typeinfo, attnum);
		ColumnDef  *col;
		char	   *colname;

		if (lc)
		{
			colname = strVal(lfirst(lc));
			lc = lnext(into->colNames, lc);
		}
		else
			colname = NameStr(attribute->attname);

		col = makeColumnDef(colname,
							attribute->atttypid,
							attribute->atttypmod,
							attribute->attcollation);

		/*
		 * It's possible that the column is of a collatable type but the
		 * collation could not be resolved, so double-check.  (We must check
		 * this here because DefineRelation would adopt the type's default
		 * collation rather than complaining.)
		 */
		if (!OidIsValid(col->collOid) &&
			type_is_collatable(col->typeName->typeOid))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_COLLATION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							col->colname,
							format_type_be(col->typeName->typeOid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));

		attrList = lappend(attrList, col);
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/*
	 * Actually create the target table
	 */
	intoRelationAddr = create_ctas_internal(attrList, into);

	/*
	 * Finally we can open the target table
	 */
	intoRelationDesc = table_open(intoRelationAddr.objectId, AccessExclusiveLock);

	/*
	 * Make sure the constructed table does not have RLS enabled.
	 *
	 * check_enable_rls() will ereport(ERROR) itself if the user has requested
	 * something invalid, and otherwise will return RLS_ENABLED if RLS should
	 * be enabled here.  We don't actually support that currently, so throw
	 * our own ereport(ERROR) if that happens.
	 */
	if (check_enable_rls(intoRelationAddr.objectId, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("policies not yet implemented for this command")));

	/*
	 * Tentatively mark the target as populated, if it's a matview and we're
	 * going to fill it; otherwise, no change needed.
	 */
	if (is_matview && !into->skipData)
		SetMatViewPopulatedState(intoRelationDesc, true);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->rel = intoRelationDesc;
	myState->reladdr = intoRelationAddr;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM;

	/*
	 * If WITH NO DATA is specified, there is no need to set up the state for
	 * bulk inserts as there are no tuples to insert.
	 */
	if (!into->skipData)
		myState->bistate = GetBulkInsertState();
	else
		myState->bistate = NULL;

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(intoRelationDesc) == InvalidBlockNumber);
}

/*
 * intorel_receive --- receive one tuple
 */
static bool
intorel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;

	/* Nothing to insert if WITH NO DATA is specified. */
	if (!myState->into->skipData)
	{
		/*
		 * Note that the input slot might not be of the type of the target
		 * relation. That's supported by table_tuple_insert(), but slightly
		 * less efficient than inserting with the right slot - but the
		 * alternative would be to copy into a slot of the right type, which
		 * would not be cheap either. This also doesn't allow accessing per-AM
		 * data (say a tuple's xmin), but since we don't do that here...
		 */
		table_tuple_insert(myState->rel,
						   slot,
						   myState->output_cid,
						   myState->ti_options,
						   myState->bistate);
	}

	/* We know this is a newly created relation, so there are no indexes */

	return true;
}

/*
 * intorel_shutdown --- executor end
 */
static void
intorel_shutdown(DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;

	if (!into->skipData)
	{
		FreeBulkInsertState(myState->bistate);
		table_finish_bulk_insert(myState->rel, myState->ti_options);
	}

	/* close rel, but keep lock until commit */
	table_close(myState->rel, NoLock);
	myState->rel = NULL;
}

/*
 * intorel_destroy --- release DestReceiver object
 */
static void
intorel_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * CreateIvmTriggersOnBaseTables -- create IVM triggers on all base tables
 */
void
CreateIvmTriggersOnBaseTables(Query *qry, Oid matviewOid, bool is_create)
{
	Relids	relids = NULL;
	bool	ex_lock = false;
	Index	first_rtindex = is_create ? 1 : PRS2_NEW_VARNO + 1;
	RangeTblEntry *rte;

	/* Immediately return if we don't have any base tables. */
	if (list_length(qry->rtable) < first_rtindex)
		return;

	/*
	 * If the view has more than one base tables, we need an exclusive lock
	 * on the view so that the view would be maintained serially to avoid
	 * the inconsistency that occurs when two base tables are modified in
	 * concurrent transactions. However, if the view has only one table,
	 * we can use a weaker lock.
	 *
	 * The type of lock should be determined here, because if we check the
	 * view definition at maintenance time, we need to acquire a weaker lock,
	 * and upgrading the lock level after this increases probability of
	 * deadlock.
	 */

	rte = list_nth(qry->rtable, first_rtindex - 1);
	if (list_length(qry->rtable) > first_rtindex ||
		rte->rtekind != RTE_RELATION)
		ex_lock = true;

	CreateIvmTriggersOnBaseTablesRecurse(qry, (Node *)qry, matviewOid, &relids, ex_lock);

	bms_free(relids);
}

static void
CreateIvmTriggersOnBaseTablesRecurse(Query *qry, Node *node, Oid matviewOid,
									 Relids *relids, bool ex_lock)
{
	if (node == NULL)
		return;

	/* This can recurse, so check for excessive recursion */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Query:
			{
				Query *query = (Query *) node;
				ListCell *lc;

				CreateIvmTriggersOnBaseTablesRecurse(qry, (Node *)query->jointree, matviewOid, relids, ex_lock);
				foreach(lc, query->cteList)
				{
					CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
					Assert(IsA(cte->ctequery, Query));
					CreateIvmTriggersOnBaseTablesRecurse((Query *) cte->ctequery, cte->ctequery, matviewOid, relids, ex_lock);
				}
			}
			break;

		case T_RangeTblRef:
			{
				int			rti = ((RangeTblRef *) node)->rtindex;
				RangeTblEntry *rte = rt_fetch(rti, qry->rtable);

				if (rte->rtekind == RTE_RELATION && !bms_is_member(rte->relid, *relids))
				{
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_INSERT, TRIGGER_TYPE_BEFORE, ex_lock);
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_DELETE, TRIGGER_TYPE_BEFORE, ex_lock);
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_UPDATE, TRIGGER_TYPE_BEFORE, ex_lock);
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_INSERT, TRIGGER_TYPE_AFTER, ex_lock);
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_DELETE, TRIGGER_TYPE_AFTER, ex_lock);
					CreateIvmTrigger(rte->relid, matviewOid, TRIGGER_TYPE_UPDATE, TRIGGER_TYPE_AFTER, ex_lock);

					*relids = bms_add_member(*relids, rte->relid);
				}
				else if (rte->rtekind == RTE_SUBQUERY)
				{
					Query *subquery = rte->subquery;
					Assert(rte->subquery != NULL);

					CreateIvmTriggersOnBaseTablesRecurse(subquery, (Node *)subquery, matviewOid, relids, ex_lock);
				}
			}
			break;

		case T_FromExpr:
			{
				FromExpr   *f = (FromExpr *) node;
				ListCell   *l;

				foreach(l, f->fromlist)
					CreateIvmTriggersOnBaseTablesRecurse(qry, lfirst(l), matviewOid, relids, ex_lock);
			}
			break;

		case T_JoinExpr:
			{
				JoinExpr   *j = (JoinExpr *) node;

				CreateIvmTriggersOnBaseTablesRecurse(qry, j->larg, matviewOid, relids, ex_lock);
				CreateIvmTriggersOnBaseTablesRecurse(qry, j->rarg, matviewOid, relids, ex_lock);
			}
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
}

/*
 * CreateIvmTrigger -- create IVM trigger on a base table
 */
static void
CreateIvmTrigger(Oid relOid, Oid viewOid, int16 type, int16 timing, bool ex_lock)
{
	ObjectAddress	refaddr;
	ObjectAddress	address;
	CreateTrigStmt *ivm_trigger;
	List *transitionRels = NIL;

	Assert(timing == TRIGGER_TYPE_BEFORE || timing == TRIGGER_TYPE_AFTER);

	refaddr.classId = RelationRelationId;
	refaddr.objectId = viewOid;
	refaddr.objectSubId = 0;

	ivm_trigger = makeNode(CreateTrigStmt);
	ivm_trigger->relation = NULL;
	ivm_trigger->row = false;

	ivm_trigger->timing = timing;
	ivm_trigger->events = type;

	switch (type)
	{
		case TRIGGER_TYPE_INSERT:
			ivm_trigger->trigname = (timing == TRIGGER_TYPE_BEFORE ? "IVM_trigger_ins_before" : "IVM_trigger_ins_after");
			break;
		case TRIGGER_TYPE_DELETE:
			ivm_trigger->trigname = (timing == TRIGGER_TYPE_BEFORE ? "IVM_trigger_del_before" : "IVM_trigger_del_after");
			break;
		case TRIGGER_TYPE_UPDATE:
			ivm_trigger->trigname = (timing == TRIGGER_TYPE_BEFORE ? "IVM_trigger_upd_before" : "IVM_trigger_upd_after");
			break;
		default:
			elog(ERROR, "unsupported trigger type");
	}

	if (timing == TRIGGER_TYPE_AFTER)
	{
		if (type == TRIGGER_TYPE_INSERT || type == TRIGGER_TYPE_UPDATE)
		{
			TriggerTransition *n = makeNode(TriggerTransition);
			n->name = "__ivm_newtable";
			n->isNew = true;
			n->isTable = true;

			transitionRels = lappend(transitionRels, n);
		}
		if (type == TRIGGER_TYPE_DELETE || type == TRIGGER_TYPE_UPDATE)
		{
			TriggerTransition *n = makeNode(TriggerTransition);
			n->name = "__ivm_oldtable";
			n->isNew = false;
			n->isTable = true;

			transitionRels = lappend(transitionRels, n);
		}
	}

	ivm_trigger->funcname =
		(timing == TRIGGER_TYPE_BEFORE ? SystemFuncName("IVM_immediate_before") : SystemFuncName("IVM_immediate_maintenance"));

	ivm_trigger->columns = NIL;
	ivm_trigger->transitionRels = transitionRels;
	ivm_trigger->whenClause = NULL;
	ivm_trigger->isconstraint = false;
	ivm_trigger->deferrable = false;
	ivm_trigger->initdeferred = false;
	ivm_trigger->constrrel = NULL;
	ivm_trigger->args = list_make2(
		makeString(DatumGetPointer(DirectFunctionCall1(oidout, ObjectIdGetDatum(viewOid)))),
		makeString(DatumGetPointer(DirectFunctionCall1(boolout, BoolGetDatum(ex_lock))))
		);

	address = CreateTrigger(ivm_trigger, NULL, relOid, InvalidOid, InvalidOid,
						 InvalidOid, InvalidOid, InvalidOid, NULL, true, false);

	recordDependencyOn(&address, &refaddr, DEPENDENCY_AUTO);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * check_ivm_restriction --- look for specify nodes in the query tree
 */
static void
check_ivm_restriction(Node *node)
{
	check_ivm_restriction_context context = {false, false, false, false, NIL, NIL, 0};

	check_ivm_restriction_walker(node, &context);
}

static bool
check_ivm_restriction_walker(Node *node, check_ivm_restriction_context *context)
{
	if (node == NULL)
		return false;

	/* This can recurse, so check for excessive recursion */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Query:
			{
				Query *qry = (Query *)node;
				ListCell   *lc;
				List       *vars;

				if (qry->havingQual != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg(" HAVING clause is not supported on incrementally maintainable materialized view")));
				if (qry->sortClause != NIL)	/* There is a possibility that we don't need to return an error */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ORDER BY clause is not supported on incrementally maintainable materialized view")));
				if (qry->limitOffset != NULL || qry->limitCount != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("LIMIT/OFFSET clause is not supported on incrementally maintainable materialized view")));
				if (qry->hasDistinctOn)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DISTINCT ON is not supported on incrementally maintainable materialized view")));
				if (qry->hasWindowFuncs)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("window functions are not supported on incrementally maintainable materialized view")));
				if (qry->groupingSets != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("GROUPING SETS, ROLLUP, or CUBE clauses is not supported on incrementally maintainable materialized view")));
				if (qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("UNION/INTERSECT/EXCEPT statements are not supported on incrementally maintainable materialized view")));
				if (list_length(qry->targetList) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("empty target list is not supported on incrementally maintainable materialized view")));
				if (qry->rowMarks != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("FOR UPDATE/SHARE clause is not supported on incrementally maintainable materialized view")));

				/* CTE restrictions */
				if (qry->hasRecursive)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("recursive CTE is not supported on incrementally maintainable materialized view")));

				/* system column restrictions */
				vars = pull_vars_of_level((Node *) qry, 0);
				foreach(lc, vars)
				{
					if (IsA(lfirst(lc), Var))
					{
						Var *var = (Var *) lfirst(lc);
						/* if system column, return error */
						if (var->varattno < 0)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("system column is not supported on incrementally maintainable materialized view")));
					}
				}

				/* subquery restrictions */
				if (context->sublevels_up > 0 && qry->distinctClause != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DISTINCT clause in nested query are not supported on incrementally maintainable materialized view")));
				if (context->sublevels_up > 0 && qry->hasAggs)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("aggregate functions in nested query are not supported on incrementally maintainable materialized view")));

				context->has_agg |= qry->hasAggs;

				/* restrictions for rtable */
				foreach(lc, qry->rtable)
				{
					RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

					if (rte->tablesample != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("TABLESAMPLE clause is not supported on incrementally maintainable materialized view")));

					if (rte->relkind == RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("partitioned table is not supported on incrementally maintainable materialized view")));

					if (rte->relkind == RELKIND_RELATION && has_superclass(rte->relid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("partitions is not supported on incrementally maintainable materialized view")));

					if (rte->relkind == RELKIND_RELATION && find_inheritance_children(rte->relid, NoLock) != NIL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("inheritance parent is not supported on incrementally maintainable materialized view")));

					if (rte->relkind == RELKIND_FOREIGN_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign table is not supported on incrementally maintainable materialized view")));

					if (rte->relkind == RELKIND_VIEW ||
						rte->relkind == RELKIND_MATVIEW)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("VIEW or MATERIALIZED VIEW is not supported on incrementally maintainable materialized view")));

					if (rte->rtekind == RTE_VALUES)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("VALUES is not supported on incrementally maintainable materialized view")));

					if (rte->rtekind == RTE_SUBQUERY)
					{
						if (context->has_outerjoin)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("this query is not allowed on incrementally maintainable materialized view"),
									 errhint("subquery or CTE is not supported with outer join")));

						context->has_subquery = true;

						context->sublevels_up++;
						check_ivm_restriction_walker((Node *)rte->subquery, context);
						context->sublevels_up--;
					}
				}

				query_tree_walker(qry, check_ivm_restriction_walker, (void *) context, QTW_IGNORE_RANGE_TABLE);

				/* additional restriction checks for exists subquery */
				if (context->exists_qual_vars != NIL && context->sublevels_up == 0)
				{
					ListCell *lc;

					foreach (lc, context->exists_qual_vars)
					{
						Var	*var = (Var *) lfirst(lc);
						ListCell *lc2;
						bool found = false;

						foreach(lc2, qry->targetList)
						{
							TargetEntry	*tle = lfirst(lc2);
							Var *var2;

							if (!IsA(tle->expr, Var))
								continue;
							var2 = (Var *) tle->expr;
							if (var->varno == var2->varno && var->varattno == var2->varattno)
							{
								found = true;
								break;
							}
						}
						if (!found)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("this query is not allowed on incrementally maintainable materialized view"),
									 errhint("targetlist must contain vars that are referred to in EXISTS subquery")));
					}
				}

				/* additional restriction checks for outer join query */
				if (context->has_outerjoin && context->sublevels_up == 0)
				{
					List	*where_quals_vars = NIL;
					List	*nonnullable_vars = find_nonnullable_vars((Node *) qry->jointree->quals);
					List	*qual_vars = NIL;
					ListCell *lc;

					foreach (lc, context->join_quals)
					{
						OpExpr	*op = (OpExpr *) lfirst(lc);

						if (!is_equijoin_condition(op))
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("this query is not allowed on incrementally maintainable materialized view"),
										 errhint("Only simple equijoin is supported with outer join")));

						op = (OpExpr *) flatten_join_alias_vars(qry, (Node *) op);
						qual_vars = list_concat(qual_vars, pull_vars_of_level((Node *) op, 0));
					}

					foreach (lc, qual_vars)
					{
						Var	*var = lfirst(lc);
						ListCell *lc2;
						bool found = false;

						foreach(lc2, qry->targetList)
						{
							TargetEntry	*tle = lfirst(lc2);

							if (IsA(tle->expr, Var))
							{
								Var *var2 = (Var *) flatten_join_alias_vars(qry, (Node *) tle->expr);
								if (var->varno == var2->varno && var->varattno == var2->varattno)
								{
									found = true;
									break;
								}
							}
						}
						if (!found)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("this query is not allowed on incrementally maintainable materialized view"),
									 errhint("targetlist must contain vars in the join condition with outer join")));
					}

					where_quals_vars = pull_vars_of_level(flatten_join_alias_vars(qry, (Node *) qry->jointree->quals), 0);

					if (list_length(list_difference(where_quals_vars, nonnullable_vars)) > 0)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("this query is not allowed on incrementally maintainable materialized view"),
								 errhint("WHERE cannot contain non null-rejecting predicates with outer join")));

					if (contain_nonstrict_functions((Node *) qry->targetList))
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("this query is not allowed on incrementally maintainable materialized view"),
									 errhint("targetlist cannot contain non strict functions with outer join")));
				}

				break;
			}
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				if (isIvmName(cte->ctename))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("CTE name %s is not supported on incrementally maintainable materialized view", cte->ctename)));

				context->sublevels_up++;
				check_ivm_restriction_walker(cte->ctequery, (void *) context);
				context->sublevels_up--;
				break;
			}
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *)node;
				if (isIvmName(tle->resname))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("column name %s is not supported on incrementally maintainable materialized view", tle->resname)));
				if (context->has_agg && !IsA(tle->expr, Aggref) && contain_aggs_of_level((Node *) tle->expr, 0))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("expression containing an aggregate in it is not supported on incrementally maintainable materialized view")));
				if (IsA(tle->expr, SubLink))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("this query is not allowed on incrementally maintainable materialized view"),
							 errhint("subquery is not supported in targetlist")));

				expression_tree_walker(node, check_ivm_restriction_walker, (void *) context);
				break;
			}
		case T_JoinExpr:
			{
				JoinExpr *joinexpr = (JoinExpr *)node;
				if (IS_OUTER_JOIN(joinexpr->jointype))
				{
					if (context->has_subquery)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("this query is not allowed on incrementally maintainable materialized view"),
								 errhint("subquery or CTE is not supported with outer join")));
					if (context->has_agg)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("this query is not allowed on incrementally maintainable materialized view"),
								 errhint("aggregate is not supported with outer join")));

					context->has_outerjoin = true;
					context->join_quals = lappend(context->join_quals, joinexpr->quals);
				}
				expression_tree_walker(node, check_ivm_restriction_walker, (void *) context);
				break;
			}
		case T_Var:
			{
				Var	*variable = (Var *) node;
				/* If EXISTS subquery refers to vars of the upper query, collect these vars */
				if (variable->varlevelsup > 0 && context->in_exists_subquery)
					context->exists_qual_vars = lappend(context->exists_qual_vars, node);
				break;
			}
		case T_SubLink:
			{
				/* Now, EXISTS clause is supported only */
				SubLink	*sublink = (SubLink *) node;
				if (sublink->subLinkType != EXISTS_SUBLINK)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("this query is not allowed on incrementally maintainable materialized view"),
							 errhint("subquery in WHERE clause only supports subquery with EXISTS clause")));
				if (context->sublevels_up > 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("nested subquery is not supported on incrementally maintainable materialized view")));

				if (context->has_outerjoin)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("this query is not allowed on incrementally maintainable materialized view"),
							 errhint("subquery with outer join is not supported")));

				context->in_exists_subquery = true;
				context->sublevels_up++;
				check_ivm_restriction_walker(sublink->subselect, context);
				context->sublevels_up--;
				context->in_exists_subquery = false;
				break;
			}
		case T_Aggref:
			{
				/* Check if this supports IVM */
				Aggref *aggref = (Aggref *) node;
				const char *aggname = format_procedure(aggref->aggfnoid);

				if (aggref->aggfilter != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("aggregate function with FILTER clause is not supported on incrementally maintainable materialized view")));

				if (aggref->aggdistinct != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("aggregate function with DISTINCT arguments is not supported on incrementally maintainable materialized view")));

				if (aggref->aggorder != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("aggregate function with ORDER clause is not supported on incrementally maintainable materialized view")));

				if (!check_aggregate_supports_ivm(aggref->aggfnoid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("aggregate function %s is not supported on incrementally maintainable materialized view", aggname)));
				break;
			}
		default:
			expression_tree_walker(node, check_ivm_restriction_walker, (void *) context);
			break;
	}
	return false;
}

/*
 * is_equijoin_condition - check if all operators must be btree equality or hash equality
 */
static bool
is_equijoin_condition(OpExpr *op)
{
	Oid			opno;
	Node	   *left_expr;
	Node	   *right_expr;
	Relids		left_varnos;
	Relids		right_varnos;
	Oid			opinputtype;
	PlannerInfo root;

	/* Is it a binary opclause? */
	if (!IsA(op, OpExpr) || list_length(op->args) != 2)
		return false;

	opno = op->opno;
	left_expr = linitial(op->args);
	right_expr = lsecond(op->args);
	left_varnos = pull_varnos(&root, left_expr);
	right_varnos = pull_varnos(&root, right_expr);
	opinputtype = exprType(left_expr);

	if (bms_num_members(left_varnos) != 1 || bms_num_members(right_varnos) != 1 ||
		bms_equal(left_varnos, right_varnos) !=0)
		return false;

	if (op_mergejoinable(opno, opinputtype) && get_mergejoin_opfamilies(opno) != NIL)
		return true;

	if (op_hashjoinable(opno, opinputtype))
		return true;

	return false;
}

/*
 * check_aggregate_supports_ivm
 *
 * Check if the given aggregate function is supporting IVM
 */
static bool
check_aggregate_supports_ivm(Oid aggfnoid)
{
	switch (aggfnoid)
	{
		/* count */
		case F_COUNT_ANY:
		case F_COUNT_:

		/* sum */
		case F_SUM_INT8:
		case F_SUM_INT4:
		case F_SUM_INT2:
		case F_SUM_FLOAT4:
		case F_SUM_FLOAT8:
		case F_SUM_MONEY:
		case F_SUM_INTERVAL:
		case F_SUM_NUMERIC:

		/* avg */
		case F_AVG_INT8:
		case F_AVG_INT4:
		case F_AVG_INT2:
		case F_AVG_NUMERIC:
		case F_AVG_FLOAT4:
		case F_AVG_FLOAT8:
		case F_AVG_INTERVAL:

		/* min */
		case F_MIN_ANYARRAY:
		case F_MIN_INT8:
		case F_MIN_INT4:
		case F_MIN_INT2:
		case F_MIN_OID:
		case F_MIN_FLOAT4:
		case F_MIN_FLOAT8:
		case F_MIN_DATE:
		case F_MIN_TIME:
		case F_MIN_TIMETZ:
		case F_MIN_MONEY:
		case F_MIN_TIMESTAMP:
		case F_MIN_TIMESTAMPTZ:
		case F_MIN_INTERVAL:
		case F_MIN_TEXT:
		case F_MIN_NUMERIC:
		case F_MIN_BPCHAR:
		case F_MIN_TID:
		case F_MIN_ANYENUM:
		case F_MIN_INET:
		case F_MIN_PG_LSN:

		/* max */
		case F_MAX_ANYARRAY:
		case F_MAX_INT8:
		case F_MAX_INT4:
		case F_MAX_INT2:
		case F_MAX_OID:
		case F_MAX_FLOAT4:
		case F_MAX_FLOAT8:
		case F_MAX_DATE:
		case F_MAX_TIME:
		case F_MAX_TIMETZ:
		case F_MAX_MONEY:
		case F_MAX_TIMESTAMP:
		case F_MAX_TIMESTAMPTZ:
		case F_MAX_INTERVAL:
		case F_MAX_TEXT:
		case F_MAX_NUMERIC:
		case F_MAX_BPCHAR:
		case F_MAX_TID:
		case F_MAX_ANYENUM:
		case F_MAX_INET:
		case F_MAX_PG_LSN:
			return true;

		default:
			return false;
	}
}

/*
 * CreateIndexOnIMMV
 *
 * Create a unique index on incremental maintainable materialized view.
 * If the view definition query has a GROUP BY clause, the index is created
 * on the columns of GROUP BY expressions. Otherwise, if the view contains
 * all primary key attritubes of its base tables in the target list, the index
 * is created on these attritubes. In other cases, no index is created.
 */
void
CreateIndexOnIMMV(Query *query, Relation matviewRel, bool is_create)
{
	ListCell *lc;
	IndexStmt  *index;
	ObjectAddress address;
	List *constraintList = NIL;
	char		idxname[NAMEDATALEN];
	List	   *indexoidlist = RelationGetIndexList(matviewRel);
	ListCell   *indexoidscan;

	snprintf(idxname, sizeof(idxname), "%s_index", RelationGetRelationName(matviewRel));

	index = makeNode(IndexStmt);

	/*
	 * We consider null values not distinct to make sure that views with DISTINCT
	 * or GROUP BY don't contain multiple NULL rows when NULL is inserted to
	 * a base table concurrently.
	 */
	index->nulls_not_distinct = true;

	index->unique = true;
	index->primary = false;
	index->isconstraint = false;
	index->deferrable = false;
	index->initdeferred = false;
	index->idxname = idxname;
	index->relation =
		makeRangeVar(get_namespace_name(RelationGetNamespace(matviewRel)),
					 pstrdup(RelationGetRelationName(matviewRel)),
					 -1);
	index->accessMethod = DEFAULT_INDEX_TYPE;
	index->options = NIL;
	index->tableSpace = get_tablespace_name(matviewRel->rd_rel->reltablespace);
	index->whereClause = NULL;
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilenodeSubid = InvalidSubTransactionId;
	index->transformed = true;
	index->concurrent = false;
	index->if_not_exists = false;

	if (query->groupClause)
	{
		/* create unique constraint on GROUP BY expression columns */
		foreach(lc, query->groupClause)
		{
			SortGroupClause *scl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(scl, query->targetList);
			Form_pg_attribute attr = TupleDescAttr(matviewRel->rd_att, tle->resno - 1);
			IndexElem  *iparam;

			iparam = makeNode(IndexElem);
			iparam->name = pstrdup(NameStr(attr->attname));
			iparam->expr = NULL;
			iparam->indexcolname = NULL;
			iparam->collation = NIL;
			iparam->opclass = NIL;
			iparam->opclassopts = NIL;
			iparam->ordering = SORTBY_DEFAULT;
			iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
			index->indexParams = lappend(index->indexParams, iparam);
		}
	}
	else if (query->distinctClause)
	{
		/* create unique constraint on all columns */
		foreach(lc, query->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Form_pg_attribute attr = TupleDescAttr(matviewRel->rd_att, tle->resno - 1);
			IndexElem  *iparam;

			iparam = makeNode(IndexElem);
			iparam->name = pstrdup(NameStr(attr->attname));
			iparam->expr = NULL;
			iparam->indexcolname = NULL;
			iparam->collation = NIL;
			iparam->opclass = NIL;
			iparam->opclassopts = NIL;
			iparam->ordering = SORTBY_DEFAULT;
			iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
			index->indexParams = lappend(index->indexParams, iparam);
		}
	}
	else
	{
		Bitmapset *key_attnos;

		/* create index on the base tables' primary key columns */
		key_attnos = get_primary_key_attnos_from_query(query, &constraintList, is_create);
		if (key_attnos)
		{
			foreach(lc, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);
				Form_pg_attribute attr = TupleDescAttr(matviewRel->rd_att, tle->resno - 1);

				if (bms_is_member(tle->resno - FirstLowInvalidHeapAttributeNumber, key_attnos))
				{
					IndexElem  *iparam;

					iparam = makeNode(IndexElem);
					iparam->name = pstrdup(NameStr(attr->attname));
					iparam->expr = NULL;
					iparam->indexcolname = NULL;
					iparam->collation = NIL;
					iparam->opclass = NIL;
					iparam->opclassopts = NIL;
					iparam->ordering = SORTBY_DEFAULT;
					iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
					index->indexParams = lappend(index->indexParams, iparam);
				}
			}
		}
		else
		{
			/* create no index, just notice that an appropriate index is necessary for efficient IVM */
			ereport(NOTICE,
					(errmsg("could not create an index on materialized view \"%s\" automatically",
							RelationGetRelationName(matviewRel)),
					 errdetail("This target list does not have all the primary key columns, "
							   "or this view does not contain GROUP BY or DISTINCT clause."),
					 errhint("Create an index on the materialized view for efficient incremental maintenance.")));
			return;
		}
	}

	/* If we have a compatible index, we don't need to create another. */
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;
		bool		hasCompatibleIndex = false;

		indexRel = index_open(indexoid, AccessShareLock);

		if (CheckIndexCompatible(indexRel->rd_id,
								index->accessMethod,
								index->indexParams,
								index->excludeOpNames))
			hasCompatibleIndex = true;

		index_close(indexRel, AccessShareLock);

		if (hasCompatibleIndex)
			return;
	}

	address = DefineIndex(RelationGetRelid(matviewRel),
						  index,
						  InvalidOid,
						  InvalidOid,
						  InvalidOid,
						  false, true, false, false, true);

	ereport(NOTICE,
			(errmsg("created index \"%s\" on materialized view \"%s\"",
					idxname, RelationGetRelationName(matviewRel))));

	/*
	 * Make dependencies so that the index is dropped if any base tables's
	 * primary key is dropped.
	 */
	foreach(lc, constraintList)
	{
		Oid constraintOid = lfirst_oid(lc);
		ObjectAddress	refaddr;

		refaddr.classId = ConstraintRelationId;
		refaddr.objectId = constraintOid;
		refaddr.objectSubId = 0;

		recordDependencyOn(&address, &refaddr, DEPENDENCY_NORMAL);
	}
}


/*
 * get_primary_key_attnos_from_query
 *
 * Identify the columns in base tables' primary keys in the target list.
 *
 * Returns a Bitmapset of the column attnos of the primary key's columns of
 * tables that used in the query.  The attnos are offset by
 * FirstLowInvalidHeapAttributeNumber as same as get_primary_key_attnos.
 *
 * If any table has no primary key or any primary key's columns is not in
 * the target list, return NULL.  We also return NULL if any pkey constraint
 * is deferrable.
 *
 * constraintList is set to a list of the OIDs of the pkey constraints.
 */
static Bitmapset *
get_primary_key_attnos_from_query(Query *query, List **constraintList, bool is_create)
{
	List *key_attnos_list = NIL;
	ListCell *lc;
	int i;
	Bitmapset *keys = NULL;
	Relids	rels_in_from;
	PlannerInfo root;


	/* This can recurse, so check for excessive recursion */
	check_stack_depth();

	/* convert CTEs to subqueries */
	query = copyObject(query);
	foreach (lc, query->cteList)
	{
		PlannerInfo root;
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->cterefcount == 0)
			continue;

		root.parse = query;
		inline_cte(&root, cte);
	}
	query->cteList = NIL;

	/*
	 * Collect primary key attributes from all tables used in query. The key attributes
	 * sets for each table are stored in key_attnos_list in order by RTE index.
	 */
	i = 1;
	foreach(lc, query->rtable)
	{
		RangeTblEntry *r = (RangeTblEntry*) lfirst(lc);
		Bitmapset *key_attnos;
		bool	has_pkey = true;
		Index	first_rtindex = is_create ? 1 : PRS2_NEW_VARNO + 1;

		/* skip NEW/OLD entries */
		if (i >= first_rtindex)
		{
			/* for subqueries, scan recursively */
			if (r->rtekind == RTE_SUBQUERY)
			{
				key_attnos = get_primary_key_attnos_from_query(r->subquery, constraintList, true);
				has_pkey = (key_attnos != NULL);
			}
			/* for tables, call get_primary_key_attnos */
			else if (r->rtekind == RTE_RELATION)
			{
				Oid constraintOid;
				key_attnos = get_primary_key_attnos(r->relid, false, &constraintOid);
				*constraintList = lappend_oid(*constraintList, constraintOid);
				has_pkey = (key_attnos != NULL);
			}
			/* for other RTEs, store NULL into key_attnos_list */
			else
				key_attnos = NULL;
		}
		else
			key_attnos = NULL;

		/*
		 * If any table or subquery has no primary key or its pkey constraint is deferrable,
		 * we cannot get key attributes for this query, so return NULL.
		 */
		if (!has_pkey)
			return NULL;

		key_attnos_list = lappend(key_attnos_list, key_attnos);
		i++;
	}

	/* Collect key attributes appearing in the target list */
	i = 1;
	foreach(lc, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) flatten_join_alias_vars(query, lfirst(lc));

		if (IsA(tle->expr, Var))
		{
			Var *var = (Var*) tle->expr;
			Bitmapset *attnos = list_nth(key_attnos_list, var->varno - 1);

			/* check if this attribute is from a base table's primary key */
			if (bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber, attnos))
			{
				/*
				 * Remove found key attributes from key_attnos_list, and add this
				 * to the result list.
				 */
				bms_del_member(attnos, var->varattno - FirstLowInvalidHeapAttributeNumber);
				keys = bms_add_member(keys, i - FirstLowInvalidHeapAttributeNumber);
			}
		}
		i++;
	}

	/* Collect relations appearing in the FROM clause */
	rels_in_from = pull_varnos_of_level(&root, (Node *)query->jointree, 0);

	/*
	 * Check if all key attributes of relations in FROM are appearing in the target
	 * list.  If an attribute remains in key_attnos_list in spite of the table is used
	 * in FROM clause, the target is missing this key attribute, so we return NULL.
	 */
	i = 1;
	foreach(lc, key_attnos_list)
	{
		Bitmapset *bms = (Bitmapset *)lfirst(lc);
		if (!bms_is_empty(bms) && bms_is_member(i, rels_in_from))
			return NULL;
		i++;
	}

	return keys;
}
