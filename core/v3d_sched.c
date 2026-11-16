/* v3d_sched.c: queues, fences and the hang check.

   Linux's v3d driver uses the DRM GPU scheduler: one entity per client
   and queue, jobs that wait for their dependencies' fences, one job at a
   time on the hardware per queue, a timeout that resets the GPU when a
   job stops making progress. This is the same, for one CPU with no
   threads:

   - A fence is (queue, number). Each queue numbers its jobs from 1 in the
     order they are submitted and finishes them in that order, so
     "completed[q] >= n" says whether job n on queue q is done.
   - v3d_pump starts every job that can start: the head of each queue,
     when the queue's hardware is free and the job's dependencies are
     done. It runs from the interrupt handler when a job finishes, and
     from the SWI that submits. Cache-clean jobs (after compute) run in
     v3d_pump itself, as Linux runs them on the CPU.
   - Finished jobs go on a list; v3d_service, at the next call into the
     driver with interrupts on, lets go of their buffers and checks for
     hangs (Linux's timeout: 500 ms without progress).
   - Jobs from all clients share each queue in submission order (Linux:
     FIFO between entities; one entity per client).

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2018 Broadcom (the Linux scheduler this follows)
   Copyright (c) 2026 Andrew Youll */
#include "v3d.h"

int
v3d_fence_done (struct v3d_dev *v3d, struct v3d_fence f)
{
  return f.seq == 0 || (int32_t) (v3d->completed[f.q] - f.seq) >= 0;
}

static int
deps_done (struct v3d_dev *v3d, const struct v3d_job *job)
{
  for (int q = 0; q < V3D_NQ; q++)
    if (job->dep[q] && (int32_t) (v3d->completed[q] - job->dep[q]) < 0)
      return 0;
  return 1;
}

static struct v3d_job *
pop (struct v3d_dev *v3d, int q)
{
  struct v3d_job *job = v3d->head[q];
  v3d->head[q] = job->next;
  if (!job->next)
    v3d->tail[q] = 0;
  job->next = 0;
  return job;
}

static void
retire (struct v3d_dev *v3d, struct v3d_job *job)
{
  v3d->completed[job->q] = job->seq;
  v3d->stats.jobs[job->q]++;
  job->next = v3d->retired;
  v3d->retired = job;
}

/* Interrupts off. */
void
v3d_job_done (struct v3d_dev *v3d, int q, int failed)
{
  struct v3d_job *job = v3d->active[q];
  if (!job)
    return;
  v3d->active[q] = 0;
  job->failed = (uint8_t) failed;
  /* (0.13) its time on the GPU, as Linux's v3d_job_update_stats */
  uint64_t ns = plat_time_ns () - job->start_ns;
  v3d->stats.busy_ns[q] += ns;
  if (job->file >= 0 && job->file < V3D_MAX_FILES && v3d->files[job->file].used)
    {
      v3d->files[job->file].busy_ns[q] += ns;
      v3d->files[job->file].done[q]++;
    }
  /* A render job whose bin job failed isn't run: its tile lists aren't
     there (Linux: the bin fence's error ends the render job unrun). */
  if (failed && q == V3D_BIN && job->a.cl.render)
    job->a.cl.render->skip = 1;
  retire (v3d, job);
}

/* Interrupts off. Starts everything that can start. */
void
v3d_pump (struct v3d_dev *v3d)
{
  int progress;
  do
    {
      progress = 0;
      for (int q = 0; q < V3D_HW_QUEUES; q++)
	if (!v3d->active[q] && v3d->head[q] && deps_done (v3d, v3d->head[q]))
	  {
	    struct v3d_job *job = pop (v3d, q);
	    if (job->skip)
	      {
		job->failed = 1;
		retire (v3d, job);
		progress = 1;
		continue;
	      }
	    v3d->active[q] = job;
	    job->started = 1;
	    job->start_cs = 0;		/* v3d_service sets it */
	    job->start_ns = plat_time_ns ();
	    plat_barrier ();
	    v3d_run_job (v3d, job);
	    progress = 1;
	  }
      while (v3d->head[V3D_CACHE_CLEAN]
	     && deps_done (v3d, v3d->head[V3D_CACHE_CLEAN]))
	{
	  struct v3d_job *job = pop (v3d, V3D_CACHE_CLEAN);
	  uint64_t t0 = plat_time_ns ();
	  v3d_clean_caches (v3d);
	  uint64_t ns = plat_time_ns () - t0;
	  v3d->stats.busy_ns[V3D_CACHE_CLEAN] += ns;
	  if (job->file >= 0 && job->file < V3D_MAX_FILES && v3d->files[job->file].used)
	    {
	      v3d->files[job->file].busy_ns[V3D_CACHE_CLEAN] += ns;
	      v3d->files[job->file].done[V3D_CACHE_CLEAN]++;
	    }
	  retire (v3d, job);
	  progress = 1;
	}
    }
  while (progress);
}

