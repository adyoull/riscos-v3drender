/* v3d_ioctl.c: the request interface, as Linux's v3d driver answers it
   (v3d_drv.c, v3d_bo.c, v3d_submit.c) together with the generic DRM
   calls Mesa's v3d driver uses (GEM_CLOSE, GET_CAP, sync objects).

   Requests carry Linux's numbers and structures. As Linux's drm_ioctl
   does, a structure shorter than ours (from older Mesa) is taken with
   the rest as zero, and only the caller's length is written back.

   Not supported yet, and refused as Linux refuses a driver without them:
   ioctl extensions (multisync, the CPU queue), performance monitors,
   super pages, and sharing BOs between files (PRIME).

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2014-2018 Broadcom (the Linux driver this follows)
   Copyright (c) 2026 Andrew Youll */
#include "v3d.h"
#include "v3d_regs.h"

#define V3D_READ(o) v3d_rd (v3d, V3D_BANK_HUB, (o))
#define V3D_CORE_READ(c, o) v3d_rd (v3d, V3D_BANK_CORE0 + (c), (o))

static void *
uptr (uint64_t p)
{
  return (void *) (uintptr_t) p;
}

int
v3d_file_valid (struct v3d_dev *v3d, int file)
{
  return file >= 0 && file < V3D_MAX_FILES && v3d->files[file].used;
}

static int
lookup_bo (struct v3d_dev *v3d, int file, uint32_t handle)
{
  if (handle == 0 || handle >= v3d->files[file].nhandles)
    return 0;
  return v3d->files[file].handles[handle];
}

static struct v3d_syncobj *
lookup_syncobj (struct v3d_dev *v3d, int file, uint32_t handle)
{
  if (handle == 0 || handle >= V3D_MAX_SYNCOBJS)
    return 0;
  struct v3d_syncobj *s = &v3d->files[file].syncobjs[handle];
  return s->used ? s : 0;
}

/* ---- v3d_drv.c ---- */

static int
get_param (struct v3d_dev *v3d, int f, struct drm_v3d_get_param *args)
{
  static const uint32_t reg_map[] = {
    [DRM_V3D_PARAM_V3D_UIFCFG] = V3D_HUB_UIFCFG,
    [DRM_V3D_PARAM_V3D_HUB_IDENT1] = V3D_HUB_IDENT1,
    [DRM_V3D_PARAM_V3D_HUB_IDENT2] = V3D_HUB_IDENT2,
    [DRM_V3D_PARAM_V3D_HUB_IDENT3] = V3D_HUB_IDENT3,
    [DRM_V3D_PARAM_V3D_CORE0_IDENT0] = V3D_CTL_IDENT0,
    [DRM_V3D_PARAM_V3D_CORE0_IDENT1] = V3D_CTL_IDENT1,
    [DRM_V3D_PARAM_V3D_CORE0_IDENT2] = V3D_CTL_IDENT2,
  };
  if (args->pad != 0)
    return -V3D_EINVAL;
  /* DRM_V3D_PARAM_V3D_CORE0_IDENT0 is 0, so it is allowed explicitly */
  if (args->param < sizeof reg_map / sizeof reg_map[0]
      && (reg_map[args->param] || args->param == DRM_V3D_PARAM_V3D_CORE0_IDENT0))
    {
      uint32_t offset = reg_map[args->param];
      if (args->value != 0)
	return -V3D_EINVAL;
      if (args->param >= DRM_V3D_PARAM_V3D_CORE0_IDENT0
	  && args->param <= DRM_V3D_PARAM_V3D_CORE0_IDENT2)
	args->value = V3D_CORE_READ (0, offset);
      else
	args->value = V3D_READ (offset);
      return 0;
    }
  switch (args->param)
    {
    case DRM_V3D_PARAM_SUPPORTS_TFU:
    case DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH:
      args->value = 1;
      return 0;
    case DRM_V3D_PARAM_SUPPORTS_CSD:
      args->value = v3d->ver >= V3D_GEN_41;
      return 0;
    case DRM_V3D_PARAM_SUPPORTS_PERFMON:
    case DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT:
    case DRM_V3D_PARAM_SUPPORTS_CPU_QUEUE:
    case DRM_V3D_PARAM_MAX_PERF_COUNTERS:
    case DRM_V3D_PARAM_SUPPORTS_SUPER_PAGES:
      args->value = 0;			/* not yet */
      return 0;
    case DRM_V3D_PARAM_GLOBAL_RESET_COUNTER:
      args->value = v3d->stats.resets;
      return 0;
    case DRM_V3D_PARAM_CONTEXT_RESET_COUNTER:
      args->value = v3d->files[f].resets;
      return 0;
    default:
      return -V3D_EINVAL;
    }
}

