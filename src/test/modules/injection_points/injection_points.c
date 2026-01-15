/*--------------------------------------------------------------------------
 *
 * injection_points.c
 *		Code for testing injection points.
 *
 * Injection points are able to trigger user-defined callbacks in pre-defined
 * code paths.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_points.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* Maximum number of waits usable in injection points at once */
#define INJ_MAX_WAIT	8
#define INJ_NAME_MAXLEN	64
#define INJ_MAX_STATS	128

/*
 * Conditions related to injection points.  This tracks in shared memory the
 * runtime conditions under which an injection point is allowed to run,
 * stored as private_data when an injection point is attached, and passed as
 * argument to the callback.
 *
 * If more types of runtime conditions need to be expanded, this structure
 * should be expanded.
 */
typedef enum InjectionPointConditionType
{
	INJ_CONDITION_ALWAYS = 0,	/* always run */
	INJ_CONDITION_PID,			/* PID restriction */
} InjectionPointConditionType;

typedef struct InjectionPointCondition
{
	/* Type of the condition */
	InjectionPointConditionType type;

	/* ID of the process where the injection point is allowed to run */
	int			pid;
} InjectionPointCondition;

/*
 * List of injection points stored in TopMemoryContext attached
 * locally to this process.
 */
static List *inj_list_local = NIL;

/*
 * Simple statistics for injection points (numcalls).
 */
typedef struct InjectionPointStat
{
	char		name[INJ_NAME_MAXLEN];
	pg_atomic_uint64 numcalls;
} InjectionPointStat;

/*
 * Shared state information for injection points.
 */
typedef struct InjectionPointSharedState
{
	/* Protects access to other fields */
	slock_t		lock;

	/* Counters advancing when injection_points_wakeup() is called */
	uint32		wait_counts[INJ_MAX_WAIT];

	/* Names of injection points attached to wait counters */
	char		name[INJ_MAX_WAIT][INJ_NAME_MAXLEN];

	/* Condition variable used for waits and wakeups */
	ConditionVariable wait_point;

	/* Statistics */
	InjectionPointStat stats[INJ_MAX_STATS];
} InjectionPointSharedState;

/* Pointer to shared-memory state. */
static InjectionPointSharedState *inj_state = NULL;

extern PGDLLEXPORT void injection_error(const char *name,
										const void *private_data,
										void *arg);
extern PGDLLEXPORT void injection_notice(const char *name,
										 const void *private_data,
										 void *arg);
extern PGDLLEXPORT void injection_wait(const char *name,
									   const void *private_data,
									   void *arg);

/* track if injection points attached in this process are linked to it */
static bool injection_point_local = false;

/*
 * GUC variable
 *
 * This GUC is useful to control if statistics should be enabled or not
 * during a test with injection points, like for example if a test relies
 * on a callback run in a critical section where no allocation should happen.
 */
bool		inj_stats_enabled = false;

/* Shared memory init callbacks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/*
 * Routine for shared memory area initialization.
 */
static void
injection_point_init_state(void *ptr)
{
	InjectionPointSharedState *state = (InjectionPointSharedState *) ptr;

	SpinLockInit(&state->lock);
	memset(state->wait_counts, 0, sizeof(state->wait_counts));
	memset(state->name, 0, sizeof(state->name));
	ConditionVariableInit(&state->wait_point);
	
	/* Initialize stats */
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		state->stats[i].name[0] = '\0';
		pg_atomic_init_u64(&state->stats[i].numcalls, 0);
	}
}

/* Shared memory initialization when loading module */
static void
injection_shmem_request(void)
{
	Size		size;

	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	size = MAXALIGN(sizeof(InjectionPointSharedState));
	RequestAddinShmemSpace(size);
}

static void
injection_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	inj_state = ShmemInitStruct("injection_points",
								sizeof(InjectionPointSharedState),
								&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.
		 */
		injection_point_init_state(inj_state);
	}

	LWLockRelease(AddinShmemInitLock);
}

static void
injection_init_shmem(void)
{
	if (inj_state != NULL)
		return;

	ereport(ERROR,
			(errmsg("injection_points must be loaded via shared_preload_libraries")));
}

/*
 * Check runtime conditions associated to an injection point.
 *
 * Returns true if the named injection point is allowed to run, and false
 * otherwise.
 */
static bool
injection_point_allowed(InjectionPointCondition *condition)
{
	bool		result = true;

	switch (condition->type)
	{
		case INJ_CONDITION_PID:
			if (MyProcPid != condition->pid)
				result = false;
			break;
		case INJ_CONDITION_ALWAYS:
			break;
	}

	return result;
}

/*
 * Local statistics helpers
 */
static void
pgstat_create_inj(const char *name)
{
	if (!inj_stats_enabled || inj_state == NULL)
		return;

	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		if (inj_state->stats[i].name[0] == '\0')
		{
			strlcpy(inj_state->stats[i].name, name, INJ_NAME_MAXLEN);
			pg_atomic_write_u64(&inj_state->stats[i].numcalls, 0);
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);
}