struct v3d_job *
v3d_job_alloc (struct v3d_dev *v3d)
{
  for (;;)
    {
      unsigned s = plat_irq_off ();
      struct v3d_job *job = v3d->free_jobs;
      if (job)
	v3d->free_jobs = job->next;
      plat_irq_restore (s);
      if (job)
	{
	  memset (job, 0, sizeof *job);
	  job->file = -1;
	  return job;
	}
      /* all in use: wait for some to finish */
      if (v3d_idle (v3d))
	return 0;			/* (can't happen) */
      v3d_service (v3d);
      plat_wait ();
    }
}

/* Gives the job its number and queues it. Interrupts off. */
void
v3d_push_job (struct v3d_dev *v3d, struct v3d_job *job)
{
  job->seq = ++v3d->emitted[job->q];
  if (job->seq == 0)			/* (after 2^32 jobs: 0 means "none") */
    job->seq = ++v3d->emitted[job->q];
  job->next = 0;
  if (v3d->tail[job->q])
    v3d->tail[job->q]->next = job;
  else
    v3d->head[job->q] = job;
  v3d->tail[job->q] = job;
}

static void
release_job (struct v3d_dev *v3d, struct v3d_job *job)
{
  for (uint32_t i = 0; i < job->nbos; i++)
    {
      /* (0.11) the CPU's copy of a cached or imported BO is invalidated
	 before its file next sees it (v3d_file_caches, in its context) */
      struct v3d_bo *b = &v3d->bos[job->bos[i]];
      if (v3d_bo_cpu_cached (b) && !b->stale && b->file >= 0
	  && v3d->files[b->file].used)
	{
	  b->stale = 1;
	  v3d->files[b->file].nstale++;
	}
      v3d_bo_put (v3d, job->bos[i]);
    }
  plat_free (job->bos);
  /* binner overflow memory goes back to the pool (Linux frees it with
     the render job) */
  for (uint32_t i = 0; i < job->noverflow; i++)
    {
      unsigned s = plat_irq_off ();
      if (v3d->noverflow_pool < V3D_OVERFLOW_POOL)
	{
	  v3d->overflow_pool[v3d->noverflow_pool++] = job->overflow[i];
	  job->overflow[i] = 0;
	}
      plat_irq_restore (s);
      if (job->overflow[i])
	v3d_bo_put (v3d, job->overflow[i]);
    }
  unsigned s = plat_irq_off ();
  job->next = v3d->free_jobs;
  v3d->free_jobs = job;
  plat_irq_restore (s);
}

/* A hang: Linux's v3d_gpu_reset_for_timeout. The guilty job (the one
   that hung) is ended as failed. Since 0.13 the innocent ones, on the
   other queues, go back to the front of their queues and run again
   (Linux: drm_sched_resubmit_jobs): a render job's hang no longer loses
   the next frame's bin job too. A reset asked for (v3d_cancel_all) ends
   every job, as before. */
void
v3d_gpu_reset_now (struct v3d_dev *v3d)
{
  /* the first hung job's BOs, before anything is released (0.06) */
  for (int q = 0; q < V3D_HW_QUEUES; q++)
    if (v3d->active[q] && v3d->active[q]->guilty)
      v3d_dump_hang (v3d, v3d->active[q]);
  unsigned s = plat_irq_off ();
  v3d->reset_needed = 0;
  v3d_reset (v3d);
  for (int q = 0; q < V3D_HW_QUEUES; q++)
    if (v3d->active[q])
      {
	/* Linux counts the reset against the file whose job hung (its
	   CONTEXT_RESET_COUNTER), and against everyone (GLOBAL) */
	struct v3d_job *job = v3d->active[q];
	if (job->guilty && job->file >= 0 && job->file < V3D_MAX_FILES
	    && v3d->files[job->file].used)
	  v3d->files[job->file].resets++;
	if (!job->guilty && !v3d->reset_asked)
	  {
	    v3d->active[q] = 0;		/* innocent: again, from the start */
	    job->started = 0;
	    job->start_cs = job->first_cs = 0;
	    job->next = v3d->head[q];
	    v3d->head[q] = job;
	    if (!v3d->tail[q])
	      v3d->tail[q] = job;
	    v3d->stats.requeued++;
	    continue;
	  }
	v3d_job_done (v3d, q, 1);
      }
  v3d_pump (v3d);
  plat_irq_restore (s);
}

