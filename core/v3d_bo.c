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

/* (0.25) avoid: pooled pages count as taken, so a new place (and an
   import, which has its own pages) leaves the pool's pages for BOs that
   can use them */
static uint32_t
taken_word (struct v3d_dev *v3d, uint32_t w, int avoid)
{
  return v3d->page_map[w] | (avoid ? v3d->page_backed[w] : 0);
}

static int
taken (struct v3d_dev *v3d, uint32_t p, int avoid)
{
  return (taken_word (v3d, p >> 5, avoid) >> (p & 31)) & 1;
}

/* first fit; whole free words are skipped 32 pages at a time */
static int
find_pages (struct v3d_dev *v3d, uint32_t n, uint32_t *first, int avoid)
{
  uint32_t run = 0, start = 0;
  for (uint32_t p = 1; p < v3d->area_pages;)
    {
      if ((p & 31) == 0 && p + 32 <= v3d->area_pages)
	{
	  uint32_t w = taken_word (v3d, p >> 5, avoid);
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
      if (taken (v3d, p, avoid))
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

/* (0.13) n free pages from a multiple of align (pages), as Linux's
   drm_mm aligns BOs of 64 KB and 1 MB: then the GPU's MMU can use 64 KB
   and 1 MB entries for them, where the physical pages line up too */
static int
range_ok (struct v3d_dev *v3d, uint32_t p, uint32_t n, int avoid)
{
  uint32_t end = p + n;
  while (p < end)
    {
      if ((p & 31) == 0 && p + 32 <= end)
	{
	  if (taken_word (v3d, p >> 5, avoid))
	    return 0;
	  p += 32;
	  continue;
	}
      if (taken (v3d, p, avoid))
	return 0;
      p++;
    }
  return 1;
}

static int
range_free (struct v3d_dev *v3d, uint32_t p, uint32_t n)
{
  return range_ok (v3d, p, n, 0);
}

/* Aligned BOs are placed from the top of the area down, the rest first
   fit from the bottom, so the small ones don't break up the aligned
   space (and a *V3DDump's small BOs still replay at their addresses). */
static int
find_pages_aligned (struct v3d_dev *v3d, uint32_t n, uint32_t *first, int avoid)
{
  uint32_t align = n >= 256 ? 256 : n >= 16 ? 16 : 1;
  if (align > 1 && n < v3d->area_pages)
    for (uint32_t p = (v3d->area_pages - n) & ~(align - 1); p >= align; p -= align)
      if (range_ok (v3d, p, n, avoid))
	{
	  *first = p;
	  return 1;
	}
  return find_pages (v3d, n, first, avoid);
}

/* a new place: clear of the pool if there is room, else anywhere free */
static int
find_place (struct v3d_dev *v3d, uint32_t n, uint32_t *first, int aligned)
{
  if (aligned)
    return find_pages_aligned (v3d, n, first, 1) || find_pages_aligned (v3d, n, first, 0);
  return find_pages (v3d, n, first, 1) || find_pages (v3d, n, first, 0);
}

/* ---- (0.25) the page pool ----
   Each CREATE_BO used to claim its pages from RISC OS (OS_Memory 12 and
   OS_DynamicArea 21/22, a few pages at a time) and each GEM_CLOSE gave
   them back. WebKit and Skia make and drop textures all the time while a
   page scrolls: in Strada's profile (22 Nov 2026) CREATE_BO was the
   biggest item in the GPU library, and each claim of specific pages can
   make RISC OS move other memory's pages (0.23's page moves). Now a
   freed BO's pages stay claimed and mapped where they are, up to
   pool_max pages while a program has the GPU open, and a new BO is put
   where pooled pages of its kind (cached or not) already are. */

static int
bit (const uint32_t *map, uint32_t p)
{
  return (map[p >> 5] >> (p & 31)) & 1;
}

static void
set_bits (uint32_t *map, uint32_t first, uint32_t n, int on)
{
  for (uint32_t p = first; p < first + n; p++)
    if (on)
      map[p >> 5] |= 1u << (p & 31);
    else
      map[p >> 5] &= ~(1u << (p & 31));
}

/* pages of word w that are pooled (claimed, not in use) and of this kind */
static uint32_t
pooled_word (struct v3d_dev *v3d, uint32_t w, int cached)
{
  uint32_t c = v3d->page_backed_cached[w];
  return v3d->page_backed[w] & ~v3d->page_map[w] & (cached ? c : ~c);
}

/* first fit of n pooled pages of the kind, starting at a multiple of align */
static int
find_pooled (struct v3d_dev *v3d, uint32_t n, uint32_t align, int cached, uint32_t *first)
{
  uint32_t start = 0, in = 0;
  for (uint32_t p = 1; p < v3d->area_pages;)
    {
      uint32_t w = pooled_word (v3d, p >> 5, cached);
      if ((p & 31) == 0 && p + 32 <= v3d->area_pages && (w == 0 || w == ~0u))
	{
	  if (w == 0)
	    {
	      in = 0;
	      p += 32;
	      continue;
	    }
	  if (!in)
	    in = 1, start = p;
	  p += 32;
	}
      else if ((w >> (p & 31)) & 1)
	{
	  if (!in)
	    in = 1, start = p;
	  p++;
	}
      else
	{
	  in = 0;
	  p++;
	  continue;
	}
      uint32_t s = (start + align - 1) & ~(align - 1);
      if (s < p && p - s >= n)
	{
	  *first = s;
	  return 1;
	}
    }
  return 0;
}

/* give pooled pages first..first+n-1 (all claimed) back to RISC OS */
static void
unpool (struct v3d_dev *v3d, uint32_t first, uint32_t n)
{
  plat_pages_release (v3d, first, n);
  set_bits (v3d->page_backed, first, n, 0);
  v3d->pool_pages -= n;
}

/* The pool's pages back to RISC OS, down to `keep` pages. */
void
v3d_pool_trim (struct v3d_dev *v3d, uint32_t keep)
{
  if (!v3d->page_backed)
    return;
  for (uint32_t p = 1; p < v3d->area_pages && v3d->pool_pages > keep;)
    {
      uint32_t w = v3d->page_backed[p >> 5] & ~v3d->page_map[p >> 5];
      if ((p & 31) == 0 && w == 0)
	{
	  p += 32;
	  continue;
	}
      if (!((w >> (p & 31)) & 1))
	{
	  p++;
	  continue;
	}
      uint32_t n = 1;
      int c = bit (v3d->page_backed_cached, p);
      while (p + n < v3d->area_pages && v3d->pool_pages - n > keep
	     && bit (v3d->page_backed, p + n) && !bit (v3d->page_map, p + n)
	     && bit (v3d->page_backed_cached, p + n) == c)
	n++;
      unpool (v3d, p, n);
      p += n;
    }
}

/* A program other than the desktop's 2D has the GPU open: then freed
   pages are worth keeping */
int
v3d_programs_open (struct v3d_dev *v3d)
{
  for (int f = 0; f < V3D_MAX_FILES; f++)
    if (v3d->files[f].used && f != v3d->twod.file)
      return 1;
  return 0;
}

/* Pages first..first+n-1 (none in use) claimed and mapped for the CPU
   as `cached` says: pooled pages of that kind are kept, pooled pages of
   the other kind given back first, the rest claimed. Pages it claims
   before a failure are left pooled. */
static int
back_pages (struct v3d_dev *v3d, uint32_t first, uint32_t n, int cached)
{
  uint32_t end = first + n;
  for (uint32_t p = first; p < end;)
    {
      int b = bit (v3d->page_backed, p);
      if (b && bit (v3d->page_backed_cached, p) == (cached != 0))
	{
	  p++;
	  continue;
	}
      uint32_t k = 1;
      while (p + k < end && bit (v3d->page_backed, p + k) == b
	     && (!b || bit (v3d->page_backed_cached, p + k) != (cached != 0)))
	k++;
      if (b)
	unpool (v3d, p, k);		/* the other kind: its mapping differs */
      if (plat_pages_claim (v3d, p, k, cached))
	return -1;
      set_bits (v3d->page_backed, p, k, 1);
      set_bits (v3d->page_backed_cached, p, k, cached);
      v3d->pool_pages += k;		/* (taken out of the pool below) */
      v3d->stats.pages_claimed += k;
      p += k;
    }
  return 0;
}

/* an import's GPU addresses: any pooled pages there go back first */
static void
unpool_range (struct v3d_dev *v3d, uint32_t first, uint32_t n)
{
  for (uint32_t p = first; p < first + n;)
    {
      if (!bit (v3d->page_backed, p))
	{
	  p++;
	  continue;
	}
      uint32_t k = 1;
      while (p + k < first + n && bit (v3d->page_backed, p + k)
	     && bit (v3d->page_backed_cached, p + k) == bit (v3d->page_backed_cached, p))
	k++;
      unpool (v3d, p, k);
      p += k;
    }
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
  if (bo == V3D_MAX_BOS)
    return -V3D_ENOMEM;
  uint64_t t0 = plat_time_ns ();
  uint32_t at = v3d->place_at, align = n >= 256 ? 256 : n >= 16 ? 16 : 1;
  v3d->place_at = 0;
  cached = cached != 0;
  if (at && at + n <= v3d->area_pages && range_free (v3d, at, n))
    first = at;				/* (0.13) the replay's address */
  else if (!(v3d->pool_pages >= n
	     && ((align > 1 && find_pooled (v3d, n, align, cached, &first))
		 || find_pooled (v3d, n, 1, cached, &first)))
	   && !find_place (v3d, n, &first, 1))
    return -V3D_ENOMEM;
  uint32_t claimed = v3d->stats.pages_claimed;
  if (back_pages (v3d, first, n, cached))
    {
      /* short of memory: the pool goes back to RISC OS, and once more */
      v3d_pool_trim (v3d, 0);
      if (back_pages (v3d, first, n, cached))
	{
	  v3d_pool_trim (v3d, 0);
	  return -V3D_ENOMEM;
	}
    }
  claimed = v3d->stats.pages_claimed - claimed;
  v3d->stats.bo_creates++;
  v3d->stats.pages_reused += n > claimed ? n - claimed : 0;
  if (!claimed)
    v3d->stats.bo_from_pool++;
  v3d->pool_pages -= n;
  mark (v3d, first, n, 1);
  v3d->pages_used += n;
  struct v3d_bo *b = &v3d->bos[bo];
  memset (b, 0, sizeof *b);
  b->used = 1;
  b->first = first;
  b->npages = n;
  b->refs = 1;
  b->cached = (uint8_t) cached;
  b->file = -1;
  /* zeroed, as Linux's shmem BOs are: fresh pages hold whatever RISC OS
     left there, pooled ones the last BO's contents */
  uint64_t t1 = plat_time_ns ();
  plat_zero_pages (v3d_bo_cpu (v3d, bo), n * V3D_PAGE);
  if (cached)
    plat_cache_clean (v3d_bo_cpu (v3d, bo), n * V3D_PAGE);	/* the zeroes, to memory */
  uint64_t t2 = plat_time_ns ();
  v3d_mmu_insert_ptes (v3d, bo);
  v3d->stats.zero_ns += t2 - t1;
  v3d->stats.create_ns += plat_time_ns () - t0;
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
  if (bo == V3D_MAX_BOS || !find_place (v3d, n, &first, 0))
    {
      plat_free (pa);
      return -V3D_ENOMEM;
    }
  unpool_range (v3d, first, n);
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

/* (0.17) Memory the OS doesn't page (the display's framestore): `bytes`
   from physical address `pa`, both page aligned, mapped for the GPU only
   (no CPU address, never cached by V3DRender). Placed as other BOs of its
   size are (aligned when big), so a 1 MB-aligned framestore gets 1 MB
   entries. */
int
v3d_bo_import_phys (struct v3d_dev *v3d, uint64_t pa, uint32_t bytes, int *bo_out)
{
  if (bytes == 0 || (pa & (V3D_PAGE - 1)) || (bytes & (V3D_PAGE - 1)))
    return -V3D_EINVAL;
  uint32_t n = bytes / V3D_PAGE, first;
  if (n >= v3d->area_pages)
    return -V3D_ENOMEM;
  uint64_t *pas = plat_alloc (n * sizeof *pas);
  if (!pas)
    return -V3D_ENOMEM;
  for (uint32_t i = 0; i < n; i++)
    pas[i] = pa + (uint64_t) i * V3D_PAGE;
  int bo;
  for (bo = 1; bo < V3D_MAX_BOS && v3d->bos[bo].used; bo++)
    ;
  if (bo == V3D_MAX_BOS || !find_place (v3d, n, &first, 1))
    {
      plat_free (pas);
      return -V3D_ENOMEM;
    }
  unpool_range (v3d, first, n);
  mark (v3d, first, n, 1);
  struct v3d_bo *b = &v3d->bos[bo];
  memset (b, 0, sizeof *b);
  b->used = 1;
  b->first = first;
  b->npages = n;
  b->refs = 1;
  b->imported = 1;
  b->nocache = 1;
  b->file = -1;
  b->cpu = 0;
  b->pa = pas;
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
  if (v3d_bo_cpu_cached (b))
    {
      uint64_t t0 = plat_time_ns ();
      plat_cache_clean (v3d_bo_cpu (v3d, bo), b->npages * V3D_PAGE);
      v3d->stats.cache_ns += plat_time_ns () - t0;
      v3d->stats.cache_cleans++;
    }
}

/* (0.15) a job that will write it again is still queued or running */
static int
bo_gpu_busy (struct v3d_dev *v3d, const struct v3d_bo *b)
{
  for (int q = 0; q < V3D_NQ; q++)
    if (b->last[q] && !v3d_fence_done (v3d, (struct v3d_fence) { (uint32_t) q, b->last[q] }))
      return 1;
  return 0;
}

/* Stale BOs are invalidated once their last job is done: the CPU can't
   be reading them before then (it waits first, and the wait's request
   ends here), and each invalidate of a big BO costs real time (0.15:
   YSFlight's 9 jobs a frame invalidated its whole frame 9 times). */
void
v3d_file_caches (struct v3d_dev *v3d, int f)
{
  struct v3d_file *file = &v3d->files[f];
  if (!file->nstale)
    return;
  uint32_t left = 0;
  uint64_t t0 = plat_time_ns ();
  for (int h = 1; h < V3D_MAX_HANDLES; h++)
    {
      int bo = file->handles[h];
      if (!bo || !v3d->bos[bo].stale)
	continue;
      if (bo_gpu_busy (v3d, &v3d->bos[bo]))
	{
	  left++;
	  v3d->stats.cache_deferred++;
	  continue;
	}
      plat_cache_inval (v3d_bo_cpu (v3d, bo), v3d->bos[bo].npages * V3D_PAGE);
      v3d->bos[bo].stale = 0;
      v3d->stats.cache_invals++;
    }
  v3d->stats.cache_ns += plat_time_ns () - t0;
  file->nstale = left;			/* (closed ones aren't counted) */
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
  mark (v3d, b->first, b->npages, 0);
  if (b->imported)
    plat_free (b->pa);			/* the memory is the program's */
  else
    {
      v3d->pages_used -= b->npages;
      v3d->pool_pages += b->npages;	/* (0.25) kept, if there's room */
      if (v3d->pool_pages > v3d->pool_max || !v3d_programs_open (v3d))
	unpool (v3d, b->first, b->npages);
    }
  b->used = 0;
}

int
v3d_bo_init (struct v3d_dev *v3d)
{
  uint32_t words = (v3d->area_pages + 31) / 32 * 4;
  v3d->page_map = plat_alloc (words);
  v3d->page_backed = plat_alloc (words);
  v3d->page_backed_cached = plat_alloc (words);
  if (!v3d->page_map || !v3d->page_backed || !v3d->page_backed_cached)
    {
      plat_free (v3d->page_map);
      plat_free (v3d->page_backed);
      plat_free (v3d->page_backed_cached);
      v3d->page_map = v3d->page_backed = v3d->page_backed_cached = 0;
      return -V3D_ENOMEM;
    }
  v3d->pool_pages = 0;
  if (!v3d->pool_max)
    v3d->pool_max = V3D_POOL_DEFAULT;
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
  v3d_pool_trim (v3d, 0);
  plat_free (v3d->page_map);
  plat_free (v3d->page_backed);
  plat_free (v3d->page_backed_cached);
  v3d->page_map = v3d->page_backed = v3d->page_backed_cached = 0;
}

/* ---- (0.23) pages the OS moves ----
   RISC OS may move a physical page out from under any memory that isn't
   locked, copying it and remapping its logical address: to give a
   dynamic area the specific pages it asked for, as V3DRender's own
   OS_Memory 12 claims do. A program's memory imported as a BO is used by
   the GPU through its physical pages, so a moved page would leave the
   GPU writing the old one, which now belongs to someone else: the web
   browser's frames overwrote render control lists that way (21 Nov
   2026). The OS says before (Service_PagesUnsafe) and after
   (Service_PagesSafe), as it does for any DMA. */

static int
bo_uses (const struct v3d_bo *b, const uint64_t *pa, uint32_t n)
{
  for (uint32_t i = 0; i < b->npages; i++)
    for (uint32_t j = 0; j < n; j++)
      if (b->pa[i] == pa[j])
	return 1;
  return 0;
}

int
v3d_pages_unsafe (struct v3d_dev *v3d, const uint64_t *pa, uint32_t n)
{
  int hit = 0;
  for (int bo = 1; bo < V3D_MAX_BOS && !hit; bo++)
    if (v3d->bos[bo].used && v3d->bos[bo].imported && v3d->bos[bo].pa)
      hit = bo_uses (&v3d->bos[bo], pa, n);
  if (!hit)
    return 0;
  v3d->hold = 1;
  v3d->stats.page_moves++;
  /* the jobs running now finish; the interrupt is polled, since this may
     come with interrupts off */
  uint32_t t0 = plat_time_cs ();
  for (;;)
    {
      unsigned s = plat_irq_off ();
      int busy = 0;
      for (int q = 0; q < V3D_HW_QUEUES; q++)
	busy |= v3d->active[q] != 0;
      if (busy)
	{
	  v3d_irq (v3d);		/* (not counted: only work done counts) */
	  busy = 0;
	  for (int q = 0; q < V3D_HW_QUEUES; q++)
	    busy |= v3d->active[q] != 0;
	}
      plat_irq_restore (s);
      if (!busy)
	break;
      if (plat_time_cs () - t0 > 100)
	{
	  plat_log (v3d, "Pages moving while the GPU is still busy", 0);
	  break;
	}
    }
  return 1;
}

void
v3d_pages_safe (struct v3d_dev *v3d, const uint64_t *old, const uint64_t *new_, uint32_t n)
{
  for (int bo = 1; bo < V3D_MAX_BOS; bo++)
    {
      struct v3d_bo *b = &v3d->bos[bo];
      if (!b->used || !b->imported || !b->pa)
	continue;
      int changed = 0;
      for (uint32_t i = 0; i < b->npages; i++)
	for (uint32_t j = 0; j < n; j++)
	  if (b->pa[i] == old[j] && old[j] != new_[j])
	    {
	      b->pa[i] = new_[j];
	      changed = 1;
	      v3d->stats.pages_moved++;
	      break;
	    }
      if (changed)
	v3d_mmu_insert_ptes (v3d, bo);
    }
  if (v3d->hold)
    {
      v3d->hold = 0;
      unsigned s = plat_irq_off ();
      v3d_pump (v3d);
      plat_irq_restore (s);
    }
}
