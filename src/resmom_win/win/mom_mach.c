/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *  
 * This file is part of the PBS Professional ("PBS Pro") software.
 * 
 * Open Source License Information:
 *  
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free 
 * Software Foundation, either version 3 of the License, or (at your option) any 
 * later version.
 *  
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */
#include <time.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <mmsystem.h>
#include <ntsecapi.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <wtsapi32.h>

#include "pbs_error.h"
#include "list_link.h"
#include "server_limits.h"
#include "attribute.h"
#include "resource.h"
#include "job.h"
#include "log.h"
#include "mom_mach.h"
#include "resmon.h"
#include "rm_dep.h"

/*
 **	System dependent code to gather information for the resource
 **	monitor for a WIN2000 machine
 **
 **	Resources known by this code:
 **		cput		cpu time for a job
 **		ncpus		number of cpus
 **		physmem		physical memory size in KB
 **		size		size of a file or filesystem in KB
 */




typedef  NTSTATUS(NTAPI	*NtOpenThread_t)
(HANDLE *,		/* resulting thread object handle */
	ACCESS_MASK,		/* desired access */
	DWORD *, 		/* object's attributes */
	DWORD *		/* (proc-ID, thread-id) */
	);

#ifndef TRUE
#define FALSE	0
#define TRUE	1
#endif	/* TRUE */

#ifndef	MAX
#define MAX(a, b) (((a)>(b))?(a):(b))
#endif	/* MAX */

/*
 ** external functions and data
 */
extern	int	nice_val;
extern	struct	config		*search(struct config *, char *);
extern	struct	rm_attribute	*momgetattr(char *);
extern	int			rm_errno;
extern	unsigned	int	reqnum;
extern	double	cputfactor;
extern	double	wallfactor;
extern  int	nrun_factor;
extern	int	suspend_signal, resume_signal;
extern	pbs_list_head	svr_alljobs;	/* all jobs under MOM's control */

/*
 ** local functions and data
 */
char	*physmem	(struct rm_attribute *attrib);
char	*ncpus		(struct rm_attribute *attrib);
extern char	*loadave	(struct rm_attribute *attrib);

extern char	*nullproc	(struct rm_attribute *attrib);

extern	char	*ret_string;
time_t		wait_time = 10;
DWORD		last_time = 0;

extern	struct	pbs_err_to_txt	pbs_err_to_txt[];
extern	time_t			time_now;
extern  int     num_oscpus;
extern  int     num_acpus;
extern  int     num_pcpus;

extern	char	extra_parm[];
extern	char	no_parm[];
char		no_count[] = "count not found";
static	long	page_size;
static	u_Long	pmem_size;
static  unsigned int load = 0;

int             mom_does_chkpnt = 0;
static  NtOpenThread_t NtOpenThread = NULL;
/*
 ** local resource array
 */
struct	config	dependent_config[] = {
	{ "physmem",	physmem },
	{ "ncpus",	ncpus },
	{ "loadave",	loadave },
	{ NULL,		nullproc },
};

/* Windows Profiling functions */
#define SAMPLE_DELTA	10
#define PROF_FSHIFT  	11
#define FSCALE		(1 << PROF_FSHIFT)
#define CEXP   ((unsigned int)(0.9200444146293232 * FSCALE))    /* exp(-1/12) */

typedef struct {
	HQUERY	 hQuery;
	HCOUNTER hCounter;
	DWORD	 value;
} PDH_profile;

PDH_profile mom_prof;

#if (_WIN32_WINNT < 0x0501)
/**
 *
 * @brief
 *	Is the process part of Windows job object.
 *
 * @param[in]	hProc - handle to process
 * @param[in]	hJob - handle to job
 * @param[out]	p_is_process_in_job - pointer to store a bool value indicating
 *              whether the process is part of the Windows job object
 *
 * @return      void
 **/
void
IsProcessInJob(HANDLE hProc, HANDLE hJob, BOOL *p_is_process_in_job)
{
	int			                nps = 0;
	DWORD			                i = 0;
	DWORD			                pidlistsize = 0;
	DWORD                                   pid = 0;
	PJOBOBJECT_BASIC_PROCESS_ID_LIST	pProcessList;
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION	ji;

	if (hJob == INVALID_HANDLE_VALUE || hJob == NULL
		|| hProc == INVALID_HANDLE_VALUE || hProc == NULL) {
		*p_is_process_in_job = FALSE;
		return;
	}

	/* Get the number of processes embedded in the job */
	if (QueryInformationJobObject(hJob,
		JobObjectBasicAccountingInformation,
		&ji, sizeof(ji), NULL)) {
		nps = ji.TotalProcesses;
	}

	if (nps == 0) {
		*p_is_process_in_job = FALSE;
		return;
	}

	/* Compute the size of pid list */
	pidlistsize = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
		(nps-1) * sizeof(DWORD);

	pProcessList = (PJOBOBJECT_BASIC_PROCESS_ID_LIST) malloc(pidlistsize);
	if (pProcessList == NULL) {
		*p_is_process_in_job = FALSE;
		return;
	}

	pProcessList->NumberOfAssignedProcesses = nps;
	pProcessList->NumberOfProcessIdsInList = 0;

	/* Get the pid list */
	if (FALSE == QueryInformationJobObject(hJob,
		JobObjectBasicProcessIdList,
		pProcessList, pidlistsize, NULL))
		return;

	/*
	 * Traverse through each process and find the
	 * memory used by that process during its execution.
	 */
	pid = GetProcessId(hProc);
	for (i = 0; i < (pProcessList->NumberOfProcessIdsInList); i++) {
		if (pProcessList->ProcessIdList[i] == pid) {
			free(pProcessList);
			*p_is_process_in_job = TRUE;
			return;
		}
	}
	free(pProcessList);
	*p_is_process_in_job = FALSE;
	return;
}
#endif /* _WIN32_WINNT < 0x0501 */

