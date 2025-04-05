/*-------------------------------------------------------------------------
 *
 * dynamic_explain.c
 *	  Explain query execution plans
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/dynamic_explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "commands/dynamic_explain.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/backend_status.h"

bool		ProcessLogQueryPlanInterruptActive = false;

/* Currently executing query's QueryDesc */
static QueryDesc *ActiveQueryDesc = NULL;

static TupleTableSlot *ExecProcNodeWithExplain(PlanState *ps);
static void WrapExecProcNodeWithExplain(PlanState *ps);
static void UnwrapExecProcNodeWithExplain(PlanState *ps);

/*
 * HandleLogQueryPlanInterrupt -
 *    Handle receipt of an interrupt indicating logging the plan of the currently
 *    running query.
 *
 * All the actual work is deferred to ProcessLogQueryPlanInterrupt(),
 * because we cannot safely emit a log message inside the signal handler.
 */
void
HandleLogQueryPlanInterrupt(void)
{
	InterruptPending = true;
	LogQueryPlanPending = true;
	/* latch will be set by procsignal_sigusr1_handler */
}

/*
 * ResetLogQueryPlanState -
 *   Clear pg_log_query_plan() related state during (sub)transaction abort
 */
void
ResetLogQueryPlanState(void)
{
	/*
	 * After abort, some elements of ActiveQueryDesc is freed. To avoid
	 * accessing them, reset ActiveQueryDesc here.
	 */
	ActiveQueryDesc = NULL;
	ProcessLogQueryPlanInterruptActive = false;
}

/*
 * WrapPlanStatesWithExplain -
 *	  Wrap array of PlanState ExecProcNodes with ExecProcNodeWithExplain
 */
static void
WrapPlanStatesWithExplain(PlanState **planstates, int nplans)
{
	int			i;

	for (i = 0; i < nplans; i++)
		WrapExecProcNodeWithExplain(planstates[i]);
}

/*
 * WrapCustomPlanChildWithExplain -
 *	  Wrap CustomScanstate children's ExecProcNodes with ExecProcNodeWithExplain
 */
static void
WrapCustomPlanChildWithExplain(CustomScanState *css)
{
	ListCell   *cell;

	foreach(cell, css->custom_ps)
		WrapExecProcNodeWithExplain((PlanState *) lfirst(cell));
}

/*
 * WrapExecProcNodeWithExplain -
 *	  Wrap ExecProcNode with ExecProcNodeWithExplain recursively
 */
static void
WrapExecProcNodeWithExplain(PlanState *ps)
{
	/* wrapping can be done only once */
	if (ps->ExecProcNodeOriginal != NULL)
		return;

	check_stack_depth();

	ps->ExecProcNodeOriginal = ps->ExecProcNode;
	ps->ExecProcNode = ExecProcNodeWithExplain;

	if (ps->lefttree != NULL)
		WrapExecProcNodeWithExplain(ps->lefttree);
	if (ps->righttree != NULL)
		WrapExecProcNodeWithExplain(ps->righttree);
	if (ps->subPlan != NULL)
	{
		ListCell   *l;

		foreach(l, ps->subPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			WrapExecProcNodeWithExplain(sstate->planstate);
		}
	}

	/* special child plans */
	switch (nodeTag(ps->plan))
	{
		case T_Append:
			WrapPlanStatesWithExplain(((AppendState *) ps)->appendplans,
											  ((AppendState *) ps)->as_nplans);
			break;
		case T_MergeAppend:
			WrapPlanStatesWithExplain(((MergeAppendState *) ps)->mergeplans,
											  ((MergeAppendState *) ps)->ms_nplans);
			break;
		case T_BitmapAnd:
			WrapPlanStatesWithExplain(((BitmapAndState *) ps)->bitmapplans,
											  ((BitmapAndState *) ps)->nplans);
			break;
		case T_BitmapOr:
			WrapPlanStatesWithExplain(((BitmapOrState *) ps)->bitmapplans,
											  ((BitmapOrState *) ps)->nplans);
			break;
		case T_SubqueryScan:
			WrapExecProcNodeWithExplain(((SubqueryScanState *) ps)->subplan);
			break;
		case T_CustomScan:
			WrapCustomPlanChildWithExplain((CustomScanState *) ps);
			break;
		default:
			break;
	}
}

/*
 * UnwrapPlanStatesWithExplain -
 *	  Unwrap array of PlanStates ExecProcNodes with ExecProcNodeWithExplain
 */
static void
UnwrapPlanStatesWithExplain(PlanState **planstates, int nplans)
{
	int			i;

	for (i = 0; i < nplans; i++)
		UnwrapExecProcNodeWithExplain(planstates[i]);
}

/*
 * UnwrapCustomPlanChildWithExplain -
 *	  Unwrap CustomScanstate children's ExecProcNodes with ExecProcNodeWithExplain
 */
static void
UnwrapCustomPlanChildWithExplain(CustomScanState *css)
{
	ListCell   *cell;

	foreach(cell, css->custom_ps)
		UnwrapExecProcNodeWithExplain((PlanState *) lfirst(cell));
}

/*
 * UnwrapExecProcNodeWithExplain -
 *	  Unwrap ExecProcNode with ExecProcNodeWithExplain recursively
 */
