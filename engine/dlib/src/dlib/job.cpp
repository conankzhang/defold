#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "array.h"
#include "align.h"
#include "atomic.h"
#include "math.h"
#include "mutex.h"
#include "poolallocator.h"
#include "index_pool.h"
#include "condition_variable.h"
#include "static_assert.h"
#include "buffer.h"
#include "job.h"
#include "thread.h"

/*TODO
    - Job system
    - Job system in lua
    - How to register jobs from C++?
    - When/how to delete jobs?
      - Can't just rely on gc in lua
        If a lua-program doesn't keep a reference a job could be deleted prior to completion
      - Should we delete jobs recursively, i.e. starting from root?
    - Support for locking buffers
    - Max jobs in queue and error handling

LATER
    - Simple signature for jobs, e.g. a string describing all the params: "FDBV", float, double, buffer, vector4
*/

// https://blog.molecular-matters.com/2015/08/24/job-system-2-0-lock-free-work-stealing-part-1-basics/
// https://blog.molecular-matters.com/2012/07/09/building-a-load-balanced-task-scheduler-part-4-false-sharing/
// https://manu343726.github.io/2017/03/13/lock-free-job-stealing-task-system-with-modern-c.html

// Example https://pastebin.com/iyKwsYvK

/*
Lua

local function do_stuff_from_lua(params)

end

local root = job.new(nil, nil, nil)
for local i = 1,10 do
    local params = {...}
    local j = job.new(do_stuff_from_lua, params, root)
    job.run(j)
end
job.run(root)
job.wait(root)

*/

namespace dmJob
{
    dmThread::TlsKey g_WorkerTlsKey = dmThread::AllocTls();    

    enum ParamType
    {
        PARAM_TYPE_FLOAT    = 0,
        PARAM_TYPE_DOUBLE   = 1,
        PARAM_TYPE_VECTOR4  = 2,
        PARAM_TYPE_MATRIX4  = 3,
        PARAM_TYPE_BUFFER   = 4,
    };

    struct Job;

    struct Job
    {
        Job* DM_ALIGNED(256) m_Parent;
        JobEntry             m_JobEntry;
        int32_atomic_t       m_UnfinishedJobs;
        Param                m_Params[MAX_JOB_PARAMS];
        uint8_t              m_ParamTypes[MAX_JOB_PARAMS];
        uint32_t             m_ParamCount;
    };

    const uint32_t MAX_WORKERS = 8;

    struct Worker;

    struct JobQueue
    {
        dmMutex::Mutex m_Lock;
        dmArray<Job*>  m_Jobs;

        JobQueue()
        {
            m_Lock = dmMutex::New();
        }

        ~JobQueue()
        {
            dmMutex::Delete(m_Lock);
        }

        void Push(Job* job)
        {
            DM_MUTEX_SCOPED_LOCK(m_Lock);
            if (m_Jobs.Full()) {
                m_Jobs.OffsetCapacity(64);
            }
            m_Jobs.Push(job);
        }

        Job* Pop()
        {
            DM_MUTEX_SCOPED_LOCK(m_Lock);
            uint32_t n = m_Jobs.Size();
            if (n == 0) {
                return 0;
            } else {
                Job* j = m_Jobs[n - 1];
                m_Jobs.SetSize(n - 1);
                return j;
            }            
        }
    };

    // Make sure that an array of jobs is properly aligned in order to
    // avoid false sharing
    //DM_STATIC_ASSERT(sizeof(Job) == 256, Invalid_Struct_Size);

    struct Worker
    {
        dmThread::Thread  m_Thread;
        JobQueue          m_Queue;
        bool              m_Active;
    };

    struct JobSystem
    {
        dmConditionVariable::ConditionVariable m_Condition;
        dmMutex::Mutex                         m_Lock;
        JobQueue                               m_Queue;
        Worker*                                m_Workers[MAX_WORKERS];
        dmArray<Job>                           m_Jobs;
        dmIndexPool32                          m_JobPool;
        int32_atomic_t                         m_WorkerCount;
    };

    JobSystem* g_JobSystem = 0;

    static Job* WaitForJob()
    {
        JobSystem* js = g_JobSystem;
        Job *j = 0;
        do  {
            j = js->m_Queue.Pop();
            if (j == 0) {
                dmMutex::Lock(js->m_Lock);
                dmConditionVariable::Wait(js->m_Condition, js->m_Lock);
                dmMutex::Unlock(js->m_Lock);
            }
        } while (j == 0);
        return j;
    }