static void
pgstat_drop_inj(const char *name)
{
	if (!inj_stats_enabled || inj_state == NULL)
		return;

	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		if (strcmp(inj_state->stats[i].name, name) == 0)
		{
			inj_state->stats[i].name[0] = '\0';
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);
}

static void
pgstat_report_inj(const char *name)
{
	int index = -1;

	if (!inj_stats_enabled || inj_state == NULL)
		return;

	/* 
	 * We don't hold the lock while updating the counter for better concurrency.
	 */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		if (strcmp(inj_state->stats[i].name, name) == 0)
		{
			index = i;
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index >= 0)
		pg_atomic_fetch_add_u64(&inj_state->stats[index].numcalls, 1);
}

/* Stub for fixed stats report */
static void
pgstat_report_inj_fixed(uint32 numattach, uint32 numdetach, uint32 numrun, uint32 numcached, uint32 numloaded)
{
	(void) numattach;
	(void) numdetach;
	(void) numrun;
	(void) numcached;
	(void) numloaded;
}


/*
 * before_shmem_exit callback to remove injection points linked to a
 * specific process.
 */
static void
injection_points_cleanup(int code, Datum arg)
{
	ListCell   *lc;

	/* Leave if nothing is tracked locally */
	if (!injection_point_local)
		return;

	/* Detach all the local points */
	foreach(lc, inj_list_local)
	{
		char	   *name = strVal(lfirst(lc));

		(void) InjectionPointDetach(name);

		/* Remove stats entry */
		pgstat_drop_inj(name);
	}
}

/* Set of callbacks available to be attached to an injection point. */
void
injection_error(const char *name, const void *private_data, void *arg)
{
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;
	char	   *argstr = (char *) arg;

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

	if (argstr)
		elog(ERROR, "error triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(ERROR, "error triggered for injection point %s", name);
}

void
injection_notice(const char *name, const void *private_data, void *arg)
{
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;
	char	   *argstr = (char *) arg;

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

	if (argstr)
		elog(NOTICE, "notice triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(NOTICE, "notice triggered for injection point %s", name);
}

/* Wait on a condition variable, awaken by injection_points_wakeup() */
void
injection_wait(const char *name, const void *private_data, void *arg)
{
	uint32		old_wait_counts = 0;
	int			index = -1;
	uint32		injection_wait_event = 0;
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;

	if (inj_state == NULL)
		injection_init_shmem();

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

	/*
	 * Use PG_WAIT_EXTENSION for custom wait event.
	 */
	injection_wait_event = PG_WAIT_EXTENSION;

	/*
	 * Find a free slot to wait for, and register this injection point's name.
	 */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (inj_state->name[i][0] == '\0')
		{
			index = i;
			strlcpy(inj_state->name[i], name, INJ_NAME_MAXLEN);
			old_wait_counts = inj_state->wait_counts[i];
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index < 0)
		elog(ERROR, "could not find free slot for wait of injection point %s ",
			 name);

	/* And sleep.. */
	ConditionVariablePrepareToSleep(&inj_state->wait_point);
    PG_TRY();
    {
        for (;;)
        {
            uint32      new_wait_counts;

            SpinLockAcquire(&inj_state->lock);
            new_wait_counts = inj_state->wait_counts[index];
            SpinLockRelease(&inj_state->lock);

            if (old_wait_counts != new_wait_counts)
                break;

            /*
             * If the user sends a cancel signal (Ctrl+C), we generally want to
             * stop waiting, even if interrupts are technically held off (e.g.
             * by a buffer lock).  We check QueryCancelPending manually here
             * to allow breaking out.  We must clear the flag so that the
             * standard interrupt handler doesn't throw an error later.
             */
            if (QueryCancelPending)
            {
                QueryCancelPending = false;
                break;
            }

            /*
             * We use WaitLatch directly instead of ConditionVariableSleep() because
             * ConditionVariableSleep() calls CHECK_FOR_INTERRUPTS(), which usually
             * does nothing if we are holding a lock (WaitEventInjectionPoint),
             * causing the process to hang and ignore our manual check above.
             * By using WaitLatch, we ensure we return on any signal (including
             * cancel) to re-evaluate our loop condition.
             */
            WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
                      injection_wait_event);
            ResetLatch(MyLatch);
        }
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        if (geterrcode() == ERRCODE_QUERY_CANCELED)
        {
            FlushErrorState();
        }
        else
        {
            PG_RE_THROW();
        }
    }
    PG_END_TRY();
	ConditionVariableCancelSleep();

	/* Remove this injection point from the waiters. */
	SpinLockAcquire(&inj_state->lock);
	inj_state->name[index][0] = '\0';
	SpinLockRelease(&inj_state->lock);
}

/*
 * SQL function for creating an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_attach);
Datum
injection_points_attach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *function;
	InjectionPointCondition condition = {0};

	if (strcmp(action, "error") == 0)
		function = "injection_error";
	else if (strcmp(action, "notice") == 0)
		function = "injection_notice";
	else if (strcmp(action, "wait") == 0)
		function = "injection_wait";
	else
		elog(ERROR, "incorrect action \"%s\" for injection point creation", action);

	if (injection_point_local)
	{
		condition.type = INJ_CONDITION_PID;
		condition.pid = MyProcPid;
	}

	pgstat_report_inj_fixed(1, 0, 0, 0, 0);
	InjectionPointAttach(name, "injection_points", function, &condition,
						 sizeof(InjectionPointCondition));

	if (injection_point_local)
	{
		MemoryContext oldctx;

		/* Local injection point, so track it for automated cleanup */
		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		inj_list_local = lappend(inj_list_local, makeString(pstrdup(name)));
		MemoryContextSwitchTo(oldctx);
	}

	/* Add entry for stats */
	pgstat_create_inj(name);

	PG_RETURN_VOID();
}

