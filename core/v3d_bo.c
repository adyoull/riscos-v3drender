/* v3d_bo.c: buffer objects.

   Linux's v3d BOs are shmem pages, given a range of the GPU's address
   space (drm_mm) and mapped by the GPU's MMU (v3d_mmu_insert_ptes). Here
   the platform keeps one memory area for all of them, and a BO is a run
   of pages in it: the same run is its range of the GPU's address space
   (GPU address = offset in the area), and programs reach it at the same
   offset from the area's start, so no mapping is made per BO. Page 0 is
   never used: as on Linux, address 0 is special to parts of the GPU.

   Pages are claimed one by one from RISC OS and need not be contiguous:
   the GPU's MMU joins them up (using 64 KB and 1 MB entries where they
   happen to be contiguous). Like Linux's shmem BOs they start zeroed.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2015-2018 Broadcom (the Linux driver this follows)
   Copyright (c) 2026 Andrew Youll */
#include "v3d.h"

void v3d_mmu_insert_ptes (struct v3d_dev *v3d, int bo);
void v3d_mmu_remove_ptes (struct v3d_dev *v3d, int bo);

static int
page_used (struct v3d_dev *v3d, uint32_t p)
{
  return (v3d->page_map[p >> 5] >> (p & 31)) & 1;
}

int
v3d_page_used (struct v3d_dev *v3d, uint32_t p)
{
  return p < v3d->area_pages && page_used (v3d, p);
}

static void
mark (struct v3d_dev *v3d, uint32_t first, uint32_t n, int used)
{
  for (uint32_t p = first; p < first + n; p++)
    if (used)
      v3d->page_map[p >> 5] |= 1u << (p & 31);
    else
      v3d->page_map[p >> 5] &= ~(1u << (p & 31));
}

/* first fit; whole free words are skipped 32 pages at a time */
static int
find_pages (struct v3d_dev *v3d, uint32_t n, uint32_t *first)
{
  uint32_t run = 0, start = 0;
  for (uint32_t p = 1; p < v3d->area_pages;)
    {
      if ((p & 31) == 0 && p + 32 <= v3d->area_pages)
	{
	  uint32_t w = v3d->page_map[p >> 5];
	  if (w == 0)
	    {
	      if (!run)
		start = p;
	      run += 32;
	      p += 32;
	      if (run >= n)
		break;
	      continue;
	    }
	  if (w == ~0u)
	    {
	      run = 0;
	      p += 32;
	      continue;
	    }
	}
      if (page_used (v3d, p))
	run = 0;
      else
	{
	  if (!run)
	    start = p;
	  if (++run >= n)
	    break;
	}
      p++;
    }
  if (run < n)
    return 0;
  *first = start;
  return 1;
}

int
v3d_bo_create (struct v3d_dev *v3d, uint32_t bytes, int *bo_out)
{
  return v3d_bo_create_cached (v3d, bytes, 0, bo_out);
}

/* (0.11) cached: the CPU's mapping is cacheable (riscos-mesa's frames
   read back), with cache maintenance around jobs. */
int
v3d_bo_create_cached (struct v3d_dev *v3d, uint32_t bytes, int cached, int *bo_out)
{
  if (bytes == 0)
    return -V3D_EINVAL;
  uint32_t n = (bytes + V3D_PAGE - 1) / V3D_PAGE, first;
  if (n >= v3d->area_pages)
    return -V3D_ENOMEM;
  int bo;
  for (bo = 1; bo < V3D_MAX_BOS && v3d->bos[bo].used; bo++)
    ;
  if (bo == V3D_MAX_BOS || !find_pages (v3d, n, &first))
    return -V3D_ENOMEM;
  if (plat_pages_claim (v3d, first, n, cached))
    return -V3D_ENOMEM;
  mark (v3d, first, n, 1);
  v3d->pages_used += n;
  struct v3d_bo *b = &v3d->bos[bo];
  memset (b, 0, sizeof *b);
  b->used = 1;
  b->first = first;
  b->npages = n;
  b->refs = 1;
  b->cached = (uint8_t) (cached != 0);
  b->file = -1;
  memset (v3d_bo_cpu (v3d, bo), 0, n * V3D_PAGE);
  if (cached)
    plat_cache_clean (v3d_bo_cpu (v3d, bo), n * V3D_PAGE);	/* the zeroes, to memory */
  v3d_mmu_insert_ptes (v3d, bo);
  *bo_out = bo;
  return 0;
}

/* (0.11) The program's own memory, page-aligned, in the GPU's address
   space: a range of the area's pages is taken for its GPU addresses
   (they are never claimed or mapped for the CPU), and the GPU's MMU
   points them at the memory's physical pages. */
