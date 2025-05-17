/*-------------------------------------------------------------------------
 *
 * dynamic_explain.c
 *	  Explain query plans during execution
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

/*
 * True while this backend is processing a log query plan request,
 * from the start of wrapping plan nodes until the log output is completed.
 */
static bool ProcessLogQueryPlanInterruptActive = false;

/* Currently executing query's QueryDesc */
static QueryDesc *ActiveQueryDesc = NULL;

static void WrapExecProcNodeWithExplain(PlanState *ps);
static void UnwrapExecProcNodeWithExplain(PlanState *ps);

/*
 * Handle receipt of an interrupt indicating logging the plan of the currently
 * running query.
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
 * Clear pg_log_query_plan() related state during (sub)transaction abort
 */
void
ResetLogQueryPlanState(void)
{
	ActiveQueryDesc = NULL;
	ProcessLogQueryPlanInterruptActive = false;
}

/*
 * Wrap array of PlanState ExecProcNodes with ExecProcNodeWithExplain
 */
static void
WrapPlanStatesWithExplain(PlanState **planstates, int nplans)
{
	int			i;

	for (i = 0; i < nplans; i++)
		WrapExecProcNodeWithExplain(planstates[i]);
}

/*
 * Wrap CustomScanState children's ExecProcNodes with ExecProcNodeWithExplain
 */
static void
WrapCustomPlanChildWithExplain(CustomScanState *css)
{
	ListCell   *cell;

	foreach(cell, css->custom_ps)
		WrapExecProcNodeWithExplain((PlanState *) lfirst(cell));
}

/*
 * Recursively wrap all possible ExecProcNode().
 *
 * Recursion is necessary because the next ExecProcNode() call may be invoked
 * not only through the current node ('ps' parameter), but also via lefttree,
 * righttree, subPlan, or other special child plans.
 */
static void
WrapExecProcNodeWithExplain(PlanState *ps)
{
	check_stack_depth();

	ExecSetExecProcNode(ps, ps->ExecProcNodeReal);

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
 * Unwrap array of PlanStates ExecProcNodes with ExecProcNodeWithExplain
 */
static void
UnwrapPlanStatesWithExplain(PlanState **planstates, int nplans)
{
	int			i;

	for (i = 0; i < nplans; i++)
		UnwrapExecProcNodeWithExplain(planstates[i]);
}

/*
 * Unwrap CustomScanState children's ExecProcNodes with ExecProcNodeWithExplain
 */
static void
UnwrapCustomPlanChildWithExplain(CustomScanState *css)
{
	ListCell   *cell;

	foreach(cell, css->custom_ps)
		UnwrapExecProcNodeWithExplain((PlanState *) lfirst(cell));
}

/*
 * Unwrap ExecProcNode with ExecProcNodeWithExplain recursively
 */
static void
UnwrapExecProcNodeWithExplain(PlanState *ps)
{
//	Assert(ps->ExecProcNodeOriginal != NULL);

	check_stack_depth();

	// ps->ExecProcNode = ps->ExecProcNodeOriginal;
	// ps->ExecProcNodeOriginal = NULL;

	if (ps->instrument && INSTR_TIME_IS_ZERO(ps->instrument->starttime))
		ps->ExecProcNode = ExecProcNodeInstr;
	else
		ps->ExecProcNode = ps->ExecProcNodeReal;

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
 * Wrapper for logging currently running plan.
 *
 * ExecProcNode wrapper that performs logging plan of the currently running query.
 */
TupleTableSlot *
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

	/*
	 * ActiveQueryDesc is valid only during standard_ExecutorRun(). However,
	 * ExecProcNode() can still be called afterward, such as ExecPostprocessPlan().
	 *  To handle the case, check ActiveQueryDesc.
	 */
	if (ActiveQueryDesc == NULL)
		ereport(LOG_SERVER_ONLY,
				errmsg("backend with PID %d is finishing query",
					   MyProcPid),
				errhidestmt(true),
				errhidecontext(true));
	else
	{
		ExplainStringAssemble(es, ActiveQueryDesc, es->format, 0, -1);

		ereport(LOG_SERVER_ONLY,
				errmsg("query plan running on backend with PID %d is:\n%s",
					   MyProcPid, es->str->data),
				errhidestmt(true),
				errhidecontext(true));
	}

	MemoryContextSwitchTo(old_cxt);
	MemoryContextDelete(cxt);

	/*
	 * Unwrap ExecProcNode(), or wrap it for instrumentation if needed.
	 * Since ExecProcNodeWithExplain() is wrapped ealier in ExecProcNodeFirst(),
	 * perform instrumentation wrapping here if requied.
	 */
	if (ps->instrument && INSTR_TIME_IS_ZERO(ps->instrument->starttime))
		ps->ExecProcNode = ExecProcNodeInstr;
	else
		ps->ExecProcNode = ps->ExecProcNodeReal;

	ProcessLogQueryPlanInterruptActive = false;

	return ps->ExecProcNode(ps);
}

/*
 * Perform logging plan for the currently running query.
 *
 * Since executing EXPLAIN-related code at an arbitrary CHECK_FOR_INTERRUPTS()
 * point is potentially unsafe, this function wraps the ExecProcNode() to log
 * the query plan. This ensures that EXPLAIN code is executed only during
 * ExecProcNode(), where it is considered safe.
 */
void
ProcessLogQueryPlanInterrupt(void)
{
	LogQueryPlanPending = false;

	/* Prevent re-entrance until the plan has been logged and the unwrapping has done */
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
}

bool
GetProcessLogQueryPlanInterruptActive(void)
{
	return ProcessLogQueryPlanInterruptActive;
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
 * Signal a backend process to log the query plan of the running query.
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