int
v3d_open (struct v3d_dev *v3d, uint32_t owner)
{
  for (int f = 0; f < V3D_MAX_FILES; f++)
    if (!v3d->files[f].used)
      {
	struct v3d_file *file = &v3d->files[f];
	file->handles = plat_alloc (V3D_FIRST_HANDLES * sizeof *file->handles);
	if (file->handles)
	  memset (file->handles, 0, V3D_FIRST_HANDLES * sizeof *file->handles);
	file->syncobjs = plat_alloc (V3D_MAX_SYNCOBJS * sizeof *file->syncobjs);
	if (!file->handles || !file->syncobjs)
	  {
	    plat_free (file->handles);
	    plat_free (file->syncobjs);
	    return -V3D_ENOMEM;
	  }
	file->used = 1;
	file->owner = owner;
	file->nhandles = V3D_FIRST_HANDLES;
	return f;
      }
  return -V3D_EBUSY;
}

/* The file's handles go; jobs still running keep their own references
   to the BOs (Linux: v3d_postclose). */
void
v3d_close (struct v3d_dev *v3d, int f)
{
  if (!v3d_file_valid (v3d, f))
    return;
  v3d_service (v3d);
  struct v3d_file *file = &v3d->files[f];
  for (uint32_t h = 1; h < file->nhandles; h++)
    if (file->handles[h])
      v3d_bo_put (v3d, file->handles[h]);
  plat_free (file->handles);
  plat_free (file->syncobjs);
  memset (file, 0, sizeof *file);
  /* (0.31) the last program gone: PRESENT's file too (its jobs keep
     what they use until they finish) */
  if (!v3d_internal_file (v3d, f) && v3d->present.file >= 0 && !v3d_programs_open (v3d))
    v3d_present_stop (v3d);
  /* (0.25) the last program gone: its pooled pages back to RISC OS
     (BOs its jobs still hold go back as the jobs are released) */
  v3d_pool_trim (v3d, v3d_programs_open (v3d) ? v3d->pool_max : 0);
}

/* ---- v3d_bo.c ---- */

static int
create_bo (struct v3d_dev *v3d, int f, struct drm_v3d_create_bo *args)
{
  if (args->flags & ~DRM_RISCOS_BO_CPU_CACHED)
    return -V3D_EINVAL;
  uint32_t h = v3d_handle_slot (v3d, f);	/* (0.30) grows */
  if (!h)
    {
      v3d_bo_refused (v3d, V3D_REFUSED_HANDLES, args->size);
      return -V3D_ENOSPC;
    }
  v3d_service (v3d);
  int bo, ret = v3d_bo_create_cached (v3d, args->size,
				      (args->flags & DRM_RISCOS_BO_CPU_CACHED) != 0, &bo);
  if (ret)
    return ret;
  v3d->bos[bo].file = (int16_t) f;
  v3d->files[f].handles[h] = (uint16_t) bo;
  args->handle = h;
  args->offset = v3d_bo_offset (v3d, bo);
  return 0;
}

/* (0.11) DRM_IOCTL_RISCOS_IMPORT_MEMORY */
static int
import_memory (struct v3d_dev *v3d, int f, struct drm_riscos_import_memory *args)
{
  uint32_t h = v3d_handle_slot (v3d, f);	/* (0.30) grows */
  if (!h)
    return -V3D_ENOMEM;
  v3d_service (v3d);
  if ((args->flags & ~DRM_RISCOS_IMPORT_UNCACHED)
      || (uint64_t) (uintptr_t) args->address != args->address)
    return -V3D_EINVAL;
  int bo, ret = v3d_bo_import (v3d, (uintptr_t) args->address, args->size, &bo);
  if (ret)
    return ret;
  v3d->bos[bo].file = (int16_t) f;
  v3d->bos[bo].nocache = (args->flags & DRM_RISCOS_IMPORT_UNCACHED) != 0;
  v3d->files[f].handles[h] = (uint16_t) bo;
  args->handle = h;
  args->offset = v3d_bo_offset (v3d, bo);
  return 0;
}

