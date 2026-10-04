/*-------------------------------------------------------------------------
 *
 * explain_running.c
 *	  Explain query plans during execution
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_running.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "common/pg_prng.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_running.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/backend_status.h"
#include "utils/injection_point.h"

typedef uint64 LogQueryPlanRequestId;

typedef enum LogQueryPlanRequestState
{
	LOG_QUERY_PLAN_REQUEST_IDLE = 0,
	LOG_QUERY_PLAN_REQUEST_PENDING,
	LOG_QUERY_PLAN_REQUEST_PROCESSING,
} LogQueryPlanRequestState;

typedef struct LogQueryPlanRequest
{
	LogQueryPlanRequestId request_id;
	pid_t		target_pid;
	/* Future versions may add fields here to carry EXPLAIN options. */
} LogQueryPlanRequest;

typedef struct LogQueryPlanRequestSlot
{
	/* All fields below are protected by mutex. */
	slock_t		mutex;
	LogQueryPlanRequestState state;
	LogQueryPlanRequest request;
} LogQueryPlanRequestSlot;

static LogQueryPlanRequestSlot *LogQueryPlanRequestSlots;

static void LogQueryPlanShmemRequest(void *arg);
static void LogQueryPlanShmemInit(void *arg);
static void LogQueryPlanRequestSlotCleanup(int code, Datum arg);

const ShmemCallbacks LogQueryPlanShmemCallbacks = {
	.request_fn = LogQueryPlanShmemRequest,
	.init_fn = LogQueryPlanShmemInit,
};

/* Is plan node wrapping for query plan logging currently in progress? */
static bool WrapNodesInProgress = false;

/* Request being processed by this backend. */
static bool LogQueryPlanRequestActive = false;
static LogQueryPlanRequest ActiveLogQueryPlanRequest;

static bool wrapExecProcNode(PlanState *ps, void *context);
static LogQueryPlanRequestId GenerateLogQueryPlanRequestId(void);
static bool ReserveLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
									   LogQueryPlanRequestId request_id);
static bool RollbackLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
										LogQueryPlanRequestId request_id);
static bool TakeLogQueryPlanRequest(void);
static void FinishLogQueryPlanRequest(void);

static void
LogQueryPlanShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Log Query Plan Request Slots",
					   .size = mul_size(sizeof(LogQueryPlanRequestSlot), MaxBackends),
					   .ptr = (void **) &LogQueryPlanRequestSlots,
		);
}

static void
LogQueryPlanShmemInit(void *arg)
{
	for (int i = 0; i < MaxBackends; i++)
	{
		LogQueryPlanRequestSlot *slot = &LogQueryPlanRequestSlots[i];

		SpinLockInit(&slot->mutex);
		slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
		MemSet(&slot->request, 0, sizeof(slot->request));
	}
}

/*
 * Initialize the request slot assigned to a newly-started process.
 *
 * This is called before InitProcessPhase2() publishes the process in the
 * ProcArray, so no other backend can yet install a query plan request for
 * this process.
 */
void
LogQueryPlanRequestSlotInit(void)
{
	LogQueryPlanRequestSlot *slot;
	pid_t		previous_target_pid = 0;

	Assert(MyBackendType == B_BACKEND);
	slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	if (slot->state != LOG_QUERY_PLAN_REQUEST_IDLE && slot->request.target_pid != MyProcPid)
		previous_target_pid = slot->request.target_pid;
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	MemSet(&slot->request, 0, sizeof(slot->request));
	SpinLockRelease(&slot->mutex);

	/*
	 * Register before InitProcessPhase2() registers RemoveProcFromArray, so
	 * exit callbacks remove us from the ProcArray before releasing this slot,
	 * and release the slot before ProcKill makes our PGPROC reusable.
	 */
	on_shmem_exit(LogQueryPlanRequestSlotCleanup, Int32GetDatum(MyProcNumber));

	if (previous_target_pid != 0)
		elog(LOG, "process %d taking over query plan request slot %d owned by process %d",
			 MyProcPid, MyProcNumber, (int) previous_target_pid);
}

/*
 * Release a request slot when its process exits.
 *
 * Do not throw an error here, since this is called during process exit.
 */
static void
LogQueryPlanRequestSlotCleanup(int code, Datum arg)
{
	ProcNumber	proc_number = DatumGetInt32(arg);
	LogQueryPlanRequestSlot *slot;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	MemSet(&slot->request, 0, sizeof(slot->request));
	SpinLockRelease(&slot->mutex);
}