int
v3d_bo_import (struct v3d_dev *v3d, uintptr_t cpu, uint32_t bytes, int *bo_out)
{
  if (bytes == 0 || (cpu & (V3D_PAGE - 1)) || (bytes & (V3D_PAGE - 1)))
    return -V3D_EINVAL;
  uint32_t n = bytes / V3D_PAGE, first;
  if (n >= v3d->area_pages)
    return -V3D_ENOMEM;
  uint64_t *pa = plat_alloc (n * sizeof *pa);
  if (!pa)
    return -V3D_ENOMEM;
  for (uint32_t i = 0; i < n; i++)
    if (!(pa[i] = plat_virt_pa (cpu + i * V3D_PAGE)))
      {
	plat_free (pa);
	return -V3D_EFAULT;
      }
  int bo;
  for (bo = 1; bo < V3D_MAX_BOS && v3d->bos[bo].used; bo++)
    ;
  if (bo == V3D_MAX_BOS || !find_pages (v3d, n, &first))
    {
      plat_free (pa);
      return -V3D_ENOMEM;
    }
  mark (v3d, first, n, 1);
  struct v3d_bo *b = &v3d->bos[bo];
  memset (b, 0, sizeof *b);
  b->used = 1;
  b->first = first;
  b->npages = n;
  b->refs = 1;
  b->imported = 1;
  b->file = -1;
  b->cpu = cpu;
  b->pa = pa;
  v3d_mmu_insert_ptes (v3d, bo);
  *bo_out = bo;
  return 0;
}

uint64_t
v3d_bo_page_pa (struct v3d_dev *v3d, int bo, uint32_t i)
{
  struct v3d_bo *b = &v3d->bos[bo];
  return b->imported ? b->pa[i] : plat_page_pa (v3d, b->first + i);
}

int
v3d_bo_at (struct v3d_dev *v3d, uint32_t gpu)
{
  uint32_t page = gpu >> V3D_MMU_PAGE_SHIFT;
  for (int bo = 1; bo < V3D_MAX_BOS; bo++)
    {
      struct v3d_bo *b = &v3d->bos[bo];
      if (b->used && page >= b->first && page < b->first + b->npages)
	return bo;
    }
  return 0;
}

/* Cache maintenance (0.11). Both run in the context of the BO's own
   file's calls, so an imported BO's addresses are the program's. */
void
v3d_bo_cpu_clean (struct v3d_dev *v3d, int bo)
{
  struct v3d_bo *b = &v3d->bos[bo];
  if (b->cached || b->imported)
    plat_cache_clean (v3d_bo_cpu (v3d, bo), b->npages * V3D_PAGE);
}

void
v3d_file_caches (struct v3d_dev *v3d, int f)
{
  struct v3d_file *file = &v3d->files[f];
  if (!file->nstale)
    return;
  for (int h = 1; h < V3D_MAX_HANDLES && file->nstale; h++)
    {
      int bo = file->handles[h];
      if (bo && v3d->bos[bo].stale)
	{
	  plat_cache_inval (v3d_bo_cpu (v3d, bo), v3d->bos[bo].npages * V3D_PAGE);
	  v3d->bos[bo].stale = 0;
	  file->nstale--;
	}
    }
  file->nstale = 0;			/* (any left were closed) */
}

void
v3d_bo_get (struct v3d_dev *v3d, int bo)
{
  v3d->bos[bo].refs++;
}

/* The last reference goes when the last handle is closed and the last
   job using the BO has been released (v3d_service): the GPU is done with
   it by then. */
void
v3d_bo_put (struct v3d_dev *v3d, int bo)
{
  struct v3d_bo *b = &v3d->bos[bo];
  if (!b->used || --b->refs)
    return;
  v3d_mmu_remove_ptes (v3d, bo);
  if (b->imported)
    plat_free (b->pa);			/* the memory is the program's */
  else
    {
      plat_pages_release (v3d, b->first, b->npages);
      v3d->pages_used -= b->npages;
    }
  mark (v3d, b->first, b->npages, 0);
  b->used = 0;
}

int
v3d_bo_init (struct v3d_dev *v3d)
{
  v3d->page_map = plat_alloc ((v3d->area_pages + 31) / 32 * 4);
  if (!v3d->page_map)
    return -V3D_ENOMEM;
  mark (v3d, 0, 1, 1);			/* address 0 is never given out */
  return 0;
}

void
v3d_bo_fini (struct v3d_dev *v3d)
{
  for (int bo = 1; bo < V3D_MAX_BOS; bo++)
    if (v3d->bos[bo].used)
      {
	v3d->bos[bo].refs = 1;
	v3d_bo_put (v3d, bo);
      }
  plat_free (v3d->page_map);
  v3d->page_map = 0;
}