/* (0.10) Recovery: every queued job is ended unrun and every running one
   by a GPU reset, so that whatever programs left behind (a job that never
   ends, work queued behind it) is gone. Their fences are signalled, as
   failed. Interrupts on. */
void
v3d_cancel_all (struct v3d_dev *v3d)
{
  struct v3d_job *queued[V3D_NQ];
  unsigned s = plat_irq_off ();
  for (int q = 0; q < V3D_NQ; q++)
    {
      queued[q] = v3d->head[q];
      v3d->head[q] = v3d->tail[q] = 0;
    }
  plat_irq_restore (s);
  v3d->reset_asked = 1;
  v3d_gpu_reset_now (v3d);		/* ends the running jobs */
  v3d->reset_asked = 0;
  s = plat_irq_off ();
  for (int q = 0; q < V3D_NQ; q++)
    while (queued[q])
      {
	struct v3d_job *job = queued[q];
	queued[q] = job->next;
	job->next = 0;
	job->failed = 1;
	retire (v3d, job);			/* in order: after the running one */
      }
  plat_irq_restore (s);
  v3d_service (v3d);			/* lets go of their buffers */
}

/* With interrupts on, at each call into the driver. */
void
v3d_service (struct v3d_dev *v3d)
{
  unsigned s = plat_irq_off ();
  struct v3d_job *done = v3d->retired;
  v3d->retired = 0;
  plat_irq_restore (s);
  while (done)
    {
      struct v3d_job *next = done->next;
      release_job (v3d, done);
      done = next;
    }

  /* keep the binner's overflow memory topped up */
  while (v3d->noverflow_pool < V3D_OVERFLOW_POOL / 2)
    {
      int bo;
      if (v3d_bo_create (v3d, V3D_OVERFLOW_BYTES, &bo))
	break;
      v3d->bos[bo].overflow = 1;
      s = plat_irq_off ();
      v3d->overflow_pool[v3d->noverflow_pool++] = (uint16_t) bo;
      plat_irq_restore (s);
    }

  /* the hang check, every 10 cs at most */
  uint32_t now = plat_time_cs ();
  if (now - v3d->last_check_cs >= 10)
    {
      v3d->last_check_cs = now;
      s = plat_irq_off ();
      for (int q = 0; q < V3D_HW_QUEUES; q++)
	{
	  struct v3d_job *job = v3d->active[q];
	  if (!job)
	    continue;
	  if (!job->start_cs)
	    job->start_cs = job->first_cs = now | 1;
	  else if (now - job->start_cs >= V3D_TIMEOUT_CS)
	    {
	      if (v3d_job_progressing (v3d, job)
		  && now - job->first_cs < V3D_HARD_TIMEOUT_CS)
		job->start_cs = now | 1;	/* skip the reset; re-arm */
	      else
		{
		  v3d->stats.timeouts++;
		  v3d_record_hang (v3d, job);
		  job->guilty = 1;
		  v3d->reset_needed = 1;
		}
	    }
	}
      plat_irq_restore (s);
    }
  if (v3d->reset_needed)
    v3d_gpu_reset_now (v3d);
}

int64_t
v3d_now_ns (void)
{
  return (int64_t) plat_time_cs () * 10000000;
}

/* Waits until all (or, all = 0, any) of the fences are done, or the
   deadline passes (INT64_MAX: never; 0 or past: just look). 0 or
   -V3D_ETIME; *first = the first done, for "any". */
int
v3d_wait_fences (struct v3d_dev *v3d, const struct v3d_fence *f, int n,
		 int all, int64_t deadline_ns, int *first)
{
  for (;;)
    {
      v3d_service (v3d);
      int done = 0, firstdone = -1;
      for (int i = 0; i < n; i++)
	if (v3d_fence_done (v3d, f[i]))
	  {
	    done++;
	    if (firstdone < 0)
	      firstdone = i;
	  }
      if (first)
	*first = firstdone < 0 ? 0 : firstdone;
      if (all ? done == n : (done > 0 || n == 0))
	return 0;
      if (deadline_ns != INT64_MAX && v3d_now_ns () >= deadline_ns)
	return -V3D_ETIME;
      plat_wait ();
    }
}

int
v3d_idle (struct v3d_dev *v3d)
{
  for (int q = 0; q < V3D_NQ; q++)
    if ((q < V3D_HW_QUEUES && v3d->active[q]) || v3d->head[q])
      return 0;
  return 1;
}