/* (0.13) DRM_IOCTL_RISCOS_FILE_STATS */
static int
file_stats (struct v3d_dev *v3d, int f, struct drm_riscos_file_stats *args)
{
  unsigned s = plat_irq_off ();
  for (int q = 0; q < V3D_NQ && q < 5; q++)
    {
      args->file[q].busy_ns = v3d->files[f].busy_ns[q];
      args->file[q].jobs = v3d->files[f].done[q];
      args->all[q].busy_ns = v3d->stats.busy_ns[q];
      args->all[q].jobs = v3d->stats.jobs[q];
    }
  plat_irq_restore (s);
  return 0;
}

/* Linux returns an offset to give mmap; here the BO is already mapped,
   and the offset is its address: libdrm's mmap returns it as it is. */
static int
mmap_bo (struct v3d_dev *v3d, int f, struct drm_v3d_mmap_bo *args)
{
  if (args->flags != 0)
    return -V3D_EINVAL;
  int bo = lookup_bo (v3d, f, args->handle);
  if (!bo)
    return -V3D_ENOENT;
  args->offset = (uintptr_t) v3d_bo_cpu (v3d, bo);
  return 0;
}

static int
get_bo_offset (struct v3d_dev *v3d, int f, struct drm_v3d_get_bo_offset *args)
{
  int bo = lookup_bo (v3d, f, args->handle);
  if (!bo)
    return -V3D_ENOENT;
  args->offset = v3d_bo_offset (v3d, bo);
  return 0;
}

static void
bo_fences (struct v3d_dev *v3d, int bo, struct v3d_fence *f)
{
  for (int q = 0; q < V3D_NQ; q++)
    {
      f[q].q = (uint32_t) q;
      f[q].seq = v3d->bos[bo].last[q];
    }
}

static int
wait_bo (struct v3d_dev *v3d, int f, struct drm_v3d_wait_bo *args)
{
  if (args->pad != 0)
    return -V3D_EINVAL;
  int bo = lookup_bo (v3d, f, args->handle);
  if (!bo)
    return -V3D_EINVAL;
  struct v3d_fence fences[V3D_NQ];
  bo_fences (v3d, bo, fences);
  int64_t start = v3d_now_ns ();
  int64_t timeout = (int64_t) (args->timeout_ns > (uint64_t) INT64_MAX
			       ? INT64_MAX : args->timeout_ns);
  int64_t deadline = timeout >= INT64_MAX - start ? INT64_MAX : start + timeout;
  int ret = v3d_wait_fences (v3d, fences, V3D_NQ, 1, deadline, 0);
  /* Decrement the user's timeout, in case we got interrupted such that
     the ioctl will be restarted. */
  int64_t delta = v3d_now_ns () - start;
  args->timeout_ns = (uint64_t) delta >= args->timeout_ns ? 0 : args->timeout_ns - (uint64_t) delta;
  return ret;
}

static int
gem_close (struct v3d_dev *v3d, int f, struct drm_gem_close *args)
{
  int bo = lookup_bo (v3d, f, args->handle);
  if (!bo)
    return -V3D_EINVAL;
  v3d->files[f].handles[args->handle] = 0;
  v3d_service (v3d);
  v3d_bo_put (v3d, bo);
  return 0;
}

/* ---- v3d_submit.c ---- */

static void
add_dep (struct v3d_job *job, struct v3d_fence f)
{
  if (f.seq && (job->dep[f.q] == 0 || (int32_t) (f.seq - job->dep[f.q]) > 0))
    job->dep[f.q] = f.seq;
}

/* in_sync: a sync object's fence. A handle with no fence, or none at
   all, adds nothing (Linux ignores -ENOENT here). */
static void
add_syncobj_dep (struct v3d_dev *v3d, int f, struct v3d_job *job, uint32_t handle)
{
  struct v3d_syncobj *s = lookup_syncobj (v3d, f, handle);
  if (s && s->has_fence)
    add_dep (job, s->f);
}