static void
UnwrapExecProcNodeWithExplain(PlanState *ps)
{
	Assert(ps->ExecProcNodeOriginal != NULL);

	check_stack_depth();

	ps->ExecProcNode = ps->ExecProcNodeOriginal;
	ps->ExecProcNodeOriginal = NULL;

	if (ps->lefttree != NULL)
		UnwrapExecProcNodeWithExplain(ps->lefttree);
	if (ps->righttree != NULL)
		UnwrapExecProcNodeWithExplain(ps->righttree);
	if (ps->subPlan != NULL)
	{
		ListCell   *l;

		foreach(l, ps->subPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			UnwrapExecProcNodeWithExplain(sstate->planstate);
		}
	}

	/* special child plans */
	switch (nodeTag(ps->plan))
	{
		case T_Append:
			UnwrapPlanStatesWithExplain(((AppendState *) ps)->appendplans,
												((AppendState *) ps)->as_nplans);
			break;
		case T_MergeAppend:
			UnwrapPlanStatesWithExplain(((MergeAppendState *) ps)->mergeplans,
												((MergeAppendState *) ps)->ms_nplans);
			break;
		case T_BitmapAnd:
			UnwrapPlanStatesWithExplain(((BitmapAndState *) ps)->bitmapplans,
												((BitmapAndState *) ps)->nplans);
			break;
		case T_BitmapOr:
			UnwrapPlanStatesWithExplain(((BitmapOrState *) ps)->bitmapplans,
												((BitmapOrState *) ps)->nplans);
			break;
		case T_SubqueryScan:
			UnwrapExecProcNodeWithExplain(((SubqueryScanState *) ps)->subplan);
			break;
		case T_CustomScan:
			UnwrapCustomPlanChildWithExplain((CustomScanState *) ps);
			break;
		default:
			break;
	}
}

/*
 * ExecProcNodeWithExplain -
 *	  Wrap ExecProcNode with codes which logs currently running plan
 */
static TupleTableSlot *
ExecProcNodeWithExplain(PlanState *ps)
{
	ExplainState *es;
	MemoryContext cxt;
	MemoryContext old_cxt;

	check_stack_depth();

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"log_query_plan temporary context",
								ALLOCSET_DEFAULT_SIZES);

	old_cxt = MemoryContextSwitchTo(cxt);

	es = NewExplainState();

	es->format = EXPLAIN_FORMAT_TEXT;
	es->settings = true;
	es->verbose = true;
	es->signaled = true;

	ExplainStringAssemble(es, ActiveQueryDesc, es->format, 0, -1);

	ereport(LOG_SERVER_ONLY,
			errmsg("query plan running on backend with PID %d is:\n%s",
				   MyProcPid, es->str->data),
			errhidestmt(true),
			errhidecontext(true));

	MemoryContextSwitchTo(old_cxt);
	MemoryContextDelete(cxt);

	UnwrapExecProcNodeWithExplain(ActiveQueryDesc->planstate);

	/*
	 * Since unwrapping has already done, call ExecProcNode() not
	 * ExecProcNodeOriginal().
	 */
	return ps->ExecProcNode(ps);
}

/*
 * ProcessLogQueryPlanInterrupt
 *	  Add wrapper which logs explain of the plan to ExecProcNodes
 *
 * Since running EXPLAIN codes at any arbitrary CHECK_FOR_INTERRUPTS() is
 * unsafe, this function just wraps every ExecProcNode.
 * In this way, EXPLAIN code is only executed at the timing of ExecProcNode,
 * which seems safe.
 */
void
ProcessLogQueryPlanInterrupt(void)
{
	LogQueryPlanPending = false;

	/* Cannot re-enter */
	if (ProcessLogQueryPlanInterruptActive)
		return;

	ProcessLogQueryPlanInterruptActive = true;

	if (ActiveQueryDesc == NULL)
	{
		ereport(LOG_SERVER_ONLY,
				errmsg("backend with PID %d is not running a query or a subtransaction is aborted",
					   MyProcPid),
				errhidestmt(true),
				errhidecontext(true));

		ProcessLogQueryPlanInterruptActive = false;
		return;
	}

	WrapExecProcNodeWithExplain(ActiveQueryDesc->planstate);
	ProcessLogQueryPlanInterruptActive = false;
}

QueryDesc *
GetActiveQueryDesc(void)
{
	return ActiveQueryDesc;
}

void
SetActiveQueryDesc(QueryDesc *queryDesc)
{
	ActiveQueryDesc = queryDesc;
}

/*
 * pg_log_query_plan
 *    Signal a backend process to log the query plan of the running query.
 *
 * By default, only superusers are allowed to signal to log the plan because
 * allowing any users to issue this request at an unbounded rate would
 * cause lots of log messages and which can lead to denial of service.
 * Additional roles can be permitted with GRANT.
 */
Datum
pg_log_query_plan(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	PGPROC	   *proc;
	PgBackendStatus *be_status;

	proc = BackendPidGetProc(pid);

	if (proc == NULL)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process", pid)));
		PG_RETURN_BOOL(false);
	}

	be_status = pgstat_get_beentry_by_proc_number(proc->vxid.procNumber);
	if (be_status->st_backendType != B_BACKEND)
	{
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL client backend process", pid)));
		PG_RETURN_BOOL(false);
	}

	if (SendProcSignal(pid, PROCSIG_LOG_QUERY_PLAN, proc->vxid.procNumber) < 0)
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}