/*
 * SQL function for loading an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_load);
Datum
injection_points_load(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (inj_state == NULL)
		injection_init_shmem();

	pgstat_report_inj_fixed(0, 0, 0, 0, 1);
	INJECTION_POINT_LOAD(name);

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_run);
Datum
injection_points_run(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *arg = NULL;

	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		arg = text_to_cstring(PG_GETARG_TEXT_PP(1));

	pgstat_report_inj_fixed(0, 0, 1, 0, 0);
	INJECTION_POINT(name, arg);

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point from cache.
 */
PG_FUNCTION_INFO_V1(injection_points_cached);
Datum
injection_points_cached(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *arg = NULL;

	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		arg = text_to_cstring(PG_GETARG_TEXT_PP(1));

	pgstat_report_inj_fixed(0, 0, 0, 1, 0);
	INJECTION_POINT_CACHED(name, arg);

	PG_RETURN_VOID();
}

/*
 * SQL function for waking up an injection point waiting in injection_wait().
 */
PG_FUNCTION_INFO_V1(injection_points_wakeup);
Datum
injection_points_wakeup(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			index = -1;

	if (inj_state == NULL)
		injection_init_shmem();

	/* First bump the wait counter for the injection point to wake up */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (strcmp(name, inj_state->name[i]) == 0)
		{
			index = i;
			break;
		}
	}
	if (index < 0)
	{
		SpinLockRelease(&inj_state->lock);
		elog(ERROR, "could not find injection point %s to wake up", name);
	}
	inj_state->wait_counts[index]++;
	SpinLockRelease(&inj_state->lock);

	/* And broadcast the change to the waiters */
	ConditionVariableBroadcast(&inj_state->wait_point);
	PG_RETURN_VOID();
}

/*
 * injection_points_set_local
 *
 * Track if any injection point created in this process ought to run only
 * in this process.  Such injection points are detached automatically when
 * this process exits.  This is useful to make test suites concurrent-safe.
 */
PG_FUNCTION_INFO_V1(injection_points_set_local);
Datum
injection_points_set_local(PG_FUNCTION_ARGS)
{
	/* Enable flag to add a runtime condition based on this process ID */
	injection_point_local = true;

	if (inj_state == NULL)
		injection_init_shmem();

	/*
	 * Register a before_shmem_exit callback to remove any injection points
	 * linked to this process.
	 */
	before_shmem_exit(injection_points_cleanup, (Datum) 0);

	PG_RETURN_VOID();
}

/*
 * SQL function for dropping an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_detach);
Datum
injection_points_detach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	pgstat_report_inj_fixed(0, 1, 0, 0, 0);
	if (!InjectionPointDetach(name))
		elog(ERROR, "could not detach injection point \"%s\"", name);

	/* Remove point from local list, if required */
	if (inj_list_local != NIL)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		inj_list_local = list_delete(inj_list_local, makeString(name));
		MemoryContextSwitchTo(oldctx);
	}

	/* Remove stats entry */
	pgstat_drop_inj(name);

	PG_RETURN_VOID();
}

/*
 * SQL function returning the number of times an injection point
 * has been called.
 */
PG_FUNCTION_INFO_V1(injection_points_stats_numcalls);
Datum
injection_points_stats_numcalls(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	uint64		numcalls = 0;
	int			index = -1;

	if (inj_state == NULL)
		PG_RETURN_NULL();

	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		if (strcmp(inj_state->stats[i].name, name) == 0)
		{
			index = i;
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index >= 0)
	{
		numcalls = pg_atomic_read_u64(&inj_state->stats[index].numcalls);
		PG_RETURN_INT64(numcalls);
	}
	
	PG_RETURN_NULL();
}

/*
 * SQL function that drops all injection point statistics.
 */
PG_FUNCTION_INFO_V1(injection_points_stats_drop);
Datum
injection_points_stats_drop(PG_FUNCTION_ARGS)
{
	if (inj_state == NULL)
		PG_RETURN_VOID();

	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_STATS; i++)
	{
		inj_state->stats[i].name[0] = '\0';
		pg_atomic_write_u64(&inj_state->stats[i].numcalls, 0);
	}
	SpinLockRelease(&inj_state->lock);

	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomBoolVariable("injection_points.stats",
							 "Enables statistics for injection points.",
							 NULL,
							 &inj_stats_enabled,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("injection_points");

	/* Shared memory initialization */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = injection_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = injection_shmem_startup;
}
