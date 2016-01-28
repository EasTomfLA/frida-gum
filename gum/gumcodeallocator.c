/*
 * Copyright (C) 2010 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumcodeallocator.h"

#include "gumcodesegment.h"
#include "gummemory.h"
#include "gumprocess.h"
#ifdef HAVE_ARM
# include "gumthumbwriter.h"
#endif

#if defined (HAVE_ELF_H)
# include <elf.h>
#elif defined (HAVE_SYS_ELF_H)
# include <sys/elf.h>
#endif
#include <string.h>

#define GUM_CODE_SLICE_ELEMENT_FROM_SLICE(s) \
    ((GumCodeSliceElement *) (((guint8 *) (s)) - \
        G_STRUCT_OFFSET (GumCodeSliceElement, slice)))

typedef struct _GumCodePages GumCodePages;
typedef struct _GumCodeSliceElement GumCodeSliceElement;
typedef struct _GumCodeDeflectorDispatcher GumCodeDeflectorDispatcher;
typedef struct _GumCodeDeflectorImpl GumCodeDeflectorImpl;
typedef struct _GumProbeRangeForCodeCaveContext GumProbeRangeForCodeCaveContext;

struct _GumCodeSliceElement
{
  GList parent;
  GumCodeSlice slice;
};

struct _GumCodePages
{
  gint ref_count;

  GumCodeSegment * segment;
  gpointer data;
  gsize size;

  GumCodeAllocator * allocator;

  GumCodeSliceElement elements[1];
};

struct _GumCodeDeflectorDispatcher
{
  GSList * callers;

  gpointer address;
  gpointer trampoline;
  gpointer thunk;

  gpointer original_data;
  gsize original_size;
};

struct _GumCodeDeflectorImpl
{
  GumCodeDeflector parent;

  GumCodeAllocator * allocator;
};

struct _GumProbeRangeForCodeCaveContext
{
  const GumAddressSpec * caller;

  GumMemoryRange cave;
};

static GumCodeSlice * gum_code_allocator_try_alloc_batch_near (
    GumCodeAllocator * self, const GumAddressSpec * spec);

static void gum_code_pages_unref (GumCodePages * self);

static gboolean gum_code_slice_is_near (const GumCodeSlice * self,
    const GumAddressSpec * spec);
static gboolean gum_code_slice_is_aligned (const GumCodeSlice * slice,
    gsize alignment);

static GumCodeDeflectorDispatcher * gum_code_deflector_dispatcher_new (
    const GumAddressSpec * caller);
static void gum_code_deflector_dispatcher_free (
    GumCodeDeflectorDispatcher * dispatcher);
static gpointer gum_code_deflector_dispatcher_lookup (
    GumCodeDeflectorDispatcher * self, gpointer return_address);
static void gum_code_deflector_dispatcher_ensure_rw (
    GumCodeDeflectorDispatcher * self);
static void gum_code_deflector_dispatcher_ensure_rx (
    GumCodeDeflectorDispatcher * self);

#ifdef HAVE_LINUX
static gboolean gum_probe_range_for_code_cave (const GumRangeDetails * details,
    gpointer user_data);
#endif

void
gum_code_allocator_init (GumCodeAllocator * allocator,
                         gsize slice_size)
{
  allocator->slice_size = slice_size;
  allocator->slices_per_page = gum_query_page_size () / slice_size;
  allocator->pages_metadata_size = sizeof (GumCodePages) +
      ((allocator->slices_per_page - 1) * sizeof (GumCodeSliceElement));

  allocator->uncommitted_pages = NULL;
  allocator->dirty_pages = g_hash_table_new (NULL, NULL);
  allocator->free_slices = NULL;

  allocator->dispatchers = NULL;
}

void
gum_code_allocator_free (GumCodeAllocator * allocator)
{
  g_slist_foreach (allocator->dispatchers,
      (GFunc) gum_code_deflector_dispatcher_free, NULL);
  g_slist_free (allocator->dispatchers);
  allocator->dispatchers = NULL;

  g_list_foreach (allocator->free_slices, (GFunc) gum_code_pages_unref, NULL);
  g_hash_table_unref (allocator->dirty_pages);
  g_slist_free (allocator->uncommitted_pages);
  allocator->uncommitted_pages = NULL;
  allocator->dirty_pages = NULL;
  allocator->free_slices = NULL;
}

GumCodeSlice *
gum_code_allocator_alloc_slice (GumCodeAllocator * self)
{
  return gum_code_allocator_try_alloc_slice_near (self, NULL, 0);
}

GumCodeSlice *
gum_code_allocator_try_alloc_slice_near (GumCodeAllocator * self,
                                         const GumAddressSpec * spec,
                                         gsize alignment)
{
  GList * cur;

  for (cur = self->free_slices; cur != NULL; cur = cur->next)
  {
    GumCodeSliceElement * element = (GumCodeSliceElement *) cur;
    GumCodeSlice * slice = &element->slice;

    if (gum_code_slice_is_near (slice, spec) &&
        gum_code_slice_is_aligned (slice, alignment))
    {
      GumCodePages * pages = element->parent.data;

      self->free_slices = g_list_remove_link (self->free_slices, cur);

      g_hash_table_insert (self->dirty_pages, pages, pages);

      return slice;
    }
  }

  return gum_code_allocator_try_alloc_batch_near (self, spec);
}

void
gum_code_allocator_commit (GumCodeAllocator * self)
{
  gboolean rwx_supported;
  GSList * cur;
  GumCodePages * pages;
  GHashTableIter iter;

  rwx_supported = gum_query_is_rwx_supported ();

  for (cur = self->uncommitted_pages; cur != NULL; cur = cur->next)
  {
    GumCodeSegment * segment;

    pages = cur->data;
    segment = pages->segment;

    gum_code_segment_realize (segment);
    gum_code_segment_map (segment, 0,
        gum_code_segment_get_virtual_size (segment),
        gum_code_segment_get_address (segment));
  }
  g_slist_free (self->uncommitted_pages);
  self->uncommitted_pages = NULL;

  g_hash_table_iter_init (&iter, self->dirty_pages);
  while (g_hash_table_iter_next (&iter, (gpointer *) &pages, NULL))
  {
    gum_clear_cache (pages->data, pages->size);
  }
  g_hash_table_remove_all (self->dirty_pages);

  if (!rwx_supported)
  {
    g_list_foreach (self->free_slices, (GFunc) gum_code_pages_unref, NULL);
    self->free_slices = NULL;
  }
}

static GumCodeSlice *
gum_code_allocator_try_alloc_batch_near (GumCodeAllocator * self,
                                         const GumAddressSpec * spec)
{
  GumCodeSlice * result = NULL;
  gboolean rwx_supported;
  gsize size_in_pages, size_in_bytes;
  GumCodeSegment * segment;
  gpointer data;
  GumCodePages * pages;
  guint i;

  rwx_supported = gum_query_is_rwx_supported ();

  size_in_pages = 1;
  size_in_bytes = size_in_pages * gum_query_page_size ();

  if (rwx_supported)
  {
    segment = NULL;
    if (spec != NULL)
    {
      data = gum_try_alloc_n_pages_near (size_in_pages, GUM_PAGE_RWX, spec);
      if (data == NULL)
        return NULL;
    }
    else
    {
      data = gum_alloc_n_pages (size_in_pages, GUM_PAGE_RWX);
    }
  }
  else
  {
    segment = gum_code_segment_new (size_in_bytes, spec);
    if (segment == NULL)
      return NULL;
    data = gum_code_segment_get_address (segment);
  }

  pages = g_slice_alloc (self->pages_metadata_size);
  pages->ref_count = self->slices_per_page;

  pages->segment = segment;
  pages->data = data;
  pages->size = size_in_bytes;

  pages->allocator = self;

  for (i = self->slices_per_page; i != 0; i--)
  {
    guint slice_index = i - 1;
    GumCodeSliceElement * element = &pages->elements[slice_index];
    GList * link;
    GumCodeSlice * slice;

    slice = &element->slice;
    slice->data = (guint8 *) data + (slice_index * self->slice_size);
    slice->size = self->slice_size;

    link = &element->parent;
    link->data = pages;
    link->prev = NULL;
    if (slice_index == 0)
    {
      link->next = NULL;
      result = slice;
    }
    else
    {
      if (self->free_slices != NULL)
        self->free_slices->prev = link;
      link->next = self->free_slices;
      self->free_slices = link;
    }
  }

  if (!rwx_supported)
    self->uncommitted_pages = g_slist_prepend (self->uncommitted_pages, pages);

  g_hash_table_insert (self->dirty_pages, pages, pages);

  return result;
}

static void
gum_code_pages_unref (GumCodePages * self)
{
  self->ref_count--;
  if (self->ref_count == 0)
  {
    if (self->segment != NULL)
      gum_code_segment_free (self->segment);
    else
      gum_free_pages (self->data);

    g_slice_free1 (self->allocator->pages_metadata_size, self);
  }
}

void
gum_code_slice_free (GumCodeSlice * slice)
{
  GumCodeSliceElement * element;
  GumCodePages * pages;

  if (slice == NULL)
    return;

  element = GUM_CODE_SLICE_ELEMENT_FROM_SLICE (slice);
  pages = element->parent.data;

  if (gum_query_is_rwx_supported ())
  {
    GumCodeAllocator * allocator = pages->allocator;
    GList * link = &element->parent;

    if (allocator->free_slices != NULL)
      allocator->free_slices->prev = link;
    link->next = allocator->free_slices;
    allocator->free_slices = link;
  }
  else
  {
    gum_code_pages_unref (pages);
  }
}

static gboolean
gum_code_slice_is_near (const GumCodeSlice * self,
                        const GumAddressSpec * spec)
{
  gssize near_address;
  gssize slice_start, slice_end;
  gsize distance_start, distance_end;

  if (spec == NULL)
    return TRUE;

  near_address = (gssize) spec->near_address;

  slice_start = (gssize) self->data;
  slice_end = slice_start + self->size - 1;

  distance_start = ABS (near_address - slice_start);
  distance_end = ABS (near_address - slice_end);

  return distance_start <= spec->max_distance &&
      distance_end <= spec->max_distance;
}

static gboolean
gum_code_slice_is_aligned (const GumCodeSlice * slice,
                           gsize alignment)
{
  if (alignment == 0)
    return TRUE;

  return GPOINTER_TO_SIZE (slice->data) % alignment == 0;
}

GumCodeDeflector *
gum_code_allocator_alloc_deflector (GumCodeAllocator * self,
                                    const GumAddressSpec * caller,
                                    gpointer return_address,
                                    gpointer target)
{
  GumCodeDeflectorDispatcher * dispatcher = NULL;
  GSList * cur;
  GumCodeDeflectorImpl * impl;
  GumCodeDeflector * deflector;

  for (cur = self->dispatchers; cur != NULL; cur = cur->next)
  {
    GumCodeDeflectorDispatcher * d = cur->data;
    gsize distance;

    distance = ABS ((gssize) d->address - (gssize) caller->near_address);
    if (distance <= caller->max_distance)
    {
      dispatcher = d;
      break;
    }
  }

  if (dispatcher == NULL)
  {
    dispatcher = gum_code_deflector_dispatcher_new (caller);
    if (dispatcher == NULL)
      return NULL;
    self->dispatchers = g_slist_prepend (self->dispatchers, dispatcher);
  }

  impl = g_slice_new (GumCodeDeflectorImpl);

  deflector = &impl->parent;
  deflector->return_address = return_address;
  deflector->target = target;
  deflector->trampoline = dispatcher->trampoline;

  impl->allocator = self;

  dispatcher->callers = g_slist_prepend (dispatcher->callers, deflector);

  return deflector;
}

void
gum_code_deflector_free (GumCodeDeflector * deflector)
{
  GumCodeDeflectorImpl * impl = (GumCodeDeflectorImpl *) deflector;
  GumCodeAllocator * allocator = impl->allocator;
  GSList * cur;

  if (deflector == NULL)
    return;

  for (cur = allocator->dispatchers; cur != NULL; cur = cur->next)
  {
    GumCodeDeflectorDispatcher * dispatcher = cur->data;
    GSList * entry;

    entry = g_slist_find (dispatcher->callers, deflector);
    if (entry != NULL)
    {
      g_slice_free (GumCodeDeflectorImpl, impl);

      dispatcher->callers = g_slist_delete_link (dispatcher->callers, entry);
      if (dispatcher->callers == NULL)
      {
        gum_code_deflector_dispatcher_free (dispatcher);
        allocator->dispatchers = g_slist_remove (allocator->dispatchers,
            dispatcher);
      }

      return;
    }
  }

  g_assert_not_reached ();
}

static GumCodeDeflectorDispatcher *
gum_code_deflector_dispatcher_new (const GumAddressSpec * caller)
{
  GumCodeDeflectorDispatcher * dispatcher;
  GumProbeRangeForCodeCaveContext ctx;
  gsize size_in_pages, size_in_bytes;

  ctx.caller = caller;

  ctx.cave.base_address = 0;
  ctx.cave.size = 0;

#ifdef HAVE_LINUX
  gum_process_enumerate_ranges (GUM_PAGE_RX, gum_probe_range_for_code_cave,
      &ctx);
#endif

  if (ctx.cave.base_address == 0)
    return NULL;

  size_in_pages = 1;
  size_in_bytes = size_in_pages * gum_query_page_size ();

  dispatcher = g_slice_new (GumCodeDeflectorDispatcher);

  dispatcher->callers = NULL;

  dispatcher->address = GSIZE_TO_POINTER (ctx.cave.base_address);
  dispatcher->trampoline = dispatcher->address;
  dispatcher->thunk = gum_alloc_n_pages (size_in_pages, GUM_PAGE_RW);

  dispatcher->original_data = g_memdup (dispatcher->address, ctx.cave.size);
  dispatcher->original_size = ctx.cave.size;

  {
#ifdef HAVE_ARM
    GumThumbWriter tw;

    gum_thumb_writer_init (&tw, dispatcher->thunk);
    gum_thumb_writer_put_call_address_with_arguments (&tw,
        GUM_ADDRESS (gum_code_deflector_dispatcher_lookup), 2,
        GUM_ARG_ADDRESS, GUM_ADDRESS (dispatcher),
        GUM_ARG_REGISTER, ARM_REG_LR);
    gum_thumb_writer_put_bx_reg (&tw, ARM_REG_R0);
    gum_thumb_writer_flush (&tw);

    gum_code_deflector_dispatcher_ensure_rw (dispatcher);

    gum_thumb_writer_reset (&tw, dispatcher->address);
    gum_thumb_writer_put_ldr_reg_address (&tw, ARM_REG_R0,
        GUM_ADDRESS (dispatcher->thunk) + 1);
    gum_thumb_writer_put_bx_reg (&tw, ARM_REG_R0);
    gum_thumb_writer_flush (&tw);
    g_assert_cmpuint (gum_thumb_writer_offset (&tw),
        <=, dispatcher->original_size);
    gum_thumb_writer_free (&tw);

    dispatcher->trampoline = dispatcher->address + 1;
#else
    (void) gum_code_deflector_dispatcher_lookup;
#endif
  }

  gum_code_deflector_dispatcher_ensure_rx (dispatcher);
  gum_clear_cache (dispatcher->address, dispatcher->original_size);

  gum_mprotect (dispatcher->thunk, size_in_bytes, GUM_PAGE_RX);
  gum_clear_cache (dispatcher->thunk, size_in_bytes);

  return dispatcher;
}

static void
gum_code_deflector_dispatcher_free (GumCodeDeflectorDispatcher * dispatcher)
{
  gum_code_deflector_dispatcher_ensure_rw (dispatcher);
  memcpy (dispatcher->address, dispatcher->original_data,
      dispatcher->original_size);
  gum_code_deflector_dispatcher_ensure_rx (dispatcher);
  g_free (dispatcher->original_data);

  gum_free_pages (dispatcher->thunk);

  g_slist_foreach (dispatcher->callers, (GFunc) gum_code_deflector_free, NULL);
  g_slist_free (dispatcher->callers);

  g_slice_free (GumCodeDeflectorDispatcher, dispatcher);
}

static gpointer
gum_code_deflector_dispatcher_lookup (GumCodeDeflectorDispatcher * self,
                                      gpointer return_address)
{
  GSList * cur;

  for (cur = self->callers; cur != NULL; cur = cur->next)
  {
    GumCodeDeflector * caller = cur->data;

    if (caller->return_address == return_address)
      return caller->target;
  }

  return NULL;
}

static void
gum_code_deflector_dispatcher_ensure_rw (GumCodeDeflectorDispatcher * self)
{
  GumPageProtection prot;

  prot = gum_query_is_rwx_supported () ? GUM_PAGE_RWX : GUM_PAGE_RW;

  gum_mprotect (self->address, self->original_size, prot);
}

static void
gum_code_deflector_dispatcher_ensure_rx (GumCodeDeflectorDispatcher * self)
{
  gum_mprotect (self->address, self->original_size, GUM_PAGE_RX);
}

#ifdef HAVE_LINUX

static gboolean
gum_probe_range_for_code_cave (const GumRangeDetails * details,
                               gpointer user_data)
{
  GumProbeRangeForCodeCaveContext * ctx = user_data;
  const GumAddressSpec * caller = ctx->caller;
  GumAddress cave_address = details->range->base_address + 8;
  gsize distance;
  const guint8 empty_cave[8] = { 0, };

  distance = ABS ((gssize) cave_address - (gssize) caller->near_address);
  if (distance > caller->max_distance)
    return TRUE;

  if (memcmp (GSIZE_TO_POINTER (details->range->base_address), ELFMAG, SELFMAG)
      != 0)
    return TRUE;

  if (memcmp (GSIZE_TO_POINTER (cave_address), empty_cave, sizeof (empty_cave))
      != 0)
    return TRUE;

  ctx->cave.base_address = cave_address;
  ctx->cave.size = 8;
  return FALSE;
}

#endif