/* v3d_lookup_bos: the BOs a job uses, with references */
static int
lookup_bos (struct v3d_dev *v3d, int f, struct v3d_job *job,
	    const uint32_t *handles, uint32_t count)
{
  if (count == 0)
    return 0;
  if (count > V3D_MAX_JOB_BOS)
    return -V3D_EINVAL;
  job->bos = count <= V3D_JOB_FEW_BOS ? job->few : plat_alloc (count * sizeof *job->bos);
  if (!job->bos)
    return -V3D_ENOMEM;
  for (uint32_t i = 0; i < count; i++)
    {
      int bo = lookup_bo (v3d, f, handles[i]);
      if (!bo)
	return -V3D_ENOENT;
      v3d_bo_get (v3d, bo);
      v3d_bo_cpu_clean (v3d, bo);		/* (0.11) cached/imported: to memory */
      job->bos[job->nbos++] = (uint16_t) bo;
    }
  return 0;
}

/* v3d_lock_bo_reservations + drm_sched_job_add_implicit_dependencies:
   the job runs after every earlier job that used one of its BOs. */
static void
implicit_deps (struct v3d_dev *v3d, struct v3d_job *job)
{
  for (uint32_t i = 0; i < job->nbos; i++)
    for (int q = 0; q < V3D_NQ; q++)
      {
	struct v3d_fence fence = { (uint32_t) q, v3d->bos[job->bos[i]].last[q] };
	add_dep (job, fence);
      }
}

/* v3d_attach_fences_and_unlock_reservation: the job's fence becomes the
   BOs' latest, and the out_sync's */
static void
attach_fences (struct v3d_dev *v3d, int f, struct v3d_job *job, uint32_t out_sync)
{
  for (uint32_t i = 0; i < job->nbos; i++)
    v3d->bos[job->bos[i]].last[job->q] = job->seq;
  struct v3d_syncobj *s = lookup_syncobj (v3d, f, out_sync);
  if (s)
    {
      s->has_fence = 1;
      s->f.q = job->q;
      s->f.seq = job->seq;
    }
}

static void
free_unsubmitted (struct v3d_dev *v3d, struct v3d_job *job)
{
  if (!job)
    return;
  for (uint32_t i = 0; i < job->nbos; i++)
    v3d_bo_put (v3d, job->bos[i]);
  if (job->bos != job->few)
    plat_free (job->bos);
  unsigned s = plat_irq_off ();
  job->next = v3d->free_jobs;
  v3d->free_jobs = job;
  plat_irq_restore (s);
}

static struct v3d_job *
new_job (struct v3d_dev *v3d, int f, int q)
{
  struct v3d_job *job = v3d_job_alloc (v3d);
  if (job)
    {
      job->q = (uint8_t) q;
      job->file = f;
    }
  return job;
}

static int
submit_cl (struct v3d_dev *v3d, int f, struct drm_v3d_submit_cl *args)
{
  struct v3d_job *bin = 0, *render = 0, *clean = 0, *last;
  int ret;
  if (args->pad)
    return -V3D_EINVAL;
  if (args->flags & ~(DRM_V3D_SUBMIT_CL_FLUSH_CACHE | DRM_V3D_SUBMIT_EXTENSION))
    return -V3D_EINVAL;
  if (args->flags & DRM_V3D_SUBMIT_EXTENSION)
    return -V3D_EINVAL;			/* no extensions yet */
  if (args->perfmon_id)
    return -V3D_ENOENT;			/* no performance monitors yet */
  v3d_service (v3d);

  render = new_job (v3d, f, V3D_RENDER);
  if (!render)
    return -V3D_ENOMEM;
  render->a.cl.start = args->rcl_start;
  render->a.cl.end = args->rcl_end;
  render->a.cl.bstart = args->bcl_start;	/* for a hang's dump */
  render->a.cl.bend = args->bcl_end;
  render->a.cl.qma = args->qma;
  render->a.cl.qms = args->qms;
  render->a.cl.qts = args->qts;
  add_syncobj_dep (v3d, f, render, args->in_sync_rcl);

  if (args->bcl_start != args->bcl_end)
    {
      bin = new_job (v3d, f, V3D_BIN);
      if (!bin)
	{
	  ret = -V3D_ENOMEM;
	  goto fail;
	}
      bin->a.cl.start = args->bcl_start;
      bin->a.cl.end = args->bcl_end;
      bin->a.cl.qma = args->qma;
      bin->a.cl.qms = args->qms;
      bin->a.cl.qts = args->qts;
      bin->a.cl.render = render;
      add_syncobj_dep (v3d, f, bin, args->in_sync_bcl);
    }

  if (args->flags & DRM_V3D_SUBMIT_CL_FLUSH_CACHE)
    {
      clean = new_job (v3d, f, V3D_CACHE_CLEAN);
      if (!clean)
	{
	  ret = -V3D_ENOMEM;
	  goto fail;
	}
      last = clean;
    }
  else
    last = render;

  ret = lookup_bos (v3d, f, last, uptr (args->bo_handles), args->bo_handle_count);
  if (ret)
    goto fail;
  implicit_deps (v3d, last);

  unsigned s = plat_irq_off ();
  if (bin)
    {
      v3d_push_job (v3d, bin);
      struct v3d_fence bf = { V3D_BIN, bin->seq };
      add_dep (render, bf);
    }
  v3d_push_job (v3d, render);
  if (clean)
    {
      struct v3d_fence rf = { V3D_RENDER, render->seq };
      add_dep (clean, rf);
      v3d_push_job (v3d, clean);
    }
  attach_fences (v3d, f, last, args->out_sync);
  v3d_pump (v3d);
  plat_irq_restore (s);
  return 0;

fail:
  free_unsubmitted (v3d, bin);
  free_unsubmitted (v3d, render);
  free_unsubmitted (v3d, clean);
  return ret;
}