    static void Execute(Job* job);

    static void HelpOut(Job* job_waiting)
    {
        JobSystem* js = g_JobSystem;
     //   printf("job_waiting->m_UnfinishedJobs: %d\n", job_waiting->m_UnfinishedJobs);
        //while (job_waiting->m_UnfinishedJobs > 1) {
            Job* j = js->m_Queue.Pop();
            if (j) {
                //printf("HELPING!\n");
                Execute(j);
            } else {
                // TODO: Add functionality to thread.h
                pthread_yield_np();
            }
    //    }
    }

    static void Finish(Job* job) 
    {
        while (job) {
            dmAtomicDecrement32(&job->m_UnfinishedJobs);
            job = job->m_Parent;
        }
    }

    static void Execute(Job* job)
    {
        while (job->m_UnfinishedJobs > 1) {
            HelpOut(job);
        }
        job->m_JobEntry(job->m_Params);
        Finish(job);
    }

    void WorkerMain(void* w)
    {
        Worker* worker = (Worker*) w;
        dmThread::SetTlsValue(g_WorkerTlsKey, (void*) w);
        while (worker->m_Active) {
            Job* j = WaitForJob();
            if (j) {
                Execute(j);
            }
        }
    }

    Result Wait(HJob job)
    {
        while (job->m_UnfinishedJobs > 0) {
            HelpOut(job);
        }
        return RESULT_OK;
    }

    static Worker* NewWorker()
    {
        Worker* w = new Worker;
        w->m_Active = true;
        w->m_Thread = dmThread::New(WorkerMain, 0x80000, w, "Job Worker");    
        int index = dmAtomicIncrement32(&g_JobSystem->m_WorkerCount);
        g_JobSystem->m_Workers[index] = w;
        return w;
    }

    void Init(uint32_t worker_count)
    {
        assert(g_JobSystem == 0);
        worker_count = dmMath::Min(MAX_WORKERS, worker_count);
        g_JobSystem = new JobSystem;
        g_JobSystem->m_Condition = dmConditionVariable::New();
        g_JobSystem->m_Lock = dmMutex::New();
        g_JobSystem->m_WorkerCount = 0;

        for (uint32_t i = 0; i < worker_count; i++) {
            (void) NewWorker();
        }
    }

    Result New(JobEntry entry, HJob* job, HJob parent)
    {
        Job* j = new Job;
        j->m_JobEntry = entry;
        j->m_Parent = parent;
        j->m_UnfinishedJobs = 1;
        j->m_ParamCount = 0;

        if (parent) {
            dmAtomicIncrement32(&parent->m_UnfinishedJobs);
        }

        *job = j;
    }

    Result AddParam(Job* job, float x) 
    {
        if (job->m_ParamCount >= MAX_JOB_PARAMS) {
            return RESULT_TOO_MAX_PARAMS;
        } 
        uint32_t i = job->m_ParamCount++;
        job->m_Params[i].m_Float = x;
        job->m_ParamTypes[i] = PARAM_TYPE_FLOAT;
        return RESULT_OK;
    }

    Result Run(HJob job)
    {
        JobSystem* js = g_JobSystem;
        js->m_Queue.Push(job);
        dmConditionVariable::Signal(js->m_Condition);

        return RESULT_OK;
    }

    /*static Worker* GetWorker()
    {
        Worker* w = dmThread::GetTlsValue(g_WorkerTlsKey);
        if (!w) {
            w = new Worker;
            w->m_Lock = dmMutex::New();
            w->m_Active = true;
            w->m_Thread = New(WorkerMain, 0x80000, w, "Job Worker");            
        }
        return w;
    }*/

/*
    bool GetJob(Worker* worker, Job** job)
    {
        DM_MUTEX_SCOPED_LOCK(worker->m_Lock);

        int n = worker->m_Jobs.Size();

        if (n == 0) {
            return false;
        } else {
            *job = worker->m_Jobs[n - 1];
            worker->m_Jobs.EraseSwap(n - 1);
            return true;
        }
    }
*/
/*
    Result Wait(HJob job)
    {
        while (job->m_UnfinishedJobs > 0) {
            Job* stolen;
            if (GetJob(w, &stolen)) {
                Execute(stolen);
            }
        }

    }

  */  

}