/*
 * Generate an opaque request identifier.
 *
 * The identifier allocation policy is intentionally separate from the IPC
 * mechanism and can be replaced without changing request transport.
 */
static LogQueryPlanRequestId
GenerateLogQueryPlanRequestId(void)
{
	/* Keep the identifier positive and representable as SQL bigint. */
	return pg_prng_uint64_range(&pg_global_prng_state, 1, PG_INT64_MAX);
}

static bool
ReserveLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
						   LogQueryPlanRequestId request_id)
{
	LogQueryPlanRequestSlot *slot;
	bool		reserved = false;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_IDLE)
	{
		slot->request.request_id = request_id;
		slot->request.target_pid = pid;
		slot->state = LOG_QUERY_PLAN_REQUEST_PENDING;
		reserved = true;
	}
	SpinLockRelease(&slot->mutex);

	return reserved;
}

static bool
RollbackLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
							LogQueryPlanRequestId request_id)
{
	LogQueryPlanRequestSlot *slot;
	bool		rolled_back = false;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PENDING &&
		slot->request.target_pid == pid &&
		slot->request.request_id == request_id)
	{
		slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
		MemSet(&slot->request, 0, sizeof(slot->request));
		rolled_back = true;
	}
	SpinLockRelease(&slot->mutex);

	return rolled_back;
}

static bool
TakeLogQueryPlanRequest(void)
{
	LogQueryPlanRequestSlot *slot;
	bool		found = false;

	Assert(!LogQueryPlanRequestActive);
	slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PENDING &&
		slot->request.target_pid == MyProcPid)
	{
		ActiveLogQueryPlanRequest = slot->request;
		LogQueryPlanRequestActive = true;
		slot->state = LOG_QUERY_PLAN_REQUEST_PROCESSING;
		found = true;
	}
	SpinLockRelease(&slot->mutex);

	return found;
}

static void
FinishLogQueryPlanRequest(void)
{
	LogQueryPlanRequestSlot *slot;

	if (!LogQueryPlanRequestActive)
		return;

	Assert(MyProcNumber >= 0 && MyProcNumber < MaxBackends);
	slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PROCESSING &&
		slot->request.target_pid == MyProcPid &&
		slot->request.request_id == ActiveLogQueryPlanRequest.request_id)
	{
		slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
		MemSet(&slot->request, 0, sizeof(slot->request));
	}
	SpinLockRelease(&slot->mutex);

	LogQueryPlanRequestActive = false;
	MemSet(&ActiveLogQueryPlanRequest, 0,
		   sizeof(ActiveLogQueryPlanRequest));
}

/*
 * Wrap all the underlying ExecProcNode with ExecProcNodeFirst.
 */
static bool
wrapExecProcNode(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;

	ExecSetExecProcNode(ps, ps->ExecProcNodeReal);

	return planstate_tree_walker(ps, wrapExecProcNode, context);
}

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
 * Actual plan logging function.
 */
void
LogQueryPlan(void)
{
	ExplainState *es;
	MemoryContext cxt;
	MemoryContext old_cxt;
	QueryDesc  *queryDesc;

	/*
	 * Current QueryDesc is valid only during standard_ExecutorRun. However,
	 * ExecProcNode can be called afterward (i.e., ExecPostprocessPlan). To
	 * handle the case, check whether we have QueryDesc now.
	 */
	queryDesc = GetCurrentQueryDesc();

	if (queryDesc == NULL)
	{
		LogQueryPlanNoOpportunity(0);
		return;
	}

	Assert(LogQueryPlanRequestActive);

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"log_query_plan temporary context",
								ALLOCSET_DEFAULT_SIZES);
	old_cxt = MemoryContextSwitchTo(cxt);

	es = NewExplainState();
	es->running = true;

	PG_TRY();
	{
		ExplainStringAssemble(es, queryDesc, es->format, false, -1);

		ereport(LOG_SERVER_ONLY,
				errmsg("query and its plan for queryid " INT64_FORMAT " requested by request ID " UINT64_FORMAT " running on backend with PID %d are:\n%s",
					   queryDesc->plannedstmt->queryId,
					   ActiveLogQueryPlanRequest.request_id, MyProcPid,
					   es->str->data));
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(cxt);
		FinishLogQueryPlanRequest();
		LogQueryPlanPending = false;
	}
	PG_END_TRY();
}