int
v3d_lookup_bo (struct v3d_dev *v3d, int f, uint32_t handle)
{
  return lookup_bo (v3d, f, handle);
}

void
v3d_signal_syncobj (struct v3d_dev *v3d, int f, uint32_t handle)
{
  struct v3d_syncobj *s = lookup_syncobj (v3d, f, handle);
  if (s)
    {
      s->has_fence = 1;
      s->f.q = V3D_RENDER;
      s->f.seq = 0;			/* nothing to wait for */
    }
}

/* (0.31) PRESENT's jobs: a render job on the program's file, with BOs by
   index (the program's source, PRESENT's list, screen and staging) */
int
v3d_submit_render (struct v3d_dev *v3d, int f, uint32_t start, uint32_t end,
		   const uint16_t *bos, uint32_t n, uint32_t in_sync,
		   uint32_t out_sync, struct v3d_fence *fence)
{
  if (n > V3D_JOB_FEW_BOS)
    return -V3D_EINVAL;
  struct v3d_job *job = new_job (v3d, f, V3D_RENDER);
  if (!job)
    return -V3D_ENOMEM;
  job->a.cl.start = start;
  job->a.cl.end = end;
  job->bos = job->few;
  for (uint32_t i = 0; i < n; i++)
    {
      v3d_bo_get (v3d, bos[i]);
      v3d_bo_cpu_clean (v3d, bos[i]);
      job->bos[job->nbos++] = bos[i];
    }
  add_syncobj_dep (v3d, f, job, in_sync);
  implicit_deps (v3d, job);
  unsigned s = plat_irq_off ();
  v3d_push_job (v3d, job);
  attach_fences (v3d, f, job, out_sync);
  if (fence)
    {
      fence->q = V3D_RENDER;
      fence->seq = job->seq;
    }
  v3d_pump (v3d);
  plat_irq_restore (s);
  return 0;
}

static int
submit_tfu (struct v3d_dev *v3d, int f, struct drm_v3d_submit_tfu *args)
{
  if (args->flags & DRM_V3D_SUBMIT_EXTENSION)
    return -V3D_EINVAL;			/* no extensions yet */
  if (args->flags)
    return -V3D_EINVAL;
  v3d_service (v3d);
  struct v3d_job *job = new_job (v3d, f, V3D_TFU);
  if (!job)
    return -V3D_ENOMEM;
  job->a.tfu = *args;
  add_syncobj_dep (v3d, f, job, args->in_sync);
  /* the first handle is the output BO, the others inputs; 0 ends */
  uint32_t n = 0;
  while (n < 4 && args->bo_handles[n])
    n++;
  int ret = lookup_bos (v3d, f, job, args->bo_handles, n);
  if (ret)
    {
      free_unsubmitted (v3d, job);
      return ret;
    }
  implicit_deps (v3d, job);
  unsigned s = plat_irq_off ();
  v3d_push_job (v3d, job);
  attach_fences (v3d, f, job, args->out_sync);
  v3d_pump (v3d);
  plat_irq_restore (s);
  return 0;
}

