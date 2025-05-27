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

/* Currently executing query's QueryDesc */
static QueryDesc *ActiveQueryDesc = NULL;

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
 * Clear pg_log_query_plan() related state during (sub)transaction abort.
 */
void
ResetLogQueryPlanState(void)
{
	ActiveQueryDesc = NULL;
	LogQueryPlanPending = false;
}

/*
 * Actual plan logging function.
 */
void
LogQueryPlan(void)
{
	ExplainState *es;
	MemoryContext cxt;
	MemoryContext old_cxt;

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
	 * ExecProcNode can be called afterward(i.e., ExecPostprocessPlan) To
	 * handle the case, check ActiveQueryDesc.
	 */
	if (ActiveQueryDesc == NULL)
	{
		LogQueryPlanPending = false;

		ereport(LOG_SERVER_ONLY,
				errmsg("backend with PID %d is finishing query",
					   MyProcPid),
				errhidestmt(true),
				errhidecontext(true));
	}

	ExplainStringAssemble(es, ActiveQueryDesc, es->format, 0, -1);

	ereport(LOG_SERVER_ONLY,
			errmsg("query plan running on backend with PID %d is:\n%s",
				   MyProcPid, es->str->data),
			errhidestmt(true),
			errhidecontext(true));

	MemoryContextSwitchTo(old_cxt);
	MemoryContextDelete(cxt);

	LogQueryPlanPending = false;
}

/*
 * Process the request for logging current query plan.
 *
 * Since executing EXPLAIN-related code at an arbitrary CHECK_FOR_INTERRUPTS()
 * point is potentially unsafe, this function just wraps the nodes of
 * ExecProcNode with ExecProcNodeFirst, which logs corrent query plan if
 * requested. This way ensures that EXPLAIN code is executed only during
 * ExecProcNodeFirst, where it is considered safe.
 */
void
ProcessLogQueryPlanInterrupt(void)
{
	if (ActiveQueryDesc == NULL)
	{
		ereport(LOG_SERVER_ONLY,
				errmsg("backend with PID %d is not running a query or a subtransaction is aborted",
					   MyProcPid),
				errhidestmt(true),
				errhidecontext(true));

		LogQueryPlanPending = false;
		return;
	}

	/* Wrap ExecProcNode */
	ExecSetExecProcNodeRecurse(ActiveQueryDesc->planstate);
}

/*
 * Returns ActiveQueryDesc.
 */
QueryDesc *
GetActiveQueryDesc(void)
{
	return ActiveQueryDesc;
}

/*
 Set ActiveQueryDesc to queryDesc.
 */
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