/*
 * Log that an active request could not be completed, and release its slot.
 */
void
LogQueryPlanNoOpportunity(uint64 query_id)
{
	if (!LogQueryPlanRequestActive && !TakeLogQueryPlanRequest())
	{
		LogQueryPlanPending = false;
		return;
	}

	if (query_id != 0)
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging request " UINT64_FORMAT " could not be completed because there was no opportunity to log the plan for queryid " INT64_FORMAT,
					   ActiveLogQueryPlanRequest.request_id, query_id));
	else
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging request " UINT64_FORMAT " could not be completed because there was no opportunity to log the plan",
					   ActiveLogQueryPlanRequest.request_id));

	FinishLogQueryPlanRequest();
	LogQueryPlanPending = false;
}

/*
 * Process the request for logging query plan at CHECK_FOR_INTERRUPTS().
 *
 * Since executing EXPLAIN-related code at an arbitrary CHECK_FOR_INTERRUPTS()
 * point is potentially unsafe, this function just wraps the nodes of
 * ExecProcNode with ExecProcNodeFirst, which logs query plan if requested.
 * This way ensures that EXPLAIN-related code is executed only during
 * ExecProcNodeFirst, where it is considered safe.
 */
void
ProcessLogQueryPlanInterrupt(void)
{
	QueryDesc  *querydesc;
	bool		wrapped = false;

	/*
	 * Exit immediately if wrapping the plan or processing a request is already
	 * in progress.  Keep LogQueryPlanPending set until the active request is
	 * completed, without trying to take another request at subsequent interrupt
	 * checks.
	 */
	if (WrapNodesInProgress || LogQueryPlanRequestActive)
		return;

	if (!TakeLogQueryPlanRequest())
	{
		LogQueryPlanPending = false;
		return;
	}

	querydesc = GetCurrentQueryDesc();

	/* If current query has already finished, we can do nothing but exit */
	if (querydesc == NULL)
	{
		LogQueryPlanNoOpportunity(0);
		return;
	}

	WrapNodesInProgress = true;

	PG_TRY();
	{
		INJECTION_POINT("log-query-interrupt", NULL);

		/*
		 * Wrap ExecProcNodes with ExecProcNodeFirst, which logs query plan
		 * when LogQueryPlanPending is true.
		 */
		(void) wrapExecProcNode(querydesc->planstate, NULL);
		wrapped = true;
	}
	PG_FINALLY();
	{
		WrapNodesInProgress = false;
		if (!wrapped)
		{
			FinishLogQueryPlanRequest();
			LogQueryPlanPending = false;
		}
	}
	PG_END_TRY();
}

/*
 * Signal a backend process to log the query plan of the running query.
 *
 * By default, only superusers are allowed to signal a backend to log its
 * plan because the output is written to the server log, which in many cases
 * can only be read by superusers.
 * Additional roles can be permitted with GRANT.
 */
Datum
pg_log_query_plan(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	PGPROC	   *proc;
	ProcNumber	procNumber;
	PgBackendStatus *be_status = NULL;
	LogQueryPlanRequestId request_id;

	proc = BackendPidGetProc(pid);

	if (proc != NULL)
		be_status = pgstat_get_beentry_by_proc_number(proc->vxid.procNumber);

	if (proc == NULL || be_status == NULL || be_status->st_procpid != pid)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process", pid)));
		PG_RETURN_NULL();
	}

	if (be_status->st_backendType != B_BACKEND)
	{
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL client backend process", pid)));
		PG_RETURN_NULL();
	}

	procNumber = GetNumberFromPGProc(proc);
	request_id = GenerateLogQueryPlanRequestId();

	if (!ReserveLogQueryPlanRequest(procNumber, pid, request_id))
	{
		ereport(WARNING,
				(errmsg("query plan request for PID %d is already pending", pid)));
		PG_RETURN_NULL();
	}

	INJECTION_POINT("log-query-plan-before-signal", NULL);

	if (SendProcSignal(pid, PROCSIG_LOG_QUERY_PLAN, procNumber) < 0)
	{
		/* The signal was not delivered, so undo our reservation if possible. */
		(void) RollbackLogQueryPlanRequest(procNumber, pid, request_id);
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_NULL();
	}

	PG_RETURN_INT64((int64) request_id);
}