static int
submit_csd (struct v3d_dev *v3d, int f, struct drm_v3d_submit_csd *args)
{
  if (args->pad)
    return -V3D_EINVAL;
  if (v3d->ver < V3D_GEN_41)
    return -V3D_EINVAL;
  if (args->flags)
    return -V3D_EINVAL;			/* no extensions yet */
  if (args->perfmon_id)
    return -V3D_ENOENT;
  v3d_service (v3d);
  struct v3d_job *job = new_job (v3d, f, V3D_CSD);
  struct v3d_job *clean = job ? new_job (v3d, f, V3D_CACHE_CLEAN) : 0;
  if (!clean)
    {
      free_unsubmitted (v3d, job);
      return -V3D_ENOMEM;
    }
  job->a.csd = *args;
  add_syncobj_dep (v3d, f, job, args->in_sync);
  int ret = lookup_bos (v3d, f, clean, uptr (args->bo_handles), args->bo_handle_count);
  if (ret)
    {
      free_unsubmitted (v3d, job);
      free_unsubmitted (v3d, clean);
      return ret;
    }
  implicit_deps (v3d, clean);
  unsigned s = plat_irq_off ();
  v3d_push_job (v3d, job);
  struct v3d_fence cf = { V3D_CSD, job->seq };
  add_dep (clean, cf);
  v3d_push_job (v3d, clean);
  attach_fences (v3d, f, clean, args->out_sync);
  v3d_pump (v3d);
  plat_irq_restore (s);
  return 0;
}

/* ---- sync objects (drm_syncobj.c, as far as Mesa's v3d uses them) ---- */