/**
 * @brief
 *	opens profile .
 *
 * @param[in] prof - pointer to PDH_profile struct
 *
 * @return	BOOL
 * @retval	TRUE	success
 * @retval	FALSE	error
 *
 */
static BOOL
open_profile(PDH_profile *prof)
{
	BOOL ret = TRUE;

	__try {

		if (PdhOpenQuery(NULL, 0, &(prof->hQuery)) != ERROR_SUCCESS) {
			prof->hQuery = NULL;
			ret = FALSE;
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}

	return (ret);
}

/**
 * @brief
 *      closes profile .
 *
 * @param[in] prof - pointer to PDH_profile struct
 *
 * @return      BOOL
 * @retval      TRUE    success
 * @retval      FALSE   error
 *
 */
static BOOL
close_profile(PDH_profile *prof)
{
	BOOL ret = TRUE;

	__try {
		if (prof->hQuery == NULL) {
			ret = FALSE;
		} else {
			if (PdhCloseQuery(prof->hQuery) != ERROR_SUCCESS) {
				prof->hQuery = NULL;
				ret = FALSE;
			}
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}

	return (ret);
}

/**
 * @brief
 *      collects profile info .
 *
 * @param[in] prof - pointer to PDH_profile struct
 *
 * @return      BOOL
 * @retval      TRUE    success
 * @retval      FALSE   error
 *
 */

static BOOL
collect_profile(PDH_profile *prof)
{

	BOOL ret = FALSE;
	PDH_FMT_COUNTERVALUE counter_val;
	PDH_STATUS rval;

	__try {
		if ((rval=PdhCollectQueryData(prof->hQuery)) == ERROR_SUCCESS) {
			if (PdhGetFormattedCounterValue(prof->hCounter,
				PDH_FMT_LONG, NULL, &counter_val) == ERROR_SUCCESS) {
				if (counter_val.CStatus == ERROR_SUCCESS) {
					prof->value = counter_val.longValue;
					ret = TRUE;
				}
			}
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}
	return (ret);
}

/**
 * @brief
 *      initializes the profile.
 *
 * @param[out] prof - pointer to PDH_profile struct
 *
 * @return      BOOL
 * @retval      TRUE    success
 * @retval      FALSE   error
 *
 */

BOOL
init_profile(LPTSTR counter_name, PDH_profile *prof)
{

	BOOL ret = TRUE;

	prof->hQuery = NULL;
	prof->hCounter = NULL;
	prof->value = -1;

	__try {
		if (open_profile(prof)) {
			if (PdhValidatePath(counter_name) == ERROR_SUCCESS)
				ret = (PdhAddCounter(prof->hQuery, counter_name, 0,
					&(prof->hCounter)) == ERROR_SUCCESS);
			else
				ret = FALSE;
		} else {
			ret = FALSE;
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}
	return (ret);
}

/**
 * @brief
 *      destroys profile .
 *
 * @param[in] prof - pointer to PDH_profile struct
 *
 * @return      BOOL
 * @retval      TRUE    success
 * @retval      FALSE   error
 *
 */

BOOL
destroy_profile(PDH_profile *prof)
{

	BOOL ret = TRUE;
	__try {
		if (prof->hCounter != NULL)
			ret = (PdhRemoveCounter(prof->hCounter) == ERROR_SUCCESS);
		if (!close_profile(prof))
			ret = FALSE;
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}
	return (ret);
}

/**
 * @brief
 *      wrapper function for collect_profile.
 *
 * @param[in] prof - pointer to PDH_profile struct
 *
 * @return      BOOL
 * @retval      TRUE    success
 * @retval      FALSE   error
 *
 */

BOOL
get_profile(PDH_profile *prof)
{
	BOOL ret = TRUE;

	__try { 
		ret = collect_profile(prof);
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		ret = FALSE;
	}
	return (ret);
}

/**
 * @brief
 *	print the profile info.
 *
 * @param[in] prof - pointer to PDH_profile which holds profile info
 * @param[in] hdr - heading for profile.
 *
 * @return	Void
 *
 */
void
print_profile(PDH_profile *prof,char * hdr)
{
	sprintf(log_buffer, "%s prof: query=%d counter=%d value=%d", hdr,
		prof->hQuery, prof->hCounter, prof->value);
	log_err(0, "print_profile", log_buffer);
}
/**
 * @brief
 *	Don't need any periodic processing.
 */
void
end_proc()
{
	DWORD		now, delta;
	DWORD		nrun;

	now = timeGetTime();
	delta = now - last_time;

	if (delta <= SAMPLE_DELTA*1000) {
		return;
	}

	wait_time = SAMPLE_DELTA;

	if (!get_profile(&mom_prof)) {
		return;
	}
	nrun = mom_prof.value + num_acpus + nrun_factor;
	load = ((load * CEXP) +
		(nrun * (FSCALE - CEXP) * FSCALE)) >> PROF_FSHIFT;

	DBPRT(("load = %d, mom_prof=%d num_acpus=%d nrun_factor=%d", load, mom_prof.value, num_acpus, nrun_factor))

	last_time = now;

	return;
}

/**
 * @brief
 *      Called from machine independent code to do any machine
 *      dependent initialization.
 *
 * @return      Void
 *
 */
void
dep_initialize()
{
	MEMORYSTATUSEX		mse;
	SYSTEM_INFO		si;

	mse.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&mse);
	pmem_size = mse.ullTotalPhys / 1024;

	GetSystemInfo(&si);
	num_acpus = num_oscpus = num_pcpus = si.dwNumberOfProcessors;
	page_size = si.dwPageSize;

	if (!init_profile("\\System\\Processor Queue Length", &mom_prof)) {
		log_err(-1, "dep_initialize", "init_profile failed!");
		return;
	}
}

/**
 * @brief
 *      This cleans up when MOM is restarted.
 *
 * @return      Void
 *
 */
void
dep_cleanup()
{
	log_event(PBSEVENT_SYSTEM, 0, LOG_INFO, __func__, "dependent cleanup");

	if (!destroy_profile(&mom_prof)) {
		log_err(-1, "dep_cleanup", "destroy_profile failed!");
		return;
	}
}

/**
 * @brief
 * Internal session memory usage decoding routine.
 * Accepts a job pointer.  Returns the sum of all memory
 * consumed for all tasks executed by the job, in kilo bytes.
 *
 * NOTE: To retrieve a handle to any process in the system,
 *       the calling process should have a privilege "SeDebugPrivilege".
 *       A Win32 API OpenProcess() can be used in a calling process
 *       to obtain any desired process handle in the system.
 *
 *       In PBS, ena_privilege() function can be used to enable
 *       SeDebugPrivilege for calling process. For pbs_mom process,
 *       ena_privilege() has been used in it's main_thread() function.
 *
 * @param[in]	pjob - pointer to job
 *
 * @return      u_Long
 * @retval      0 - failure
 * @retval      memory usage of all the processes of a job - failure
 */
static u_Long
mem_sum(job *pjob)
{
	u_Long			mem = 0;
	int			nps = 0;
	DWORD			i;
	HANDLE			hProcess;
	DWORD			pidlistsize;
	DWORD			nwspages;
	SYSTEM_INFO		si;
	PJOBOBJECT_BASIC_PROCESS_ID_LIST	pProcessList;
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION	ji;
	pbs_task                *ptask = NULL;
	BOOL                    is_process_in_job = FALSE;

	/* Get the system info */
	GetSystemInfo(&si);

	/* Get the number of processes embedded in the job */
	if (pjob->ji_hJob != NULL &&
		QueryInformationJobObject(pjob->ji_hJob,
		JobObjectBasicAccountingInformation,
		&ji, sizeof(ji), NULL)) {
		nps = ji.TotalProcesses;
	}

	if (nps == 0) {
		pjob->ji_flags |= MOM_NO_PROC;
		return 0;
	}


	/* Compute the size of pid list */
	pidlistsize = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
		(nps-1) * sizeof(DWORD);

	pProcessList = (PJOBOBJECT_BASIC_PROCESS_ID_LIST) malloc(pidlistsize);
	if (pProcessList == NULL) {
		log_err(-1, "mem_sum:", "memory allocation failed");
		return (0);
	}

	pProcessList->NumberOfAssignedProcesses = nps;
	pProcessList->NumberOfProcessIdsInList = 0;

	/* Get the pid list */
	if (pjob->ji_hJob != NULL)
		QueryInformationJobObject(pjob->ji_hJob,
			JobObjectBasicProcessIdList,
			pProcessList, pidlistsize, NULL);

	/*
	 * Traverse through each process and find the
	 * memory used by that process during its execution.
	 */
	for (i = 0; i < (pProcessList->NumberOfProcessIdsInList); i++) {
		hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
			PROCESS_VM_READ,
			FALSE, pProcessList->ProcessIdList[i]);
		if (hProcess != NULL) {
			(void)QueryWorkingSet(hProcess, &nwspages, sizeof(nwspages));
			mem += nwspages * (si.dwPageSize >> 10);
			CloseHandle(hProcess);

		}
	}
	free(pProcessList);
	/*
	 * processes that are attached using pbs_attach and are not in session 0,
	 * should be accounted here as those are not assigned to the job object
	 */
	for (ptask = (task *)GET_NEXT(pjob->ji_tasks); ptask; ptask = (task *)GET_NEXT(ptask->ti_jobtask)) {
		/* account only if the process is not part of Windows the job object */
		if ((ptask->ti_hProc != NULL) &&
			(ptask->ti_hProc != INVALID_HANDLE_VALUE)) {
			IsProcessInJob(ptask->ti_hProc, pjob->ji_hJob, &is_process_in_job);
			/* account for processes that are not part of the Windows job object */
			if (is_process_in_job == FALSE) {
				(void)QueryWorkingSet(ptask->ti_hProc, &nwspages, sizeof(nwspages));
				mem += nwspages * (si.dwPageSize >> 10);
			}
		}
	}
	return (mem);
}

/**
 * @brief
 * 	Internal session cpu time decoding routine.
 *
 * @param[in] job - job pointer
 *
 * @return	unsigned long
 * @retval	sum of all cpu time consumed for all 
 *		tasks executed by the job, in seconds.
 *
 */
static unsigned long
cput_sum(job *pjob)
{
	double			                cputime;
	int				        nps= 0;
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION	ji;
	pbs_task                                *ptask = NULL;
	BOOL                                    is_process_in_job = FALSE;
	__int64                                 *pkerneltime;
	__int64                                 *pusertime;

	cputime = 0.0;
	if (pjob->ji_hJob != NULL &&
		QueryInformationJobObject(pjob->ji_hJob,
		JobObjectBasicAccountingInformation,
		&ji, sizeof(ji), NULL)) {
		cputime = (double)(ji.TotalUserTime.QuadPart +
			ji.TotalKernelTime.QuadPart);
		nps = ji.TotalProcesses;
	}

	for (ptask = (task *)GET_NEXT(pjob->ji_tasks); ptask; ptask = (task *)GET_NEXT(ptask->ti_jobtask)) {
		FILETIME  ftCreation, ftExit, ftKernel, ftUser;
		if ((ptask->ti_hProc != NULL) && (ptask->ti_hProc != INVALID_HANDLE_VALUE)) {
			IsProcessInJob(ptask->ti_hProc, pjob->ji_hJob, &is_process_in_job);
			/*
			 * check if the processes is not part of the Windows job object due to pbs_attach
			 */
			if (is_process_in_job == TRUE)
				continue;
			/* Account for processes that are not part of the Windows job object */
			else if (GetProcessTimes(ptask->ti_hProc, &ftCreation, &ftExit, &ftKernel, &ftUser) == TRUE) {
				pkerneltime = (__int64*)&ftKernel;
				pusertime = (__int64*)&ftUser;
				cputime = cputime + *pkerneltime + *pusertime;
			}
			nps++;
		}
	}
	if (nps == 0)
		pjob->ji_flags |= MOM_NO_PROC;

	return ((unsigned long)(cputime/10000000.0 * cputfactor));
}

extern char *msg_momsetlim;

/**
 * @brief
 *      Establish system-enforced limits for the job.
 *
 *      Run through the resource list, checking the values for all items
 *      we recognize.
 *
 * @param[in] pjob - job pointer
 * @param[in]  set_mode - setting mode   SET_LIMIT_SET or SET_LIMIT_ALTER
 *
 *      If set_mode is SET_LIMIT_SET, then also set hard limits for the
 *                        system enforced limits (not-polled).
 *      If anything goes wrong with the process, return a PBS error code
 *      and print a message on standard error.  A zero-length resource list
 *      is not an error.
 *
 *      If set_mode is SET_LIMIT_SET the entry conditions are:
 *          1.  MOM has already forked, and we are called from the child.
 *          2.  The child is still running as root.
 *          3.  Standard error is open to the user's file.
 *
 *      If set_mode is SET_LIMIT_ALTER, we are beening called to modify
 *      existing limits.  Cannot alter those set by setrlimit (kernel)
 *      because we are the wrong process.
 *
 * @return      int
 * @retval      PBSE_NONE       Success
 * @retval      PBSE_*          Error
 *
 */
int
mom_set_limits(job *pjob, int set_mode)
{
	char		*pname;
	resource	*pres;
	int			retval;
	unsigned long	value;	/* place in which to build resource value */
	JOBOBJECT_BASIC_LIMIT_INFORMATION	jl = { 0 };
	extern int      local_getsize(resource *, unsigned long *);
	extern int      local_gettime(resource *, unsigned long *);
	unsigned long   vmem_limit  = 0;

	DBPRT(("%s: entered\n", __func__))
	assert(pjob != NULL);
	assert(pjob->ji_hJob != NULL);
	assert(pjob->ji_wattr[(int)JOB_ATR_resource].at_type == ATR_TYPE_RESC);
	pres = (resource *)
		GET_NEXT(pjob->ji_wattr[(int)JOB_ATR_resource].at_val.at_list);

	/*
	 * Cycle through all the resource specifications,
	 * setting limits appropriately.
	 */

	/* mem and vmem limits come from the local node limits, not the job */
	vmem_limit  = pjob->ji_hosts[pjob->ji_nodeid].hn_nrlimit.rl_vmem << 10;

	while (pres != NULL) {
		assert(pres->rs_defin != NULL);
		pname = pres->rs_defin->rs_name;
		assert(pname != NULL);
		assert(*pname != '\0');

		if (strcmp(pname, "cput") == 0) {
			/* job cpu time */
			retval = local_gettime(pres, &value);
			if (retval != PBSE_NONE)
				return (error(pname, retval));
			jl.PerJobUserTimeLimit.QuadPart =
				((double)value * 10000000.0) / cputfactor;
			jl.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
		} else if (strcmp(pname, "pcput") == 0) {
			/* process cpu time - set */
			retval = local_gettime(pres, &value);
			if (retval != PBSE_NONE)
				return (error(pname, retval));
			jl.PerProcessUserTimeLimit.QuadPart =
				((double)value * 10000000.0) / cputfactor;
			jl.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
		} else if (strcmp(pname, "mem") == 0) {		/* ignore mem */
		} else if (strcmp(pname, "pmem") == 0) {	/* set */
			retval = local_getsize(pres, &value);
			if (retval != PBSE_NONE)
				return (error(pname, retval));
			if (value > INT_MAX)
				return (error(pname, PBSE_BADATVAL));
			if ((vmem_limit != 0) && (vmem_limit < value))
				jl.MaximumWorkingSetSize = vmem_limit;
			else
				jl.MaximumWorkingSetSize = value;
			jl.LimitFlags |= JOB_OBJECT_LIMIT_WORKINGSET;
		} else if (strcmp(pname, "walltime") == 0) {	/* Check */
			retval = local_gettime(pres, &value);
			if (retval != PBSE_NONE)
				return (error(pname, retval));
		} else if (strcmp(pname, "nice") == 0) {		/* set nice */
			long	pri = pres->rs_value.at_val.at_long;


			if (!has_privilege(SE_INC_BASE_PRIORITY_NAME)) {
				ena_privilege(SE_INC_BASE_PRIORITY_NAME);
			}

			if (pri < 0)
				jl.PriorityClass = HIGH_PRIORITY_CLASS;
			else if (pri == 0)
				jl.PriorityClass = NORMAL_PRIORITY_CLASS;
			else
				jl.PriorityClass = IDLE_PRIORITY_CLASS;
			jl.LimitFlags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
		}
		pres = (resource *)GET_NEXT(pres->rs_link);
	}
	if (set_mode == SET_LIMIT_SET && jl.LimitFlags) {
		if (SetInformationJobObject(pjob->ji_hJob,
			JobObjectBasicLimitInformation,
			&jl, sizeof(jl)) == 0) {
			sprintf(log_buffer,
				"warning: unable to set limits for job %s error=%d",
				pjob->ji_qs.ji_jobid, GetLastError());
			log_err(-1, __func__, log_buffer);
		}
	}
	return (PBSE_NONE);
}

/**
 * @brief
 *      State whether MOM main loop has to poll this job to determine if some
 *      limits are being exceeded.
 *
 * @param[in] pjob - job pointer
 *
 * @return      int
 * @retval      TRUE    if polling is necessary
 * @retval      FALSE   otherwise.
 *
 * NOTE: Actual polling is done using the mom_over_limit machine-dependent function.
 *
 */
int
mom_do_poll(job *pjob)
{
	char		*pname;
	resource	*pres;

	DBPRT(("%s: entered\n", __func__))
	assert(pjob != NULL);
	assert(pjob->ji_wattr[(int)JOB_ATR_resource].at_type == ATR_TYPE_RESC);
	pres = (resource *)
		GET_NEXT(pjob->ji_wattr[(int)JOB_ATR_resource].at_val.at_list);

	while (pres != NULL) {
		assert(pres->rs_defin != NULL);
		pname = pres->rs_defin->rs_name;
		assert(pname != NULL);
		assert(*pname != '\0');

		if (strcmp(pname, "walltime") == 0 ||
			strcmp(pname, "mem") == 0)
			return (TRUE);
		pres = (resource *)GET_NEXT(pres->rs_link);
	}

	return (FALSE);
}

/**
 * @brief
 * 	Setup for polling.
 * 	allocate memory for data structure
 */
int
mom_open_poll()
{
	return (PBSE_NONE);
}

/**
 * @brief
 * 	Declare start of polling loop.
 *
 * @return int
 * @retval 0	success
 */
int
mom_get_sample()
{
	extern	time_t	time_last_sample;

	time_last_sample = time_now;

	DBPRT(("%s: entered\n", __func__))

	return (PBSE_NONE);
}

/**
 * @brief
 * 	Update the resources used.<attributes> of a job.
 *
 * @param[in]	pjob - job in question.
 *
 * @note
 *	The first time this is called for a job, set up resource entries for
 *	each resource that can be reported for this machine.  Fill in the
 *	correct values.
 *	If a resource attribute has been set in a mom hook, then its value
 *	will not be updated here. This allows a mom hook to override the
 *	resource value.
 *      Assumes that the session ID attribute has already been set.
 *
 * @return int
 * @retval PBSE_NONE	for success.
 */
int
mom_set_use(job *pjob)
{
	resource		*pres;
	resource		*pres_req;
	attribute		*at;
	attribute		*at_req;
	resource_def		*rd;
	unsigned long		*lp, lnum;
	u_Long                  *lp_sz, lnum_sz;

	assert(pjob != NULL);
	at = &pjob->ji_wattr[(int)JOB_ATR_resc_used];
	assert(at->at_type == ATR_TYPE_RESC);

	at->at_flags |= (ATR_VFLAG_MODIFY|ATR_VFLAG_SET);

	rd = find_resc_def(svr_resc_def, "cput", svr_resc_size);
	assert(rd != NULL);
	pres = find_resc_entry(at, rd);
	if (pres == NULL) {
		pres = add_resource_entry(at, rd);
		pres->rs_value.at_flags |= ATR_VFLAG_SET;
		pres->rs_value.at_type = ATR_TYPE_LONG;
		pres->rs_value.at_val.at_long = 0;
	} else if ((pres->rs_value.at_flags & ATR_VFLAG_HOOK) == 0) {
		lp = (unsigned long *)&pres->rs_value.at_val.at_long;
		lnum = cput_sum(pjob);
		*lp = MAX(*lp, lnum);
	}


	rd = find_resc_def(svr_resc_def, "ncpus", svr_resc_size);
	assert(rd != NULL);
	pres = add_resource_entry(at, rd);
	if (pres == NULL) {
		pres->rs_value.at_flags |= ATR_VFLAG_SET;
		pres->rs_value.at_type = ATR_TYPE_LONG;
	} else if ((pres->rs_value.at_flags & ATR_VFLAG_HOOK) == 0) {
		at_req = &pjob->ji_wattr[(int)JOB_ATR_resource];
		assert(at_req->at_type == ATR_TYPE_RESC);
		pres_req = find_resc_entry(at_req, rd);
		assert(pres_req != NULL);
		pres->rs_value.at_val.at_long = pres_req->rs_value.at_val.at_long;
	}

	rd = find_resc_def(svr_resc_def, "walltime", svr_resc_size);
	assert(rd != NULL);
	pres = find_resc_entry(at, rd);
	if (pres == NULL) {
		pres = add_resource_entry(at, rd);
		pres->rs_value.at_flags |= ATR_VFLAG_SET;
		pres->rs_value.at_type = ATR_TYPE_LONG;
		pres->rs_value.at_val.at_long = 0;
	} else if ((pres->rs_value.at_flags & ATR_VFLAG_HOOK) == 0) {
		pres->rs_value.at_val.at_long =
			(long)((double)(time_now -
			pjob->ji_qs.ji_stime) * wallfactor);
	}

	rd = find_resc_def(svr_resc_def, "mem", svr_resc_size);
	assert(rd != NULL);
	pres = find_resc_entry(at, rd);
	if (pres == NULL) {
		pres = add_resource_entry(at, rd);
		pres->rs_value.at_flags |= ATR_VFLAG_SET;
		pres->rs_value.at_type = ATR_TYPE_SIZE;
		pres->rs_value.at_val.at_size.atsv_shift = 10; /* in KB */
		pres->rs_value.at_val.at_size.atsv_units= ATR_SV_BYTESZ;
	} else if ((pres->rs_value.at_flags & ATR_VFLAG_HOOK) == 0) {
		lp_sz = &pres->rs_value.at_val.at_size.atsv_num;
		lnum_sz = mem_sum(pjob);
		*lp_sz = MAX(*lp_sz, lnum_sz);
	}

	return (PBSE_NONE);
}


/**
 * @brief
 *		Signal the associate process with given task <ptask>
 *		if which = 1, then suspend all the threads of the
 *		associated process. Otherwise, resume all the threads
 *
 * @param[in]
 *		ptask - pbs task structure
 *		which - operation to perform
 *
 * @return
 *		int
 *
 * @retval
 *		No. of processes that were operated on
 *
 */
static int
signal_task(task *ptask, int which)
{
	int count = 0;

	if (which == SUSPEND) {
		sprintf(log_buffer, "job %s: suspending pid=%d",
			ptask->ti_job->ji_qs.ji_jobid,
			ptask->ti_qs.ti_sid);
		log_err(0, __func__, log_buffer);
		count = processtree_op_by_id(ptask->ti_qs.ti_sid, SUSPEND, 0);
	} else {
		sprintf(log_buffer, "job %s: resuming pid=%d",
			ptask->ti_job->ji_qs.ji_jobid,
			ptask->ti_qs.ti_sid);
		log_err(0, __func__, log_buffer);
		count = processtree_op_by_id(ptask->ti_qs.ti_sid, RESUME, 0);
	}

	if (count == -1) {
		sprintf(log_buffer, "job %s: %s failed pid=%d",
			ptask->ti_job->ji_qs.ji_jobid,
			(which == SUSPEND) ? "suspend" : "resume",
			ptask->ti_qs.ti_sid);
		log_err(0, __func__, log_buffer);
	}

	return (count);
}

/**
 * @brief
 *      kill task session
 *
 * @param[in] ptask - pointer to pbs_task structure
 * @param[in] sig - signal number
 * @param[in] dir - indication how to kill
 *                  0 - kill child first
 *                  1 - kill parent first
 *
 * @return      int
 * @retval      number of tasks
 */
int
kill_task(task *ptask, int sig, int dir)
{
	HANDLE	hProc = ptask->ti_hProc;
	int	rc;
	int     terminate = 1;	/* terminate process by default */

	if (hProc == NULL)
		return 0;

	if (sig == suspend_signal) {
		rc = signal_task(ptask, 1);
		terminate = 0;
	} else if (sig == resume_signal) {
		rc = signal_task(ptask, 0);
		terminate = 0;
	}

	if (!terminate)
		return (rc);

	(void)mom_get_sample();
	/* Normal process termination, top command shell termination will result in exit codes < 256.     */
	/* To differentiate a process termination by signals, add BASE_SIGEXIT_CODE to sig, and the       */
	/* value (BASE_SIGEXIT_CODE + sig) will be assigned as the exit code for that terminated process. */
	processtree_op_by_handle(hProc, TERMINATE, BASE_SIGEXIT_CODE + sig);
	return 1;
}

/**
 * @brief
 * 	Clean up everything related to polling.
 *
 * @return      int
 * @retval      PBSE_NONE       Success
 * @retval      PBSE_SYSTEM     Error
 *
 */
int
mom_close_poll()
{
	DBPRT(("%s: entered\n", __func__))

	return (PBSE_NONE);
}

/**
 * @brief
 *      Checkpoint the job.
 *
 * @param[in] ptask - pointer to task
 * @param[in] file - filename
 * @param[in] abort - value indicating abort
 *
 * If abort is true, kill it too.
 *
 * @return      int
 * @retval      -1
 */
int
mach_checkpoint(task *ptask, char *file, int abort)
{
	return (-1);
}


/**
 * @brief
 *      Restart the job from the checkpoint file.
 *
 * @param[in] ptask - pointer to task
 * @param[in] file - filename
 *
 * @return      long
 * @retval      session id      Success
 * @retval      -1              Error
 */
long
mach_restart(task *ptask,char *file)
{
	return (-1);
}

/**
 * @brief
 *      computes and returns the cpu time process with  pid jobid
 *
 * @param[in] jobid - process id for job
 *
 * @return      string
 * @retval      cputime         Success
 * @retval      NULL            Error
 *
 */
char	*
cput_job(char *jobid)
{
	double			                cputime;
	HANDLE			                hjob;
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION	ji;
	job		                        *pjob = NULL;
	pbs_task	                        *ptask = NULL;
	BOOL                                    is_process_in_job = FALSE;
	__int64                                 *pkerneltime;
	__int64                                 *pusertime;

	hjob = OpenJobObject(JOB_OBJECT_QUERY, 0, jobid);
	if (hjob == NULL) {
		rm_errno = RM_ERR_EXIST;
		return NULL;
	}

	if (!QueryInformationJobObject(hjob,
		JobObjectBasicAccountingInformation,
		&ji, sizeof(ji), NULL)) {
		log_err(-1, __func__, "QueryJob");
		cputime = 0.0;
	}
	else {
		cputime = (double)(ji.TotalUserTime.QuadPart +
			ji.TotalKernelTime.QuadPart);
	}
	/*
	 * account for any processes that are not part of the job object due to pbs_attach
	 */
	pjob = find_job(jobid);
	ptask = (task *)GET_NEXT(pjob->ji_tasks);
	while (ptask) {
		FILETIME  ftCreation, ftExit, ftKernel, ftUser;
		if ((ptask->ti_hProc != NULL) && (ptask->ti_hProc != INVALID_HANDLE_VALUE)) {
			IsProcessInJob(ptask->ti_hProc, pjob->ji_hJob, &is_process_in_job);
			/*
			 * check if the processes is not part of the job object due to pbs_attach,
			 */
			if (is_process_in_job == TRUE)
				continue;
			else if (GetProcessTimes(ptask->ti_hProc, &ftCreation, &ftExit, &ftKernel, &ftUser) == TRUE) {
				pkerneltime = (__int64*)&ftKernel;
				pusertime = (__int64*)&ftUser;
				cputime = cputime + *pkerneltime + *pusertime;
			}
		}
		ptask = (task *)GET_NEXT(ptask->ti_jobtask);
	}
	sprintf(ret_string, "%.2f", (cputime/10000000.0) * cputfactor);
	CloseHandle(hjob);
	return ret_string;
}

/**
 * @brief
 *      wrapper function for cput_proc and cput_job.
 *
 * @param[in] attrib - pointer to rm_attribute structure
 *
 * @return      string
 * @retval      cputime         Success
 * @retval      NULL            ERRor
 *
 */
static char	*
cput(struct rm_attribute *attrib)
{
	int			value;

	if (attrib == NULL) {
		log_err(-1, __func__, no_parm);
		rm_errno = RM_ERR_NOPARAM;
		return NULL;
	}
	if ((value = atoi(attrib->a_value)) == 0) {
		sprintf(log_buffer, "bad param: %s", attrib->a_value);
		log_err(-1, __func__, log_buffer);
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}
	if (momgetattr(NULL)) {
		log_err(-1, __func__, extra_parm);
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}

	if (strcmp(attrib->a_qualifier, "job") == 0)
		return (cput_job(attrib->a_value));
	else {
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}
}

/**
 * @brief
 *      return the number of cpus
 *
 * @param[in] attrib - pointer to rm_attribute structure
 *
 * @return      string
 * @retval      number of cpus  Success
 * @retval      NULL            Error
 *
 */
char	*
ncpus(struct rm_attribute *attrib)
{
	if (attrib) {
		log_err(-1, __func__, extra_parm);
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}
	sprintf(ret_string, "%ld", num_acpus);
	return ret_string;
}

/**
 * @brief
 *	calculate load avg.
 *
 * @return	int
 * @retval	0	
 */
int
get_la(double *rv)
{
	*rv = (double)load/FSCALE;

	return 0;
}


/**
 * @brief
 *      returns the total physical memory
 *
 * @param[in] attrib - pointer to rm_attribute structure
 *
 * @return      string
 * @retval      tot physical memory     Success
 * @retval      NULL                    Error
 *
 */
char	*
physmem(struct rm_attribute *attrib)
{
	if (attrib) {
		log_err(-1, __func__, extra_parm);
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}

	sprintf(ret_string, "%I64ukb", pmem_size);
	return ret_string;
}

/**
 * @brief
 *      get file attribute from param and put them in buffer.
 *
 * @param[in] param - file attributes
 *
 * @return      string
 * @retval      size of file    Success
 * @retval      NULL            Error
 *
 */
char	*
size_file(char *param)
{
	struct	stat	sbuf;

	if (stat(param, &sbuf) == -1) {
		log_err(errno, __func__, "stat");
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}

	sprintf(ret_string, "%ukb", sbuf.st_size >> 10); /* KB */
	return ret_string;
}



/**
 * @brief
 *      wrapper function for size_file which returns the size of file system
 *
 * @param[in] attrib - pointer to rm_attribute structure
 *
 * @return      string
 * @retval      size of file system     Success
 * @retval      NULL                    Error
 *
 */
static char	*
size(struct rm_attribute *attrib)
{
	char	*param;

	if (attrib == NULL) {
		log_err(-1, __func__, no_parm);
		rm_errno = RM_ERR_NOPARAM;
		return NULL;
	}
	if (momgetattr(NULL)) {
		log_err(-1, __func__, extra_parm);
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}

	param = attrib->a_value;
	if (strcmp(attrib->a_qualifier, "file") == 0)
		return (size_file(param));
	else if (strcmp(attrib->a_qualifier, "fs") == 0) {
		rm_errno = RM_ERR_UNKNOWN;
		return NULL;
	}
	else {
		rm_errno = RM_ERR_BADPARAM;
		return NULL;
	}
}

/**
 * @brief
 *	Get the info required for tm_attach.
 *
 * @param[in]	        pid - process pid
 * @param[out]          psid - session id
 * @param[out]          puid - process uid
 * @param[out]          puname - pointer to store user name for the process
 * @param[in]           uname_len - size of puname
 * @param[out]          comm - buffer to store command name
 * @param[in]           comm_len - size of comm
 *
 * @return              int
 * @retval	        TM_OKAY - success
 * @retval	        TM_ENOPROC - can not enumerate process or get the process info
 *
 */
int
dep_procinfo(pid_t pid, pid_t *psid, uid_t *puid, char *puname, size_t uname_len, char *comm, size_t comm_len)
{

	if (puname == NULL)
		return TM_ENOPROC;

	(void)get_processowner(pid, puid, puname, uname_len, comm, comm_len);

	if (*puname != NULL) {
		if (psid)
			*psid = pid;
		return TM_OKAY;
	}
	return TM_ENOPROC;
}

/**
 * @brief
 *	attach descendants of a task recursively, to the job
 *
 * @param[in]	        pjob            - pointer to the job struct
 * @param[in]           parentjobid     - task's parent job id
 * @param[in]           ppid            - task's parent process id
 *
 * @return              int
 * @retval	        0 - failure
 * @retval	        1 - success
 *
 */
int
dep_attach_child(job *pjob, char *parentjobid, pid_t ppid)
{
	HANDLE          hProcessSnap = INVALID_HANDLE_VALUE;
	HANDLE          hProcess = INVALID_HANDLE_VALUE;
	PROCESSENTRY32  pe32 = { sizeof(pe32) };
	BOOL            p_ok = FALSE;
	pbs_task        *ptask = NULL;
	extern pbs_task *find_session(pid_t);
	extern int task_save(pbs_task *);

	if (ppid <= 0)
		return 0;

	pe32.dwSize = sizeof(pe32);
	hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, ppid);
	if (hProcessSnap == INVALID_HANDLE_VALUE)
		return 0;

	for (p_ok=Process32First(hProcessSnap, &pe32); p_ok; p_ok=Process32Next(hProcessSnap, &pe32)) {
		if (ppid == pe32.th32ParentProcessID) {
			if (!dep_attach_child(pjob, parentjobid, pe32.th32ProcessID)) {
				close_valid_handle(&(hProcessSnap));
				return 0;
			}
		}
	}
	close_valid_handle(&(hProcessSnap));

	ptask = find_session(ppid);
	if (ptask != NULL) {
		/* task aready created, return success */
		return 1;
	}

	if ((hProcess = OpenProcess(PROCESS_ALL_ACCESS, TRUE, (DWORD)ppid)) == NULL) {
		sprintf(log_buffer, "%s: OpenProcess Failed for pid %d with error %d", __func__, ppid, GetLastError());
		return 0;
	}

	ptask = momtask_create(pjob);
	if (ptask == NULL) {
		sprintf(log_buffer, "%s: task create failed for pid %d", __func__, ppid);
		close_valid_handle(&(hProcess));
		return 0;
	}

	strcpy(ptask->ti_qs.ti_parentjobid, parentjobid);
	ptask->ti_qs.ti_parentnode = TM_ERROR_NODE;
	ptask->ti_qs.ti_myvnode = TM_ERROR_NODE;
	ptask->ti_qs.ti_parenttask = TM_INIT_TASK;
	ptask->ti_qs.ti_sid = ppid;
	ptask->ti_hProc = hProcess;
	ptask->ti_qs.ti_status = TI_STATE_RUNNING;
	ptask->ti_flags |= TI_FLAGS_ORPHAN;
	(void)task_save(ptask);

	sprintf(log_buffer,
		"pid %d sid %d cmd %s attached as task %8.8X",
		ppid, pe32.th32ParentProcessID, pe32.szExeFile, ptask->ti_qs.ti_task);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
		pjob->ji_qs.ji_jobid, log_buffer);
	return 1;
}

/**
 * @brief
 *	attach a task to the job
 *
 * @param[in]	        ptask            - pointer to the task
 *
 * @return              int
 * @retval	        TM_ENOPROC - failure
 * @retval	        TM_OKAY - success
 *
 */
int
dep_attach(task *ptask)
{
	if (!dep_attach_child(ptask->ti_job, ptask->ti_job->ji_qs.ji_jobid, ptask->ti_qs.ti_sid))
		return TM_ENOPROC;
	return TM_OKAY;
}

/**
 * @brief
 *	Dummy function to enumerate all process IDs.
 *	For now, just claim that we can't do it.
 */
pid_t *
allpids(void)
{
	return NULL;
}