static int
syncobj_create (struct v3d_dev *v3d, int f, struct drm_syncobj_create *args)
{
  if (args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
    return -V3D_EINVAL;
  struct v3d_syncobj *t = v3d->files[f].syncobjs;
  uint32_t h;
  for (h = 1; h < V3D_MAX_SYNCOBJS && t[h].used; h++)
    ;
  if (h == V3D_MAX_SYNCOBJS)
    return -V3D_ENOMEM;
  memset (&t[h], 0, sizeof t[h]);
  t[h].used = 1;
  t[h].has_fence = (args->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
  args->handle = h;
  return 0;
}

static int
syncobj_wait (struct v3d_dev *v3d, int f, struct drm_syncobj_wait *args)
{
  if (args->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
    return -V3D_EINVAL;
  if (args->count_handles == 0 || args->count_handles > 64)
    return -V3D_EINVAL;
  const uint32_t *h = uptr (args->handles);
  struct v3d_fence fences[64];
  for (uint32_t i = 0; i < args->count_handles; i++)
    {
      struct v3d_syncobj *s = lookup_syncobj (v3d, f, h[i]);
      if (!s)
	return -V3D_ENOENT;
      if (!s->has_fence)
	{
	  /* Jobs are submitted as soon as asked, so a sync object with no
	     fence will never get one while this waits. */
	  if (!(args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
	    return -V3D_EINVAL;
	  fences[i].q = 0;
	  fences[i].seq = v3d->emitted[0] + 0x40000000u;	/* never */
	}
      else
	fences[i] = s->f;
    }
  int first;
  int ret = v3d_wait_fences (v3d, fences, (int) args->count_handles,
			     (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0,
			     args->timeout_nsec, &first);
  if (ret == 0)
    args->first_signaled = (uint32_t) first;
  return ret;
}

static int
syncobj_array (struct v3d_dev *v3d, int f, struct drm_syncobj_array *args, int signal)
{
  if (args->pad)
    return -V3D_EINVAL;
  const uint32_t *h = uptr (args->handles);
  for (uint32_t i = 0; i < args->count_handles; i++)
    if (!lookup_syncobj (v3d, f, h[i]))
      return -V3D_ENOENT;
  for (uint32_t i = 0; i < args->count_handles; i++)
    {
      struct v3d_syncobj *s = lookup_syncobj (v3d, f, h[i]);
      s->has_fence = (uint8_t) signal;
      s->f.q = 0;
      s->f.seq = 0;
    }
  return 0;
}

static int
syncobj_handle (struct v3d_dev *v3d, int f, struct drm_syncobj_handle *args, int to_fd)
{
  if (args->pad)
    return -V3D_EINVAL;
  struct v3d_syncobj *s = lookup_syncobj (v3d, f, args->handle);
  if (to_fd)
    {
      /* only a sync file can be exported, not the object itself */
      if (args->flags != DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)
	return -V3D_EINVAL;
      if (!s)
	return -V3D_ENOENT;
      if (!s->has_fence)
	return -V3D_EINVAL;
      for (int i = 0; i < V3D_MAX_SYNC_FILES; i++)
	if (!v3d->sync_file_used[i])
	  {
	    v3d->sync_file_used[i] = 1;
	    v3d->sync_files[i] = s->f;
	    args->fd = i + 1;
	    return 0;
	  }
      return -V3D_ENOMEM;
    }
  if (args->flags != DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)
    return -V3D_EINVAL;
  if (!s)
    return -V3D_ENOENT;
  int i = args->fd - 1;
  if (i < 0 || i >= V3D_MAX_SYNC_FILES || !v3d->sync_file_used[i])
    return -V3D_EINVAL;
  s->has_fence = 1;
  s->f = v3d->sync_files[i];
  return 0;
}

static int
sync_file (struct v3d_dev *v3d, struct drm_riscos_sync_file *args, int wait)
{
  int i = args->fd - 1;
  if (i < 0 || i >= V3D_MAX_SYNC_FILES || !v3d->sync_file_used[i])
    return -V3D_EINVAL;
  if (!wait)
    {
      v3d->sync_file_used[i] = 0;
      return 0;
    }
  int64_t deadline = args->timeout_ms < 0 ? INT64_MAX
		     : v3d_now_ns () + (int64_t) args->timeout_ms * 1000000;
  int ret = v3d_wait_fences (v3d, &v3d->sync_files[i], 1, 1, deadline, 0);
  args->signalled = ret == 0;
  return ret;
}

/* ---- the dispatcher (drm_ioctl) ---- */

union v3d_args
{
  struct drm_v3d_get_param get_param;
  struct drm_v3d_create_bo create_bo;
  struct drm_v3d_mmap_bo mmap_bo;
  struct drm_riscos_import_memory import_memory;
  struct drm_v3d_get_bo_offset get_bo_offset;
  struct drm_v3d_wait_bo wait_bo;
  struct drm_v3d_submit_cl submit_cl;
  struct drm_v3d_submit_tfu submit_tfu;
  struct drm_v3d_submit_csd submit_csd;
  struct drm_gem_close gem_close;
  struct drm_get_cap get_cap;
  struct drm_syncobj_create syncobj_create;
  struct drm_syncobj_destroy syncobj_destroy;
  struct drm_syncobj_handle syncobj_handle;
  struct drm_syncobj_wait syncobj_wait;
  struct drm_syncobj_array syncobj_array;
  struct drm_riscos_sync_file sync_file;
  struct drm_riscos_file_stats file_stats;
  struct drm_riscos_present present;
  uint8_t bytes[160];
};

int
v3d_ioctl (struct v3d_dev *v3d, int f, uint32_t request, void *arg)
{
  if (!v3d_file_valid (v3d, f))
    return -V3D_EINVAL;
  uint32_t usize = _IOC_SIZE (request), dir = _IOC_DIR (request);
  uint32_t type = _IOC_TYPE (request), nr = _IOC_NR (request);
  union v3d_args a;
  if (usize > sizeof a)
    usize = sizeof a;			/* longer than any we know: extra ignored */
  memset (&a, 0, sizeof a);
  if ((dir & _IOC_WRITE) && usize)
    memcpy (&a, arg, usize);

  int ret = -V3D_ENOTTY;
  if (type == DRM_IOCTL_BASE && nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END)
    switch (nr - DRM_COMMAND_BASE)
      {
      case DRM_V3D_SUBMIT_CL: ret = submit_cl (v3d, f, &a.submit_cl); break;
      case DRM_V3D_WAIT_BO: ret = wait_bo (v3d, f, &a.wait_bo); break;
      case DRM_V3D_CREATE_BO: ret = create_bo (v3d, f, &a.create_bo); break;
      case DRM_V3D_MMAP_BO: ret = mmap_bo (v3d, f, &a.mmap_bo); break;
      case DRM_V3D_GET_PARAM: ret = get_param (v3d, f, &a.get_param); break;
      case DRM_V3D_GET_BO_OFFSET: ret = get_bo_offset (v3d, f, &a.get_bo_offset); break;
      case DRM_V3D_SUBMIT_TFU: ret = submit_tfu (v3d, f, &a.submit_tfu); break;
      case DRM_V3D_SUBMIT_CSD: ret = submit_csd (v3d, f, &a.submit_csd); break;
      default: ret = -V3D_EINVAL; break;	/* perfmon, CPU queue: not yet */
      }
  else if (type == DRM_IOCTL_BASE)
    switch (nr)
      {
      case 0x09: ret = gem_close (v3d, f, &a.gem_close); break;
      case 0x0c:
	a.get_cap.value = a.get_cap.capability == DRM_CAP_SYNCOBJ;
	ret = a.get_cap.capability == DRM_CAP_SYNCOBJ ? 0 : -V3D_EINVAL;
	break;
      case 0xBF: ret = syncobj_create (v3d, f, &a.syncobj_create); break;
      case 0xC0:
	{
	  struct v3d_syncobj *s = lookup_syncobj (v3d, f, a.syncobj_destroy.handle);
	  ret = a.syncobj_destroy.pad ? -V3D_EINVAL : !s ? -V3D_EINVAL : 0;
	  if (!ret)
	    s->used = 0;
	  break;
	}
      case 0xC1: ret = syncobj_handle (v3d, f, &a.syncobj_handle, 1); break;
      case 0xC2: ret = syncobj_handle (v3d, f, &a.syncobj_handle, 0); break;
      case 0xC3: ret = syncobj_wait (v3d, f, &a.syncobj_wait); break;
      case 0xC4: ret = syncobj_array (v3d, f, &a.syncobj_array, 0); break;
      case 0xC5: ret = syncobj_array (v3d, f, &a.syncobj_array, 1); break;
      }
  else if (type == DRM_RISCOS_BASE)
    switch (nr)
      {
      case 0x00: ret = sync_file (v3d, &a.sync_file, 1); break;
      case 0x01: ret = sync_file (v3d, &a.sync_file, 0); break;
      case 0x02: ret = import_memory (v3d, f, &a.import_memory); break;
      case 0x03: ret = file_stats (v3d, f, &a.file_stats); break;
      case 0x04:
	ret = v3d_internal_file (v3d, f) ? -V3D_EINVAL : v3d_present (v3d, f, &a.present);
	break;
      }
  /* (0.11) whatever the request, the file's cached and imported BOs that
     jobs have written are invalidated before it returns: a wait that saw
     a job finish is followed by the CPU reading what it wrote */
  if (v3d->files[f].nstale || v3d->retired)
    {
      v3d_service (v3d);
      v3d_file_caches (v3d, f);
    }

  if ((ret == 0 || ret == -V3D_ETIME) && (dir & _IOC_READ) && usize)
    memcpy (arg, &a, usize);
  return ret;
}

/* ---- start and stop ---- */

int
v3d_init (struct v3d_dev *v3d)
{
  for (int i = V3D_MAX_JOBS - 1; i >= 0; i--)
    {
      v3d->jobs[i].next = v3d->free_jobs;
      v3d->free_jobs = &v3d->jobs[i];
    }
  v3d->stats.first_hang.q = v3d->stats.last_hang.q = ~0u;	/* none yet */
  v3d->twod.file = -1;			/* (0.17) no 2D yet */
  v3d->present.file = -1;		/* (0.31) no PRESENT yet */
  int ret = v3d_bo_init (v3d);
  if (ret)
    return ret;
  v3d_init_hw_state (v3d);
  ret = v3d_mmu_set_page_table (v3d);
  if (ret)
    return ret;
  v3d_irq_enable (v3d);
  v3d->last_check_cs = plat_time_cs ();
  v3d_service (v3d);			/* fills the overflow pool */
  return 0;
}

/* Waits (up to a second) for the GPU to finish, then lets everything go. */
void
v3d_fini (struct v3d_dev *v3d)
{
  uint32_t t0 = plat_time_cs ();
  while (!v3d_idle (v3d) && plat_time_cs () - t0 < 100)
    {
      v3d_service (v3d);
      plat_wait ();
    }
  unsigned s = plat_irq_off ();
  v3d_irq_disable (v3d);
  for (int q = 0; q < V3D_HW_QUEUES; q++)
    v3d_job_done (v3d, q, 1);
  for (int q = 0; q < V3D_NQ; q++)
    while (v3d->head[q])
      {
	struct v3d_job *job = v3d->head[q];
	v3d->head[q] = job->next;
	job->next = v3d->retired;
	v3d->retired = job;
      }
  plat_irq_restore (s);
  v3d_service (v3d);
  for (int f = 0; f < V3D_MAX_FILES; f++)
    v3d_close (v3d, f);
  while (v3d->noverflow_pool)
    v3d_bo_put (v3d, v3d->overflow_pool[--v3d->noverflow_pool]);
  v3d_bo_fini (v3d);
}
