/*
 * Copyright (C) 2010-2023 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2023 Håvard Sørbø <havard@hsorbo.no>
 * Copyright (C) 2023 Francesco Tamagni <mrmacete@protonmail.ch>
 * Copyright (C) 2023 Grant Douglas <me@hexplo.it>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumprocess-priv.h"

#include "backend-elf/gumprocess-elf.h"
#include "gum-init.h"
#include "gumandroid.h"
#include "gumelfmodule.h"
#include "gumlinux.h"
#include "gumlinux-priv.h"
#include "gummodulemap.h"
#include "valgrind.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_PTHREAD_ATTR_GETSTACK
# include <pthread.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_ASM_PRCTL_H
# include <asm/prctl.h>
#endif
#include <sys/prctl.h>
#include <sys/ptrace.h>
#ifdef HAVE_ASM_PTRACE_H
# include <asm/ptrace.h>
#endif
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_USER_H
# include <sys/user.h>
#endif

#define GUM_PAGE_START(value, page_size) \
    (GUM_ADDRESS (value) & ~GUM_ADDRESS (page_size - 1))

#ifndef O_CLOEXEC
# define O_CLOEXEC 0x80000
#endif

#define GUM_PSR_THUMB 0x20

#if defined (HAVE_I386)
# define GumRegs struct user_regs_struct
#elif defined (HAVE_ARM)
# define GumRegs struct pt_regs
#elif defined (HAVE_ARM64)
# define GumRegs struct user_pt_regs
#elif defined (HAVE_MIPS)
# define GumRegs struct pt_regs
#else
# error Unsupported architecture
#endif
#ifndef PTRACE_GETREGS
# define PTRACE_GETREGS 12
#endif
#ifndef PTRACE_SETREGS
# define PTRACE_SETREGS 13
#endif
#ifndef PTRACE_GETREGSET
# define PTRACE_GETREGSET 0x4204
#endif
#ifndef PTRACE_SETREGSET
# define PTRACE_SETREGSET 0x4205
#endif
#ifndef PR_SET_PTRACER
# define PR_SET_PTRACER 0x59616d61
#endif
#ifndef NT_PRSTATUS
# define NT_PRSTATUS 1
#endif

#define GUM_TEMP_FAILURE_RETRY(expression) \
    ({ \
      gssize __result; \
      \
      do __result = (gssize) (expression); \
      while (__result == -EINTR); \
      \
      __result; \
    })

typedef struct _GumProgramModules GumProgramModules;
typedef guint GumProgramRuntimeLinker;
typedef struct _GumProgramRanges GumProgramRanges;
typedef ElfW(auxv_t) * (* GumReadAuxvFunc) (void);

typedef struct _GumModifyThreadContext GumModifyThreadContext;
typedef guint8 GumModifyThreadAck;

typedef struct _GumEnumerateModulesContext GumEnumerateModulesContext;
typedef struct _GumResolveModuleNameContext GumResolveModuleNameContext;

typedef gint (* GumFoundDlPhdrFunc) (struct dl_phdr_info * info,
    gsize size, gpointer data);
typedef void (* GumDlIteratePhdrImpl) (GumFoundDlPhdrFunc func, gpointer data);

typedef struct _GumUserDesc GumUserDesc;
typedef struct _GumTcbHead GumTcbHead;

typedef gint (* GumCloneFunc) (gpointer arg);

struct _GumProgramModules
{
  GumModuleDetails program;
  GumModuleDetails interpreter;
  GumModuleDetails vdso;
  GumProgramRuntimeLinker rtld;
};

enum _GumProgramRuntimeLinker
{
  GUM_PROGRAM_RTLD_NONE,
  GUM_PROGRAM_RTLD_SHARED,
};

struct _GumProgramRanges
{
  GumMemoryRange program;
  GumMemoryRange interpreter;
  GumMemoryRange vdso;
};

enum _GumModifyThreadAck
{
  GUM_ACK_READY = 1,
  GUM_ACK_READ_CONTEXT,
  GUM_ACK_MODIFIED_CONTEXT,
  GUM_ACK_WROTE_CONTEXT,
  GUM_ACK_FAILED_TO_ATTACH,
  GUM_ACK_FAILED_TO_WAIT,
  GUM_ACK_FAILED_TO_STOP,
  GUM_ACK_FAILED_TO_READ,
  GUM_ACK_FAILED_TO_WRITE,
  GUM_ACK_FAILED_TO_DETACH
};

struct _GumModifyThreadContext
{
  gint fd[2];
  GumThreadId thread_id;
  GumCpuContext cpu_context;
};

struct _GumEnumerateModulesContext
{
  GumFoundModuleFunc func;
  gpointer user_data;

  GHashTable * named_ranges;
};

struct _GumEmitExecutableModuleContext
{
  const gchar * executable_path;
  GumFoundModuleFunc func;
  gpointer user_data;

  gboolean carry_on;
};

struct _GumResolveModuleNameContext
{
  const gchar * name;
  GumAddress known_address;
  gchar * path;
  GumAddress base;
};

struct _GumUserDesc
{
  guint entry_number;
  guint base_addr;
  guint limit;
  guint seg_32bit : 1;
  guint contents : 2;
  guint read_exec_only : 1;
  guint limit_in_pages : 1;
  guint seg_not_present : 1;
  guint useable : 1;
};

struct _GumTcbHead
{
#ifdef HAVE_I386
  gpointer tcb;
  gpointer dtv;
  gpointer self;
#else
  gpointer dtv;
  gpointer priv;
#endif
};

static void gum_deinit_program_modules (void);
static gboolean gum_query_program_ranges (GumReadAuxvFunc read_auxv,
    GumProgramRanges * ranges);
static ElfW(auxv_t) * gum_read_auxv_from_proc (void);
static ElfW(auxv_t) * gum_read_auxv_from_stack (void);
static gboolean gum_query_main_thread_stack_range (GumMemoryRange * range);
static void gum_compute_elf_range_from_ehdr (const ElfW(Ehdr) * ehdr,
    GumMemoryRange * range);
static void gum_compute_elf_range_from_phdrs (const ElfW(Phdr) * phdrs,
    ElfW(Half) phdr_size, ElfW(Half) phdr_count, GumAddress base_address,
    GumMemoryRange * range);

static gchar * gum_try_init_libc_name (void);
static gboolean gum_try_resolve_dynamic_symbol (const gchar * name,
    Dl_info * info);
static void gum_deinit_libc_name (void);

static gint gum_do_modify_thread (gpointer data);
static gboolean gum_await_ack (gint fd, GumModifyThreadAck expected_ack);
static void gum_put_ack (gint fd, GumModifyThreadAck ack);

static void gum_store_cpu_context (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);

static void gum_do_enumerate_modules (const gchar * libc_name,
    GumFoundModuleFunc func, gpointer user_data);
static void gum_process_enumerate_modules_by_using_libc (
    GumDlIteratePhdrImpl iterate_phdr, GumFoundModuleFunc func,
    gpointer user_data);
static gint gum_emit_module_from_phdr (struct dl_phdr_info * info, gsize size,
    gpointer user_data);

static void gum_linux_named_range_free (GumLinuxNamedRange * range);
static gboolean gum_try_translate_vdso_name (gchar * name);
static void * gum_module_get_handle (const gchar * module_name);
static void * gum_module_get_symbol (void * module, const gchar * symbol_name);

static gboolean gum_do_resolve_module_name (const gchar * name,
    const gchar * libc_name, gchar ** path, GumAddress * base);
static gboolean gum_store_module_path_and_base_if_match (
    const GumModuleDetails * details, gpointer user_data);

static void gum_proc_maps_iter_init_for_path (GumProcMapsIter * iter,
    const gchar * path);

static void gum_acquire_dumpability (void);
static void gum_release_dumpability (void);

static gchar * gum_thread_read_name (GumThreadId thread_id);
static gboolean gum_thread_read_state (GumThreadId tid, GumThreadState * state);
static GumThreadState gum_thread_state_from_proc_status_character (gchar c);
static GumPageProtection gum_page_protection_from_proc_perms_string (
    const gchar * perms);

static gssize gum_get_regs (pid_t pid, GumRegs * regs);
static gssize gum_set_regs (pid_t pid, const GumRegs * regs);

static void gum_parse_regs (const GumRegs * regs, GumCpuContext * ctx);
static void gum_unparse_regs (const GumCpuContext * ctx, GumRegs * regs);

static gssize gum_libc_clone (GumCloneFunc child_func, gpointer child_stack,
    gint flags, gpointer arg, pid_t * parent_tidptr, GumUserDesc * tls,
    pid_t * child_tidptr);
static gssize gum_libc_read (gint fd, gpointer buf, gsize count);
static gssize gum_libc_write (gint fd, gconstpointer buf, gsize count);
static pid_t gum_libc_waitpid (pid_t pid, int * status, int options);
static gssize gum_libc_ptrace (gsize request, pid_t pid, gpointer address,
    gpointer data);

#define gum_libc_syscall_3(n, a, b, c) gum_libc_syscall_4 (n, a, b, c, 0)
static gssize gum_libc_syscall_4 (gsize n, gsize a, gsize b, gsize c, gsize d);

static GumProgramModules gum_program_modules;
static gchar * gum_libc_name;

static gboolean gum_is_regset_supported = TRUE;

G_LOCK_DEFINE_STATIC (gum_dumpable);
static gint gum_dumpable_refcount = 0;
static gint gum_dumpable_previous = 0;

static const GumProgramModules *
gum_query_program_modules (void)
{
  static gsize modules_value = 0;

  if (g_once_init_enter (&modules_value))
  {
    static GumProgramRanges ranges;
    gboolean got_kern, got_user;
    GumProgramRanges kern, user;
    GumProcMapsIter iter;
    gchar * path;
    const gchar * line;

    got_kern = gum_query_program_ranges (gum_read_auxv_from_proc, &kern);
    got_user = gum_query_program_ranges (gum_read_auxv_from_stack, &user);
    if (got_kern && got_user &&
        user.program.base_address != kern.program.base_address)
    {
      ranges = user;
      ranges.interpreter = kern.program;
    }
    else if (got_kern)
      ranges = kern;
    else
      ranges = user;

    gum_program_modules.program.range = &ranges.program;
    gum_program_modules.interpreter.range = &ranges.interpreter;
    gum_program_modules.vdso.range = &ranges.vdso;
    gum_program_modules.rtld = (ranges.interpreter.base_address == 0)
        ? GUM_PROGRAM_RTLD_NONE
        : GUM_PROGRAM_RTLD_SHARED;

    gum_proc_maps_iter_init_for_self (&iter);
    path = g_malloc (PATH_MAX);

    while (gum_proc_maps_iter_next (&iter, &line))
    {
      GumAddress start;
      GumModuleDetails * m;

      sscanf (line, "%" G_GINT64_MODIFIER "x-", &start);

      if (start == ranges.program.base_address)
        m = &gum_program_modules.program;
      else if (start == ranges.interpreter.base_address)
        m = &gum_program_modules.interpreter;
      else
        continue;

      sscanf (line, "%*x-%*x %*c%*c%*c%*c %*x %*s %*d %[^\n]", path);

      m->path = g_strdup (path);
      m->name = strrchr (m->path, '/');
      if (m->name != NULL)
        m->name++;
      else
        m->name = m->path;
    }

    g_free (path);
    gum_proc_maps_iter_destroy (&iter);

    if (ranges.vdso.base_address != 0)
    {
      GumModuleDetails * m = &gum_program_modules.vdso;
      /* FIXME: Parse soname instead of hardcoding: */
      m->path = g_strdup ("linux-vdso.so.1");
      m->name = m->path;
    }

    _gum_register_destructor (gum_deinit_program_modules);

    g_once_init_leave (&modules_value, GPOINTER_TO_SIZE (&gum_program_modules));
  }

  return GSIZE_TO_POINTER (modules_value);
}

static void
gum_deinit_program_modules (void)
{
  GumProgramModules * m = &gum_program_modules;

  g_free ((gchar *) m->program.path);
  g_free ((gchar *) m->interpreter.path);
  g_free ((gchar *) m->vdso.path);
}

static gboolean
gum_query_program_ranges (GumReadAuxvFunc read_auxv,
                          GumProgramRanges * ranges)
{
  gboolean success = FALSE;
  ElfW(auxv_t) * auxv;
  const ElfW(Phdr) * phdrs;
  ElfW(Half) phdr_size, phdr_count;
  const ElfW(Ehdr) * interpreter, * vdso;
  ElfW(auxv_t) * entry;

  bzero (ranges, sizeof (GumProgramRanges));

  auxv = read_auxv ();
  if (auxv == NULL)
    goto beach;

  phdrs = NULL;
  phdr_size = 0;
  phdr_count = 0;
  interpreter = NULL;
  vdso = NULL;
  for (entry = auxv; entry->a_type != AT_NULL; entry++)
  {
    switch (entry->a_type)
    {
      case AT_PHDR:
        phdrs = (ElfW(Phdr) *) entry->a_un.a_val;
        break;
      case AT_PHENT:
        phdr_size = entry->a_un.a_val;
        break;
      case AT_PHNUM:
        phdr_count = entry->a_un.a_val;
        break;
      case AT_BASE:
        interpreter = (const ElfW(Ehdr) *) entry->a_un.a_val;
        break;
      case AT_SYSINFO_EHDR:
        vdso = (const ElfW(Ehdr) *) entry->a_un.a_val;
        break;
    }
  }
  if (phdrs == NULL || phdr_size == 0 || phdr_count == 0)
    goto beach;

  gum_compute_elf_range_from_phdrs (phdrs, phdr_size, phdr_count, 0,
      &ranges->program);
  gum_compute_elf_range_from_ehdr (interpreter, &ranges->interpreter);
  gum_compute_elf_range_from_ehdr (vdso, &ranges->vdso);

  success = TRUE;

beach:
  g_free (auxv);

  return success;
}

static ElfW(auxv_t) *
gum_read_auxv_from_proc (void)
{
  ElfW(auxv_t) * auxv = NULL;

  gum_acquire_dumpability ();

  g_file_get_contents ("/proc/self/auxv", (gchar **) &auxv, NULL, NULL);

  gum_release_dumpability ();

  return auxv;
}

static ElfW(auxv_t) *
gum_read_auxv_from_stack (void)
{
  GumMemoryRange stack;
  gpointer stack_start, stack_end;
  ElfW(auxv_t) needle;
  const ElfW(auxv_t) * match, * last_match;
  gsize offset;
  const ElfW(auxv_t) * cursor, * auxv_start, * auxv_end;
  gsize page_size;

  if (!gum_query_main_thread_stack_range (&stack))
    return NULL;
  stack_start = GSIZE_TO_POINTER (stack.base_address);
  stack_end = stack_start + stack.size;

  needle.a_type = AT_PHENT;
  needle.a_un.a_val = sizeof (ElfW(Phdr));

  match = NULL;
  last_match = NULL;
  offset = 0;
  while (offset != stack.size)
  {
    match = memmem (GSIZE_TO_POINTER (stack.base_address) + offset,
        stack.size - offset, &needle, sizeof (needle));
    if (match == NULL)
      break;

    last_match = match;
    offset = (GUM_ADDRESS (match) - stack.base_address) + 1;
  }
  if (last_match == NULL)
    return NULL;

  auxv_start = NULL;
  page_size = gum_query_page_size ();
  for (cursor = last_match - 1;
      (gpointer) cursor >= stack_start;
      cursor--)
  {
    gboolean probably_an_invalid_type = cursor->a_type >= page_size;
    if (probably_an_invalid_type)
    {
      auxv_start = cursor + 1;
      break;
    }
  }

  auxv_end = NULL;
  for (cursor = last_match + 1;
      (gpointer) cursor <= stack_end - sizeof (ElfW(auxv_t));
      cursor++)
  {
    if (cursor->a_type == AT_NULL)
    {
      auxv_end = cursor + 1;
      break;
    }
  }
  if (auxv_end == NULL)
    return NULL;

  return g_memdup (auxv_start, (guint8 *) auxv_end - (guint8 *) auxv_start);
}

static gboolean
gum_query_main_thread_stack_range (GumMemoryRange * range)
{
  GumProcMapsIter iter;
  GumAddress stack_bottom, stack_top;
  const gchar * line;

  gum_proc_maps_iter_init_for_self (&iter);

  stack_bottom = 0;
  stack_top = 0;

  while (gum_proc_maps_iter_next (&iter, &line))
  {
    if (g_str_has_suffix (line, " [stack]"))
    {
      sscanf (line,
          "%" G_GINT64_MODIFIER "x-%" G_GINT64_MODIFIER "x ",
          &stack_bottom,
          &stack_top);
      break;
    }
  }

  range->base_address = stack_bottom;
  range->size = stack_top - stack_bottom;

  gum_proc_maps_iter_destroy (&iter);

  return range->size != 0;
}

static void
gum_compute_elf_range_from_ehdr (const ElfW(Ehdr) * ehdr,
                                 GumMemoryRange * range)
{
  if (ehdr == NULL)
  {
    range->base_address = 0;
    range->size = 0;
    return;
  }

  gum_compute_elf_range_from_phdrs ((gconstpointer) ehdr + ehdr->e_phoff,
      ehdr->e_phentsize, ehdr->e_phnum, GUM_ADDRESS (ehdr), range);
}

static void
gum_compute_elf_range_from_phdrs (const ElfW(Phdr) * phdrs,
                                  ElfW(Half) phdr_size,
                                  ElfW(Half) phdr_count,
                                  GumAddress base_address,
                                  GumMemoryRange * range)
{
  GumAddress lowest, highest;
  gsize page_size;
  ElfW(Half) i;
  const ElfW(Phdr) * phdr;

  range->base_address = 0;

  lowest = ~0;
  highest = 0;
  page_size = gum_query_page_size ();

  for (i = 0, phdr = phdrs;
      i != phdr_count;
      i++, phdr = (gconstpointer) phdr + phdr_size)
  {
    if (phdr->p_type == PT_PHDR)
      range->base_address = GPOINTER_TO_SIZE (phdrs) - phdr->p_offset;

    if (phdr->p_type == PT_LOAD && phdr->p_offset == 0)
    {
      if (range->base_address == 0)
        range->base_address = phdr->p_vaddr;
    }

    if (phdr->p_type == PT_LOAD)
    {
      lowest = MIN (GUM_PAGE_START (phdr->p_vaddr, page_size), lowest);
      highest = MAX (phdr->p_vaddr + phdr->p_memsz, highest);
    }
  }

  if (range->base_address == 0)
  {
    range->base_address = (base_address != 0)
        ? base_address
        : GUM_PAGE_START (phdrs, page_size);
  }

  range->size = highest - lowest;
}

const gchar *
gum_process_query_libc_name (void)
{
  static GOnce once = G_ONCE_INIT;

  g_once (&once, (GThreadFunc) gum_try_init_libc_name, NULL);

  if (once.retval == NULL)
    gum_panic ("Unable to locate the libc; please file a bug");

  return once.retval;
}

static gchar *
gum_try_init_libc_name (void)
{
  Dl_info info;

#ifndef HAVE_ANDROID
  if (!gum_try_resolve_dynamic_symbol ("__libc_start_main", &info))
#endif
  {
    if (!gum_try_resolve_dynamic_symbol ("exit", &info))
      return NULL;
  }

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (g_path_is_absolute (info.dli_fname))
  {
    gum_libc_name = g_strdup (info.dli_fname);
  }
  else
  {
    gum_libc_name = g_build_filename (
        "/system",
        (sizeof (gpointer) == 4) ? "lib" : "lib64",
        info.dli_fname,
        NULL);
  }
#else
  {
    GumAddress base;
    gum_do_resolve_module_name (info.dli_fname, info.dli_fname, &gum_libc_name,
        &base);
  }
#endif

  _gum_register_destructor (gum_deinit_libc_name);

  return gum_libc_name;
}

static gboolean
gum_try_resolve_dynamic_symbol (const gchar * name,
                                Dl_info * info)
{
  gpointer address;

  address = dlsym (RTLD_NEXT, name);
  if (address == NULL)
    address = dlsym (RTLD_DEFAULT, name);
  if (address == NULL)
    return FALSE;

  return dladdr (address, info) != 0;
}

static void
gum_deinit_libc_name (void)
{
  g_free (gum_libc_name);
}

gboolean
gum_process_is_debugger_attached (void)
{
  gboolean result;
  gchar * status, * p;

  status = NULL;
  g_file_get_contents ("/proc/self/status", &status, NULL, NULL);

  p = strstr (status, "TracerPid:");
  g_assert (p != NULL);

  result = atoi (p + 10) != 0;

  g_free (status);

  return result;
}

GumProcessId
gum_process_get_id (void)
{
  return getpid ();
}

GumThreadId
gum_process_get_current_thread_id (void)
{
  return syscall (__NR_gettid);
}

gboolean
gum_process_has_thread (GumThreadId thread_id)
{
  gchar path[16 + 20 + 1];
  sprintf (path, "/proc/self/task/%" G_GSIZE_MODIFIER "u", thread_id);

  return g_file_test (path, G_FILE_TEST_EXISTS);
}

gboolean
gum_process_modify_thread (GumThreadId thread_id,
                           GumModifyThreadFunc func,
                           gpointer user_data,
                           GumModifyThreadFlags flags)
{
  gboolean success = FALSE;

  if (thread_id == gum_process_get_current_thread_id ())
  {
    /*
     * getcontext/setcontext is not supported on the musl C-runtime (or
     * Android). musl, however doesn't provide any pre-processor definitions
     * which allow it to be readily identified. However, the other major
     * runtimes do, so we use their absence to determine that musl is in use and
     * hence omit the block.
     */
#if !defined (HAVE_ANDROID) && (defined (__GLIBC__) || defined (__UCLIBC__)) \
    && (!defined (__GLIBC__) || !(defined (__stub_getcontext) \
        || defined (__stub_setcontext)))
    ucontext_t uc;
    volatile gboolean modified = FALSE;

    getcontext (&uc);
    if (!modified)
    {
      GumCpuContext cpu_context;

      gum_linux_parse_ucontext (&uc, &cpu_context);
      func (thread_id, &cpu_context, user_data);
      gum_linux_unparse_ucontext (&cpu_context, &uc);

      modified = TRUE;
      setcontext (&uc);
    }

    success = TRUE;
#endif
  }
  else
  {
    GumModifyThreadContext ctx;
    gint fd;
    gssize child;
    gpointer stack, tls;
    GumUserDesc * desc;

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, ctx.fd) != 0)
      return FALSE;
    ctx.thread_id = thread_id;

    fd = ctx.fd[0];

    stack = gum_alloc_n_pages (1, GUM_PAGE_RW);
    tls = gum_alloc_n_pages (1, GUM_PAGE_RW);

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
    GumUserDesc segment;
    gint gs;

    asm volatile (
        "movw %%gs, %w0"
        : "=q" (gs)
    );

    segment.entry_number = (gs & 0xffff) >> 3;
    segment.base_addr = GPOINTER_TO_SIZE (tls);
    segment.limit = 0xfffff;
    segment.seg_32bit = 1;
    segment.contents = 0;
    segment.read_exec_only = 0;
    segment.limit_in_pages = 1;
    segment.seg_not_present = 0;
    segment.useable = 1;

    desc = &segment;
#else
    desc = tls;
#endif

#if defined (HAVE_I386)
    {
      GumTcbHead * head = tls;

      head->tcb = tls;
      head->dtv = GSIZE_TO_POINTER (GPOINTER_TO_SIZE (tls) + 1024);
      head->self = tls;
    }
#endif

    /*
     * It seems like the only reliable way to read/write the registers of
     * another thread is to use ptrace(). We used to accomplish this by
     * hi-jacking the target thread by installing a signal handler and sending a
     * real-time signal directed at the target thread, and thus relying on the
     * signal handler getting called in that thread. The signal handler would
     * then provide us with read/write access to its registers. This hack would
     * however not work if a thread was for example blocking in poll(), as the
     * signal would then just get queued and we'd end up waiting indefinitely.
     *
     * It is however not possible to ptrace() another thread when we're in the
     * same process group. This used to be supported in old kernels, but it was
     * buggy and eventually dropped. So in order to use ptrace() we will need to
     * spawn a new thread in a different process group so that it can ptrace()
     * the target thread inside our process group. This is also the solution
     * recommended by Linus:
     *
     * https://lkml.org/lkml/2006/9/1/217
     *
     * Because libc implementations don't expose an API to do this, and the
     * thread setup code is private, where the TLS part is crucial for even just
     * the syscall wrappers - due to them accessing `errno` - we cannot make any
     * libc calls in this thread. And because the libc's clone() syscall wrapper
     * typically writes to the child thread's TLS structures, which we cannot
     * portably set up correctly, we cannot use the libc clone() syscall wrapper
     * either.
     */
    child = gum_libc_clone (
        gum_do_modify_thread,
        stack + gum_query_page_size (),
        CLONE_VM | CLONE_SETTLS,
        &ctx,
        NULL,
        desc,
        NULL);
    if (child == -1)
      goto beach;

    gum_acquire_dumpability ();

    prctl (PR_SET_PTRACER, child);

    gum_put_ack (fd, GUM_ACK_READY);

    if (gum_await_ack (fd, GUM_ACK_READ_CONTEXT))
    {
      func (thread_id, &ctx.cpu_context, user_data);
      gum_put_ack (fd, GUM_ACK_MODIFIED_CONTEXT);

      success = gum_await_ack (fd, GUM_ACK_WROTE_CONTEXT);
    }

    gum_release_dumpability ();

    waitpid (child, NULL, __WCLONE);

beach:
    gum_free_pages (tls);
    gum_free_pages (stack);

    close (ctx.fd[0]);
    close (ctx.fd[1]);
  }

  return success;
}

static gint
gum_do_modify_thread (gpointer data)
{
  GumModifyThreadContext * ctx = data;
  gint fd;
  gboolean attached = FALSE;
  gssize res;
  pid_t wait_result;
  int status;
  GumRegs regs;

  fd = ctx->fd[1];

  gum_await_ack (fd, GUM_ACK_READY);

  res = gum_libc_ptrace (PTRACE_ATTACH, ctx->thread_id, NULL, NULL);
  if (res == -1)
    goto failed_to_attach;
  attached = TRUE;

  wait_result = gum_libc_waitpid (ctx->thread_id, &status, __WALL);

  if (wait_result != ctx->thread_id)
    goto failed_to_wait;

  if (!WIFSTOPPED (status))
    goto failed_to_stop;

  /*
   * Although ptrace injects SIGSTOP into our process, it is possible that our
   * target is stopped by another stop signal (e.g. SIGTTIN). The man pages for
   * ptrace mention the possible race condition. For our purposes, however, we
   * only require that the target is stopped so that we can read its registers.
   */
  res = gum_get_regs (ctx->thread_id, &regs);
  if (res == -1)
    goto failed_to_read;
  gum_parse_regs (&regs, &ctx->cpu_context);
  gum_put_ack (fd, GUM_ACK_READ_CONTEXT);

  gum_await_ack (fd, GUM_ACK_MODIFIED_CONTEXT);
  gum_unparse_regs (&ctx->cpu_context, &regs);
  res = gum_set_regs (ctx->thread_id, &regs);
  if (res == -1)
    goto failed_to_write;

  res = gum_libc_ptrace (PTRACE_DETACH, ctx->thread_id, NULL,
      GINT_TO_POINTER (SIGCONT));

  attached = FALSE;
  if (res == -1)
    goto failed_to_detach;

  gum_put_ack (fd, GUM_ACK_WROTE_CONTEXT);

  goto beach;

failed_to_attach:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_ATTACH);
    goto beach;
  }
failed_to_wait:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_WAIT);
    goto beach;
  }
failed_to_stop:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_STOP);
    goto beach;
  }
failed_to_read:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_READ);
    goto beach;
  }
failed_to_write:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_WRITE);
    goto beach;
  }
failed_to_detach:
  {
    gum_put_ack (fd, GUM_ACK_FAILED_TO_DETACH);
    goto beach;
  }
beach:
  {
    if (attached)
    {
      gum_libc_ptrace (PTRACE_DETACH, ctx->thread_id, NULL,
          GINT_TO_POINTER (SIGCONT));
    }

    return 0;
  }
}

static gboolean
gum_await_ack (gint fd,
               GumModifyThreadAck expected_ack)
{
  guint8 value;
  gssize res;

  res = GUM_TEMP_FAILURE_RETRY (gum_libc_read (fd, &value, sizeof (value)));
  if (res == -1)
    return FALSE;

  return value == expected_ack;
}

static void
gum_put_ack (gint fd,
             GumModifyThreadAck ack)
{
  guint8 value;

  value = ack;
  GUM_TEMP_FAILURE_RETRY (gum_libc_write (fd, &value, sizeof (value)));
}

void
_gum_process_enumerate_threads (GumFoundThreadFunc func,
                                gpointer user_data)
{
  GDir * dir;
  const gchar * name;
  gboolean carry_on = TRUE;

  dir = g_dir_open ("/proc/self/task", 0, NULL);
  g_assert (dir != NULL);

  while (carry_on && (name = g_dir_read_name (dir)) != NULL)
  {
    GumThreadDetails details;
    gchar * thread_name;

    details.id = atoi (name);

    thread_name = gum_thread_read_name (details.id);
    details.name = thread_name;

    if (gum_thread_read_state (details.id, &details.state))
    {
      if (gum_process_modify_thread (details.id, gum_store_cpu_context,
            &details.cpu_context, GUM_MODIFY_THREAD_FLAGS_ABORT_SAFELY))
      {
        carry_on = func (&details, user_data);
      }
    }

    g_free (thread_name);
  }

  g_dir_close (dir);
}

static void
gum_store_cpu_context (GumThreadId thread_id,
                       GumCpuContext * cpu_context,
                       gpointer user_data)
{
  memcpy (user_data, cpu_context, sizeof (GumCpuContext));
}

gboolean
_gum_process_collect_main_module (const GumModuleDetails * details,
                                  gpointer user_data)
{
  GumModuleDetails ** out = user_data;

  *out = gum_module_details_copy (details);

  return FALSE;
}

void
_gum_process_enumerate_modules (GumFoundModuleFunc func,
                                gpointer user_data)
{
  gum_do_enumerate_modules (gum_process_query_libc_name (), func, user_data);
}

static void
gum_do_enumerate_modules (const gchar * libc_name,
                          GumFoundModuleFunc func,
                          gpointer user_data)
{
  const GumProgramModules * pm;
  static gsize iterate_phdr_value = 0;
  GumDlIteratePhdrImpl iterate_phdr;

  pm = gum_query_program_modules ();

  if (pm->rtld == GUM_PROGRAM_RTLD_NONE)
  {
    if (!func (&pm->program, user_data))
      return;

    if (pm->vdso.range->base_address != 0)
      func (&pm->vdso, user_data);

    return;
  }

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE)
  {
    gum_android_enumerate_modules (func, user_data);
    return;
  }
#endif

  if (g_once_init_enter (&iterate_phdr_value))
  {
    GumAddress impl;

    impl = gum_module_find_export_by_name (libc_name, "dl_iterate_phdr");

    g_once_init_leave (&iterate_phdr_value, impl + 1);
  }

  iterate_phdr = GSIZE_TO_POINTER (iterate_phdr_value - 1);
  if (iterate_phdr != NULL)
  {
    gum_process_enumerate_modules_by_using_libc (iterate_phdr, func, user_data);
  }
  else
  {
    gum_linux_enumerate_modules_using_proc_maps (func, user_data);
  }
}

static void
gum_process_enumerate_modules_by_using_libc (GumDlIteratePhdrImpl iterate_phdr,
                                             GumFoundModuleFunc func,
                                             gpointer user_data)
{
  GumEnumerateModulesContext ctx;

  ctx.func = func;
  ctx.user_data = user_data;

  ctx.named_ranges = gum_linux_collect_named_ranges ();

  iterate_phdr (gum_emit_module_from_phdr, &ctx);

  g_hash_table_unref (ctx.named_ranges);
}

static gint
gum_emit_module_from_phdr (struct dl_phdr_info * info,
                           gsize size,
                           gpointer user_data)
{
  GumEnumerateModulesContext * ctx = user_data;
  GumMemoryRange range;
  GumLinuxNamedRange * named_range;
  const gchar * path;
  gchar * name;
  GumModuleDetails details;
  gboolean carry_on;

  gum_compute_elf_range_from_phdrs (info->dlpi_phdr, sizeof (ElfW(Phdr)),
      info->dlpi_phnum, 0, &range);

  named_range = g_hash_table_lookup (ctx->named_ranges,
      GSIZE_TO_POINTER (range.base_address));

  path = (named_range != NULL) ? named_range->name : info->dlpi_name;
  name = g_path_get_basename (path);

  details.name = name;
  details.range = &range;
  details.path = path;

  carry_on = ctx->func (&details, ctx->user_data);

  g_free (name);

  return carry_on ? 0 : 1;
}

void
gum_linux_enumerate_modules_using_proc_maps (GumFoundModuleFunc func,
                                             gpointer user_data)
{
  GumProcMapsIter iter;
  gchar * path, * next_path;
  const gchar * line;
  gboolean carry_on = TRUE;
  gboolean got_line = FALSE;

  gum_proc_maps_iter_init_for_self (&iter);

  path = g_malloc (PATH_MAX);
  next_path = g_malloc (PATH_MAX);

  do
  {
    const guint8 elf_magic[] = { 0x7f, 'E', 'L', 'F' };
    GumModuleDetails details;
    GumMemoryRange range;
    GumAddress end;
    gchar perms[5] = { 0, };
    gint n;
    gboolean is_vdso, readable, shared;
    gchar * name;

    if (!got_line)
    {
      if (!gum_proc_maps_iter_next (&iter, &line))
        break;
    }
    else
    {
      got_line = FALSE;
    }

    n = sscanf (line,
        "%" G_GINT64_MODIFIER "x-%" G_GINT64_MODIFIER "x "
        "%4c "
        "%*x %*s %*d "
        "%[^\n]",
        &range.base_address, &end,
        perms,
        path);
    if (n == 3)
      continue;
    g_assert (n == 4);

    is_vdso = gum_try_translate_vdso_name (path);

    readable = perms[0] == 'r';
    shared = perms[3] == 's';
    if (!readable || shared)
      continue;
    else if ((path[0] != '/' && !is_vdso) || g_str_has_prefix (path, "/dev/"))
      continue;
    else if (RUNNING_ON_VALGRIND && strstr (path, "/valgrind/") != NULL)
      continue;
    else if (memcmp (GSIZE_TO_POINTER (range.base_address), elf_magic,
        sizeof (elf_magic)) != 0)
      continue;

    name = g_path_get_basename (path);

    range.size = end - range.base_address;

    details.name = name;
    details.range = &range;
    details.path = path;

    while (gum_proc_maps_iter_next (&iter, &line))
    {
      n = sscanf (line,
          "%*x-%" G_GINT64_MODIFIER "x %*c%*c%*c%*c %*x %*s %*d %[^\n]",
          &end,
          next_path);
      if (n == 1)
      {
        continue;
      }
      else if (n == 2 && next_path[0] == '[')
      {
        if (!gum_try_translate_vdso_name (next_path))
          continue;
      }

      if (n == 2 && strcmp (next_path, path) == 0)
      {
        range.size = end - range.base_address;
      }
      else
      {
        got_line = TRUE;
        break;
      }
    }

    carry_on = func (&details, user_data);

    g_free (name);
  }
  while (carry_on);

  g_free (path);
  g_free (next_path);

  gum_proc_maps_iter_destroy (&iter);
}

GHashTable *
gum_linux_collect_named_ranges (void)
{
  GHashTable * result;
  GumProcMapsIter iter;
  gchar * name, * next_name;
  const gchar * line;
  gboolean carry_on = TRUE;
  gboolean got_line = FALSE;

  result = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gum_linux_named_range_free);

  gum_proc_maps_iter_init_for_self (&iter);

  name = g_malloc (PATH_MAX);
  next_name = g_malloc (PATH_MAX);

  do
  {
    GumAddress start, end;
    gsize size;
    gint n;
    GumLinuxNamedRange * range;

    if (!got_line)
    {
      if (!gum_proc_maps_iter_next (&iter, &line))
        break;
    }
    else
    {
      got_line = FALSE;
    }

    n = sscanf (line,
        "%" G_GINT64_MODIFIER "x-%" G_GINT64_MODIFIER "x "
        "%*4c "
        "%*x %*s %*d "
        "%[^\n]",
        &start, &end,
        name);
    if (n == 2)
      continue;
    g_assert (n == 3);

    gum_try_translate_vdso_name (name);

    size = end - start;

    while (gum_proc_maps_iter_next (&iter, &line))
    {
      n = sscanf (line,
          "%*x-%" G_GINT64_MODIFIER "x %*c%*c%*c%*c %*x %*s %*d %[^\n]",
          &end,
          next_name);
      if (n == 1)
      {
        continue;
      }
      else if (n == 2 && next_name[0] == '[')
      {
        if (!gum_try_translate_vdso_name (next_name))
          continue;
      }

      if (n == 2 && strcmp (next_name, name) == 0)
      {
        size = end - start;
      }
      else
      {
        got_line = TRUE;
        break;
      }
    }

    range = g_slice_new (GumLinuxNamedRange);

    range->name = g_strdup (name);
    range->base = GSIZE_TO_POINTER (start);
    range->size = size;

    g_hash_table_insert (result, range->base, range);
  }
  while (carry_on);

  g_free (name);
  g_free (next_name);

  gum_proc_maps_iter_destroy (&iter);

  return result;
}

static void
gum_linux_named_range_free (GumLinuxNamedRange * range)
{
  g_free ((gpointer) range->name);

  g_slice_free (GumLinuxNamedRange, range);
}

static gboolean
gum_try_translate_vdso_name (gchar * name)
{
  if (strcmp (name, "[vdso]") == 0)
  {
    strcpy (name, "linux-vdso.so.1");
    return TRUE;
  }

  return FALSE;
}

void
_gum_process_enumerate_ranges (GumPageProtection prot,
                               GumFoundRangeFunc func,
                               gpointer user_data)
{
  gum_linux_enumerate_ranges (getpid (), prot, func, user_data);
}

void
gum_linux_enumerate_ranges (pid_t pid,
                            GumPageProtection prot,
                            GumFoundRangeFunc func,
                            gpointer user_data)
{
  GumProcMapsIter iter;
  gboolean carry_on = TRUE;
  const gchar * line;

  gum_proc_maps_iter_init_for_pid (&iter, pid);

  while (carry_on && gum_proc_maps_iter_next (&iter, &line))
  {
    GumRangeDetails details;
    GumMemoryRange range;
    GumFileMapping file;
    GumAddress end;
    gchar perms[5] = { 0, };
    guint64 inode;
    gint length;

    sscanf (line,
        "%" G_GINT64_MODIFIER "x-%" G_GINT64_MODIFIER "x "
        "%4c "
        "%" G_GINT64_MODIFIER "x %*s %" G_GINT64_MODIFIER "d"
        "%n",
        &range.base_address, &end,
        perms,
        &file.offset, &inode,
        &length);

    range.size = end - range.base_address;

    details.file = NULL;
    if (inode != 0)
    {
      file.path = strchr (line + length, '/');
      if (file.path != NULL)
      {
        details.file = &file;
        file.size = 0; /* TODO */

        if (RUNNING_ON_VALGRIND && strstr (file.path, "/valgrind/") != NULL)
          continue;
      }
    }

    details.range = &range;
    details.protection = gum_page_protection_from_proc_perms_string (perms);

    if ((details.protection & prot) == prot)
    {
      carry_on = func (&details, user_data);
    }
  }

  gum_proc_maps_iter_destroy (&iter);
}

void
gum_process_enumerate_malloc_ranges (GumFoundMallocRangeFunc func,
                                     gpointer user_data)
{
  /* Not implemented */
}

guint
gum_thread_try_get_ranges (GumMemoryRange * ranges,
                           guint max_length)
{
#ifdef HAVE_PTHREAD_ATTR_GETSTACK
  guint n = 0;
  pthread_attr_t attr;
  gboolean allocated = FALSE;
  void * stack_addr;
  size_t stack_size;
  GumMemoryRange * range;

  if (pthread_getattr_np (pthread_self (), &attr) != 0)
    goto beach;
  allocated = TRUE;

  if (pthread_attr_getstack (&attr, &stack_addr, &stack_size) != 0)
    goto beach;

  range = &ranges[0];
  range->base_address = GUM_ADDRESS (stack_addr);
  range->size = stack_size;

  n = 1;

beach:
  if (allocated)
    pthread_attr_destroy (&attr);

  return n;
#else
  return 0;
#endif
}

gint
gum_thread_get_system_error (void)
{
  return errno;
}

void
gum_thread_set_system_error (gint value)
{
  errno = value;
}

gboolean
gum_thread_suspend (GumThreadId thread_id,
                    GError ** error)
{
  if (syscall (__NR_tgkill, getpid (), thread_id, SIGSTOP) != 0)
    goto failure;

  return TRUE;

failure:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_FAILED, "%s", g_strerror (errno));
    return FALSE;
  }
}

gboolean
gum_thread_resume (GumThreadId thread_id,
                   GError ** error)
{
  if (syscall (__NR_tgkill, getpid (), thread_id, SIGCONT) != 0)
    goto failure;

  return TRUE;

failure:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_FAILED, "%s", g_strerror (errno));
    return FALSE;
  }
}

gboolean
gum_module_load (const gchar * module_name,
                 GError ** error)
{
  GumGenericDlopenImpl dlopen_impl = dlopen;

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_module_get_handle (module_name) != NULL)
    return TRUE;

  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE)
    gum_android_find_unrestricted_dlopen (&dlopen_impl);
#endif

  if (dlopen_impl (module_name, RTLD_LAZY) == NULL)
    goto not_found;

  return TRUE;

not_found:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_FOUND, "%s", dlerror ());
    return FALSE;
  }
}

static void *
gum_module_get_handle (const gchar * module_name)
{
#if defined (HAVE_MUSL)
  struct link_map * cur;

  for (cur = dlopen (NULL, 0); cur != NULL; cur = cur->l_next)
  {
    if (gum_linux_module_path_matches (cur->l_name, module_name))
      return cur;
  }

  for (cur = dlopen (NULL, 0); cur != NULL; cur = cur->l_next)
  {
    gchar * target, * parent_dir, * canonical_path;
    gboolean is_match;

    target = g_file_read_link (cur->l_name, NULL);
    if (target == NULL)
      continue;
    parent_dir = g_path_get_dirname (cur->l_name);
    canonical_path = g_canonicalize_filename (target, parent_dir);

    is_match = gum_linux_module_path_matches (canonical_path, module_name);

    g_free (canonical_path);
    g_free (parent_dir);
    g_free (target);

    if (is_match)
      return cur;
  }

  return NULL;
#else
# if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE)
    return gum_android_get_module_handle (module_name);
# endif

  return dlopen (module_name, RTLD_LAZY | RTLD_NOLOAD);
#endif
}

static void *
gum_module_get_symbol (void * module,
                       const gchar * symbol)
{
  GumGenericDlsymImpl dlsym_impl = dlsym;

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE)
    gum_android_find_unrestricted_dlsym (&dlsym_impl);
#endif

  return dlsym_impl (module, symbol);
}

gboolean
gum_module_ensure_initialized (const gchar * module_name)
{
  void * module;

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE)
    return gum_android_ensure_module_initialized (module_name);
#endif

  module = gum_module_get_handle (module_name);
  if (module == NULL)
    return FALSE;
  dlclose (module);

#ifndef HAVE_MUSL
  module = dlopen (module_name, RTLD_LAZY);
  if (module == NULL)
    return FALSE;
  dlclose (module);
#endif

  return TRUE;
}

GumAddress
gum_module_find_export_by_name (const gchar * module_name,
                                const gchar * symbol_name)
{
  GumAddress result;
  void * module;

#if defined (HAVE_ANDROID) && !defined (GUM_DIET)
  if (gum_android_get_linker_flavor () == GUM_ANDROID_LINKER_NATIVE &&
      gum_android_try_resolve_magic_export (module_name, symbol_name, &result))
    return result;
#endif

  if (module_name != NULL)
  {
    module = gum_module_get_handle (module_name);
    if (module == NULL)
      return 0;
  }
  else
  {
    module = RTLD_DEFAULT;
  }

  result = GUM_ADDRESS (gum_module_get_symbol (module, symbol_name));

  if (module != RTLD_DEFAULT)
    dlclose (module);

  return result;
}

GumCpuType
gum_linux_cpu_type_from_file (const gchar * path,
                              GError ** error)
{
  GumCpuType result = -1;
  FILE * file;
  guint8 ei_data;
  guint16 e_machine;

  file = fopen (path, "rb");
  if (file == NULL)
    goto fopen_failed;

  if (fseek (file, EI_DATA, SEEK_SET) != 0)
    goto unsupported_executable;
  if (fread (&ei_data, sizeof (ei_data), 1, file) != 1)
    goto unsupported_executable;

  if (fseek (file, 0x12, SEEK_SET) != 0)
    goto unsupported_executable;
  if (fread (&e_machine, sizeof (e_machine), 1, file) != 1)
    goto unsupported_executable;

  if (ei_data == ELFDATA2LSB)
    e_machine = GUINT16_FROM_LE (e_machine);
  else if (ei_data == ELFDATA2MSB)
    e_machine = GUINT16_FROM_BE (e_machine);
  else
    goto unsupported_ei_data;

  switch (e_machine)
  {
    case 0x0003:
      result = GUM_CPU_IA32;
      break;
    case 0x003e:
      result = GUM_CPU_AMD64;
      break;
    case 0x0028:
      result = GUM_CPU_ARM;
      break;
    case 0x00b7:
      result = GUM_CPU_ARM64;
      break;
    case 0x0008:
      result = GUM_CPU_MIPS;
      break;
    default:
      goto unsupported_executable;
  }

  goto beach;

fopen_failed:
  {
    if (errno == ENOENT)
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_FOUND,
          "File not found");
    }
    else if (errno == EACCES)
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_PERMISSION_DENIED,
          "Permission denied");
    }
    else
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_FAILED,
          "Unable to open file: %s", g_strerror (errno));
    }
    goto beach;
  }
unsupported_ei_data:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_SUPPORTED,
        "Unsupported ELF EI_DATA");
    goto beach;
  }
unsupported_executable:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_SUPPORTED,
        "Unsupported executable");
    goto beach;
  }
beach:
  {
    if (file != NULL)
      fclose (file);

    return result;
  }
}

GumCpuType
gum_linux_cpu_type_from_pid (pid_t pid,
                             GError ** error)
{
  GumCpuType result = -1;
  GError * err;
  gchar * auxv_path, * auxv;
  gsize auxv_size;

  auxv_path = g_strdup_printf ("/proc/%d/auxv", pid);

  auxv = NULL;
  err = NULL;
  if (!g_file_get_contents (auxv_path, &auxv, &auxv_size, &err))
    goto read_failed;
  if (auxv_size == 0)
    goto nearly_dead;

  result = gum_linux_cpu_type_from_auxv (auxv, auxv_size);

  goto beach;

read_failed:
  {
    if (g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_FOUND,
          "Process not found");
    }
    else if (g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_ACCES))
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_PERMISSION_DENIED,
          "Permission denied");
    }
    else
    {
      g_set_error (error, GUM_ERROR, GUM_ERROR_FAILED,
          "%s", err->message);
    }

    g_error_free (err);

    goto beach;
  }
nearly_dead:
  {
    g_set_error (error, GUM_ERROR, GUM_ERROR_NOT_FOUND,
        "Process not found");
    goto beach;
  }
beach:
  {
    g_free (auxv);
    g_free (auxv_path);

    return result;
  }
}

GumCpuType
gum_linux_cpu_type_from_auxv (gconstpointer auxv,
                              gsize auxv_size)
{
  GumCpuType result = -1;
  GumCpuType cpu32, cpu64;
  gsize i;

#if defined (HAVE_I386)
  cpu32 = GUM_CPU_IA32;
  cpu64 = GUM_CPU_AMD64;
#elif defined (HAVE_ARM) || defined (HAVE_ARM64)
  cpu32 = GUM_CPU_ARM;
  cpu64 = GUM_CPU_ARM64;
#elif defined (HAVE_MIPS)
  cpu32 = GUM_CPU_MIPS;
  cpu64 = GUM_CPU_MIPS;
#else
# error Unsupported architecture
#endif

  /*
   * The auxilliary structure format is architecture specific. Most notably,
   * type and value are both natively sized. We therefore detect whether a
   * process is 64-bit by examining each entry and confirming that the low bits
   * of the type field are zero. Note that this is itself endian specific.
   *
   * typedef struct
   * {
   *   uint32_t a_type;
   *   union
   *   {
   *     uint32_t a_val;
   *   } a_un;
   * } Elf32_auxv_t;
   *
   * typedef struct
   * {
   *   uint64_t a_type;
   *   union
   *   {
   *     uint64_t a_val;
   *   } a_un;
   * } Elf64_auxv_t;
   *
   * If the auxiliary vector is 32-bits and contains only an AT_NULL entry (note
   * that the documentation states that "The last entry contains two zeros"),
   * this will mean it has no non-zero type codes and could be mistaken for a
   * 64-bit format auxiliary vector. We therefore handle this special case.
   *
   * If the vector is less than 16 bytes it is not large enough to contain two
   * 64-bit zero values. If it is larger, then if it is a 32-bit format vector,
   * then it must contain at least one non-zero type code and hence the test
   * below should work.
   */

  if (auxv_size < 2 * sizeof (guint64))
  {
    result = cpu32;
  }
  else
  {
    result = cpu64;

    for (i = 0; i + sizeof (guint64) <= auxv_size; i += 16)
    {
      const guint64 * auxv_type = auxv + i;

      if ((*auxv_type & G_GUINT64_CONSTANT (0xffffffff00000000)) != 0)
      {
        result = cpu32;
        break;
      }
    }
  }

  return result;
}

gboolean
_gum_process_resolve_module_name (const gchar * name,
                                  gchar ** path,
                                  GumAddress * base)
{
  return gum_do_resolve_module_name (name, gum_process_query_libc_name (), path,
      base);
}

static gboolean
gum_do_resolve_module_name (const gchar * name,
                            const gchar * libc_name,
                            gchar ** path,
                            GumAddress * base)
{
  gboolean success = FALSE;
  GumResolveModuleNameContext ctx;

  if (name[0] == '/' && base == NULL)
  {
    success = TRUE;

    if (path != NULL)
      *path = g_strdup (name);

    goto beach;
  }

  ctx.name = name;
  ctx.known_address = 0;
#if defined (HAVE_GLIBC) || defined (HAVE_MUSL)
  {
    struct link_map * map = dlopen (name, RTLD_LAZY | RTLD_NOLOAD);
    if (map != NULL)
    {
      ctx.known_address = GUM_ADDRESS (map->l_ld);
      dlclose (map);
    }
  }
#endif
  ctx.path = NULL;
  ctx.base = 0;

  if (name == libc_name &&
      gum_query_program_modules ()->rtld == GUM_PROGRAM_RTLD_NONE)
  {
    gum_linux_enumerate_modules_using_proc_maps (
        gum_store_module_path_and_base_if_match, &ctx);
  }
  else
  {
    gum_do_enumerate_modules (libc_name,
        gum_store_module_path_and_base_if_match, &ctx);
  }

  success = ctx.path != NULL;

  if (path != NULL)
    *path = g_steal_pointer (&ctx.path);

  if (base != NULL)
    *base = ctx.base;

  g_free (ctx.path);

beach:
  return success;
}

static gboolean
gum_store_module_path_and_base_if_match (
    const GumModuleDetails * details,
    gpointer user_data)
{
  GumResolveModuleNameContext * ctx = user_data;
  gboolean is_match;

  if (ctx->known_address != 0)
    is_match = GUM_MEMORY_RANGE_INCLUDES (details->range, ctx->known_address);
  else
    is_match = gum_linux_module_path_matches (details->path, ctx->name);
  if (!is_match)
    return TRUE;

  ctx->path = g_strdup (details->path);
  ctx->base = details->range->base_address;
  return FALSE;
}

gboolean
gum_linux_module_path_matches (const gchar * path,
                               const gchar * name_or_path)
{
  const gchar * s;

  if (name_or_path[0] == '/')
    return strcmp (name_or_path, path) == 0;

  if ((s = strrchr (path, '/')) != NULL)
    return strcmp (name_or_path, s + 1) == 0;

  return strcmp (name_or_path, path) == 0;
}

void
gum_proc_maps_iter_init_for_self (GumProcMapsIter * iter)
{
  gum_proc_maps_iter_init_for_path (iter, "/proc/self/maps");
}

void
gum_proc_maps_iter_init_for_pid (GumProcMapsIter * iter,
                                 pid_t pid)
{
  gchar path[31 + 1];

  sprintf (path, "/proc/%u/maps", (guint) pid);

  gum_proc_maps_iter_init_for_path (iter, path);
}

static void
gum_proc_maps_iter_init_for_path (GumProcMapsIter * iter,
                                  const gchar * path)
{
  iter->fd = open (path, O_RDONLY | O_CLOEXEC);
  iter->read_cursor = iter->buffer;
  iter->write_cursor = iter->buffer;
}

void
gum_proc_maps_iter_destroy (GumProcMapsIter * iter)
{
  if (iter->fd != -1)
    close (iter->fd);
}

gboolean
gum_proc_maps_iter_next (GumProcMapsIter * iter,
                         const gchar ** line)
{
  gchar * next_newline;
  guint available;
  gboolean need_refill;

  if (iter->fd == -1)
    return FALSE;

  next_newline = NULL;

  available = iter->write_cursor - iter->read_cursor;
  if (available == 0)
  {
    need_refill = TRUE;
  }
  else
  {
    next_newline = strchr (iter->read_cursor, '\n');
    if (next_newline != NULL)
    {
      need_refill = FALSE;
    }
    else
    {
      need_refill = TRUE;
    }
  }

  if (need_refill)
  {
    guint offset;
    gssize res;

    offset = iter->read_cursor - iter->buffer;
    if (offset > 0)
    {
      memmove (iter->buffer, iter->read_cursor, available);
      iter->read_cursor -= offset;
      iter->write_cursor -= offset;
    }

    res = GUM_TEMP_FAILURE_RETRY (gum_libc_read (iter->fd,
        iter->write_cursor,
        iter->buffer + sizeof (iter->buffer) - 1 - iter->write_cursor));
    if (res <= 0)
      return FALSE;

    iter->write_cursor += res;
    iter->write_cursor[0] = '\0';

    next_newline = strchr (iter->read_cursor, '\n');
  }

  *line = iter->read_cursor;
  *next_newline = '\0';

  iter->read_cursor = next_newline + 1;

  return TRUE;
}

static void
gum_acquire_dumpability (void)
{
  G_LOCK (gum_dumpable);

  if (++gum_dumpable_refcount == 1)
  {
    /*
     * Some systems (notably Android on release applications) spawn processes as
     * not dumpable by default, disabling ptrace() and some other things on that
     * process for anyone other than root.
     */
    gum_dumpable_previous = prctl (PR_GET_DUMPABLE);
    if (gum_dumpable_previous != -1 && gum_dumpable_previous != 1)
      prctl (PR_SET_DUMPABLE, 1);
  }

  G_UNLOCK (gum_dumpable);
}

static void
gum_release_dumpability (void)
{
  G_LOCK (gum_dumpable);

  if (--gum_dumpable_refcount == 0)
  {
    if (gum_dumpable_previous != -1 && gum_dumpable_previous != 1)
      prctl (PR_SET_DUMPABLE, gum_dumpable_previous);
  }

  G_UNLOCK (gum_dumpable);
}

void
gum_linux_parse_ucontext (const ucontext_t * uc,
                          GumCpuContext * ctx)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  const greg_t * gr = uc->uc_mcontext.gregs;

  ctx->eip = gr[REG_EIP];

  ctx->edi = gr[REG_EDI];
  ctx->esi = gr[REG_ESI];
  ctx->ebp = gr[REG_EBP];
  ctx->esp = gr[REG_ESP];
  ctx->ebx = gr[REG_EBX];
  ctx->edx = gr[REG_EDX];
  ctx->ecx = gr[REG_ECX];
  ctx->eax = gr[REG_EAX];
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  const greg_t * gr = uc->uc_mcontext.gregs;

  ctx->rip = gr[REG_RIP];

  ctx->r15 = gr[REG_R15];
  ctx->r14 = gr[REG_R14];
  ctx->r13 = gr[REG_R13];
  ctx->r12 = gr[REG_R12];
  ctx->r11 = gr[REG_R11];
  ctx->r10 = gr[REG_R10];
  ctx->r9 = gr[REG_R9];
  ctx->r8 = gr[REG_R8];

  ctx->rdi = gr[REG_RDI];
  ctx->rsi = gr[REG_RSI];
  ctx->rbp = gr[REG_RBP];
  ctx->rsp = gr[REG_RSP];
  ctx->rbx = gr[REG_RBX];
  ctx->rdx = gr[REG_RDX];
  ctx->rcx = gr[REG_RCX];
  ctx->rax = gr[REG_RAX];
#elif defined (HAVE_ARM) && defined (HAVE_LEGACY_MCONTEXT)
  const elf_greg_t * gr = uc->uc_mcontext.gregs;

  ctx->pc = gr[R15];
  ctx->sp = gr[R13];
  ctx->cpsr = 0; /* FIXME: Anything we can do about this? */

  ctx->r8 = gr[R8];
  ctx->r9 = gr[R9];
  ctx->r10 = gr[R10];
  ctx->r11 = gr[R11];
  ctx->r12 = gr[R12];

  memset (ctx->v, 0, sizeof (ctx->v));

  ctx->r[0] = gr[R0];
  ctx->r[1] = gr[R1];
  ctx->r[2] = gr[R2];
  ctx->r[3] = gr[R3];
  ctx->r[4] = gr[R4];
  ctx->r[5] = gr[R5];
  ctx->r[6] = gr[R6];
  ctx->r[7] = gr[R7];
  ctx->lr = gr[R14];
#elif defined (HAVE_ARM)
  const mcontext_t * mc = &uc->uc_mcontext;

  ctx->pc = mc->arm_pc;
  ctx->sp = mc->arm_sp;
  ctx->cpsr = mc->arm_cpsr;

  ctx->r8 = mc->arm_r8;
  ctx->r9 = mc->arm_r9;
  ctx->r10 = mc->arm_r10;
  ctx->r11 = mc->arm_fp;
  ctx->r12 = mc->arm_ip;

  memset (ctx->v, 0, sizeof (ctx->v));

  ctx->r[0] = mc->arm_r0;
  ctx->r[1] = mc->arm_r1;
  ctx->r[2] = mc->arm_r2;
  ctx->r[3] = mc->arm_r3;
  ctx->r[4] = mc->arm_r4;
  ctx->r[5] = mc->arm_r5;
  ctx->r[6] = mc->arm_r6;
  ctx->r[7] = mc->arm_r7;
  ctx->lr = mc->arm_lr;
#elif defined (HAVE_ARM64)
  const mcontext_t * mc = &uc->uc_mcontext;
  gsize i;

  ctx->pc = mc->pc;
  ctx->sp = mc->sp;
  ctx->nzcv = 0;

  for (i = 0; i != G_N_ELEMENTS (ctx->x); i++)
    ctx->x[i] = mc->regs[i];
  ctx->fp = mc->regs[29];
  ctx->lr = mc->regs[30];

  memset (ctx->v, 0, sizeof (ctx->v));
#elif defined (HAVE_MIPS)
  const greg_t * gr = uc->uc_mcontext.gregs;

  ctx->at = (guint32) gr[1];

  ctx->v0 = (guint32) gr[2];
  ctx->v1 = (guint32) gr[3];

  ctx->a0 = (guint32) gr[4];
  ctx->a1 = (guint32) gr[5];
  ctx->a2 = (guint32) gr[6];
  ctx->a3 = (guint32) gr[7];

  ctx->t0 = (guint32) gr[8];
  ctx->t1 = (guint32) gr[9];
  ctx->t2 = (guint32) gr[10];
  ctx->t3 = (guint32) gr[11];
  ctx->t4 = (guint32) gr[12];
  ctx->t5 = (guint32) gr[13];
  ctx->t6 = (guint32) gr[14];
  ctx->t7 = (guint32) gr[15];

  ctx->s0 = (guint32) gr[16];
  ctx->s1 = (guint32) gr[17];
  ctx->s2 = (guint32) gr[18];
  ctx->s3 = (guint32) gr[19];
  ctx->s4 = (guint32) gr[20];
  ctx->s5 = (guint32) gr[21];
  ctx->s6 = (guint32) gr[22];
  ctx->s7 = (guint32) gr[23];

  ctx->t8 = (guint32) gr[24];
  ctx->t9 = (guint32) gr[25];

  ctx->k0 = (guint32) gr[26];
  ctx->k1 = (guint32) gr[27];

  ctx->gp = (guint32) gr[28];
  ctx->sp = (guint32) gr[29];
  ctx->fp = (guint32) gr[30];
  ctx->ra = (guint32) gr[31];

  ctx->hi = (guint32) uc->uc_mcontext.mdhi;
  ctx->lo = (guint32) uc->uc_mcontext.mdlo;

  ctx->pc = (guint32) uc->uc_mcontext.pc;
#else
# error FIXME
#endif
}

void
gum_linux_unparse_ucontext (const GumCpuContext * ctx,
                            ucontext_t * uc)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  greg_t * gr = uc->uc_mcontext.gregs;

  gr[REG_EIP] = ctx->eip;

  gr[REG_EDI] = ctx->edi;
  gr[REG_ESI] = ctx->esi;
  gr[REG_EBP] = ctx->ebp;
  gr[REG_ESP] = ctx->esp;
  gr[REG_EBX] = ctx->ebx;
  gr[REG_EDX] = ctx->edx;
  gr[REG_ECX] = ctx->ecx;
  gr[REG_EAX] = ctx->eax;
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  greg_t * gr = uc->uc_mcontext.gregs;

  gr[REG_RIP] = ctx->rip;

  gr[REG_R15] = ctx->r15;
  gr[REG_R14] = ctx->r14;
  gr[REG_R13] = ctx->r13;
  gr[REG_R12] = ctx->r12;
  gr[REG_R11] = ctx->r11;
  gr[REG_R10] = ctx->r10;
  gr[REG_R9] = ctx->r9;
  gr[REG_R8] = ctx->r8;

  gr[REG_RDI] = ctx->rdi;
  gr[REG_RSI] = ctx->rsi;
  gr[REG_RBP] = ctx->rbp;
  gr[REG_RSP] = ctx->rsp;
  gr[REG_RBX] = ctx->rbx;
  gr[REG_RDX] = ctx->rdx;
  gr[REG_RCX] = ctx->rcx;
  gr[REG_RAX] = ctx->rax;
#elif defined (HAVE_ARM) && defined (HAVE_LEGACY_MCONTEXT)
  elf_greg_t * gr = uc->uc_mcontext.gregs;

  /* FIXME: Anything we can do about cpsr? */
  gr[R15] = ctx->pc;
  gr[R13] = ctx->sp;

  gr[R8] = ctx->r8;
  gr[R9] = ctx->r9;
  gr[R10] = ctx->r10;
  gr[R11] = ctx->r11;
  gr[R12] = ctx->r12;

  gr[R0] = ctx->r[0];
  gr[R1] = ctx->r[1];
  gr[R2] = ctx->r[2];
  gr[R3] = ctx->r[3];
  gr[R4] = ctx->r[4];
  gr[R5] = ctx->r[5];
  gr[R6] = ctx->r[6];
  gr[R7] = ctx->r[7];
  gr[R14] = ctx->lr;
#elif defined (HAVE_ARM)
  mcontext_t * mc = &uc->uc_mcontext;

  mc->arm_pc = ctx->pc;
  mc->arm_sp = ctx->sp;
  mc->arm_cpsr = ctx->cpsr;

  mc->arm_r8 = ctx->r8;
  mc->arm_r9 = ctx->r9;
  mc->arm_r10 = ctx->r10;
  mc->arm_fp = ctx->r11;
  mc->arm_ip = ctx->r12;

  mc->arm_r0 = ctx->r[0];
  mc->arm_r1 = ctx->r[1];
  mc->arm_r2 = ctx->r[2];
  mc->arm_r3 = ctx->r[3];
  mc->arm_r4 = ctx->r[4];
  mc->arm_r5 = ctx->r[5];
  mc->arm_r6 = ctx->r[6];
  mc->arm_r7 = ctx->r[7];
  mc->arm_lr = ctx->lr;
#elif defined (HAVE_ARM64)
  mcontext_t * mc = &uc->uc_mcontext;
  gsize i;

  mc->pc = ctx->pc;
  mc->sp = ctx->sp;

  for (i = 0; i != G_N_ELEMENTS (ctx->x); i++)
    mc->regs[i] = ctx->x[i];
  mc->regs[29] = ctx->fp;
  mc->regs[30] = ctx->lr;
#elif defined (HAVE_MIPS)
  greg_t * gr = uc->uc_mcontext.gregs;

  gr[1] = (guint64) ctx->at;

  gr[2] = (guint64) ctx->v0;
  gr[3] = (guint64) ctx->v1;

  gr[4] = (guint64) ctx->a0;
  gr[5] = (guint64) ctx->a1;
  gr[6] = (guint64) ctx->a2;
  gr[7] = (guint64) ctx->a3;

  gr[8] = (guint64) ctx->t0;
  gr[9] = (guint64) ctx->t1;
  gr[10] = (guint64) ctx->t2;
  gr[11] = (guint64) ctx->t3;
  gr[12] = (guint64) ctx->t4;
  gr[13] = (guint64) ctx->t5;
  gr[14] = (guint64) ctx->t6;
  gr[15] = (guint64) ctx->t7;

  gr[16] = (guint64) ctx->s0;
  gr[17] = (guint64) ctx->s1;
  gr[18] = (guint64) ctx->s2;
  gr[19] = (guint64) ctx->s3;
  gr[20] = (guint64) ctx->s4;
  gr[21] = (guint64) ctx->s5;
  gr[22] = (guint64) ctx->s6;
  gr[23] = (guint64) ctx->s7;

  gr[24] = (guint64) ctx->t8;
  gr[25] = (guint64) ctx->t9;

  gr[26] = (guint64) ctx->k0;
  gr[27] = (guint64) ctx->k1;

  gr[28] = (guint64) ctx->gp;
  gr[29] = (guint64) ctx->sp;
  gr[30] = (guint64) ctx->fp;
  gr[31] = (guint64) ctx->ra;

  uc->uc_mcontext.mdhi = (guint64) ctx->hi;
  uc->uc_mcontext.mdlo = (guint64) ctx->lo;

  uc->uc_mcontext.pc = (guint64) ctx->pc;
#else
# error FIXME
#endif
}

static void
gum_parse_regs (const GumRegs * regs,
                GumCpuContext * ctx)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  ctx->eip = regs->eip;

  ctx->edi = regs->edi;
  ctx->esi = regs->esi;
  ctx->ebp = regs->ebp;
  ctx->esp = regs->esp;
  ctx->ebx = regs->ebx;
  ctx->edx = regs->edx;
  ctx->ecx = regs->ecx;
  ctx->eax = regs->eax;
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  ctx->rip = regs->rip;

  ctx->r15 = regs->r15;
  ctx->r14 = regs->r14;
  ctx->r13 = regs->r13;
  ctx->r12 = regs->r12;
  ctx->r11 = regs->r11;
  ctx->r10 = regs->r10;
  ctx->r9 = regs->r9;
  ctx->r8 = regs->r8;

  ctx->rdi = regs->rdi;
  ctx->rsi = regs->rsi;
  ctx->rbp = regs->rbp;
  ctx->rsp = regs->rsp;
  ctx->rbx = regs->rbx;
  ctx->rdx = regs->rdx;
  ctx->rcx = regs->rcx;
  ctx->rax = regs->rax;
#elif defined (HAVE_ARM)
  gsize i;

  ctx->pc = regs->ARM_pc;
  ctx->sp = regs->ARM_sp;
  ctx->cpsr = regs->ARM_cpsr;

  ctx->r8 = regs->uregs[8];
  ctx->r9 = regs->uregs[9];
  ctx->r10 = regs->uregs[10];
  ctx->r11 = regs->uregs[11];
  ctx->r12 = regs->uregs[12];

  memset (ctx->v, 0, sizeof (ctx->v));

  for (i = 0; i != G_N_ELEMENTS (ctx->r); i++)
    ctx->r[i] = regs->uregs[i];
  ctx->lr = regs->ARM_lr;
#elif defined (HAVE_ARM64)
  gsize i;

  ctx->pc = regs->pc;
  ctx->sp = regs->sp;
  ctx->nzcv = 0;

  for (i = 0; i != G_N_ELEMENTS (ctx->x); i++)
    ctx->x[i] = regs->regs[i];
  ctx->fp = regs->regs[29];
  ctx->lr = regs->regs[30];

  memset (ctx->v, 0, sizeof (ctx->v));
#elif defined (HAVE_MIPS)
  ctx->at = regs->regs[1];

  ctx->v0 = regs->regs[2];
  ctx->v1 = regs->regs[3];

  ctx->a0 = regs->regs[4];
  ctx->a1 = regs->regs[5];
  ctx->a2 = regs->regs[6];
  ctx->a3 = regs->regs[7];

  ctx->t0 = regs->regs[8];
  ctx->t1 = regs->regs[9];
  ctx->t2 = regs->regs[10];
  ctx->t3 = regs->regs[11];
  ctx->t4 = regs->regs[12];
  ctx->t5 = regs->regs[13];
  ctx->t6 = regs->regs[14];
  ctx->t7 = regs->regs[15];

  ctx->s0 = regs->regs[16];
  ctx->s1 = regs->regs[17];
  ctx->s2 = regs->regs[18];
  ctx->s3 = regs->regs[19];
  ctx->s4 = regs->regs[20];
  ctx->s5 = regs->regs[21];
  ctx->s6 = regs->regs[22];
  ctx->s7 = regs->regs[23];

  ctx->t8 = regs->regs[24];
  ctx->t9 = regs->regs[25];

  ctx->k0 = regs->regs[26];
  ctx->k1 = regs->regs[27];

  ctx->gp = regs->regs[28];
  ctx->sp = regs->regs[29];
  ctx->fp = regs->regs[30];

  ctx->ra = regs->regs[31];

  ctx->hi = regs->hi;
  ctx->lo = regs->lo;

  ctx->pc = regs->cp0_epc;
#else
# error Unsupported architecture
#endif
}

static void
gum_unparse_regs (const GumCpuContext * ctx,
                  GumRegs * regs)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  regs->eip = ctx->eip;

  regs->edi = ctx->edi;
  regs->esi = ctx->esi;
  regs->ebp = ctx->ebp;
  regs->esp = ctx->esp;
  regs->ebx = ctx->ebx;
  regs->edx = ctx->edx;
  regs->ecx = ctx->ecx;
  regs->eax = ctx->eax;
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  regs->rip = ctx->rip;

  regs->r15 = ctx->r15;
  regs->r14 = ctx->r14;
  regs->r13 = ctx->r13;
  regs->r12 = ctx->r12;
  regs->r11 = ctx->r11;
  regs->r10 = ctx->r10;
  regs->r9 = ctx->r9;
  regs->r8 = ctx->r8;

  regs->rdi = ctx->rdi;
  regs->rsi = ctx->rsi;
  regs->rbp = ctx->rbp;
  regs->rsp = ctx->rsp;
  regs->rbx = ctx->rbx;
  regs->rdx = ctx->rdx;
  regs->rcx = ctx->rcx;
  regs->rax = ctx->rax;
#elif defined (HAVE_ARM)
  gsize i;

  regs->ARM_pc = ctx->pc;
  regs->ARM_sp = ctx->sp;
  regs->ARM_cpsr = ctx->cpsr;

  regs->uregs[8] = ctx->r8;
  regs->uregs[9] = ctx->r9;
  regs->uregs[10] = ctx->r10;
  regs->uregs[11] = ctx->r11;
  regs->uregs[12] = ctx->r12;

  for (i = 0; i != G_N_ELEMENTS (ctx->r); i++)
    regs->uregs[i] = ctx->r[i];
  regs->ARM_lr = ctx->lr;
#elif defined (HAVE_ARM64)
  gsize i;

  regs->pc = ctx->pc;
  regs->sp = ctx->sp;

  for (i = 0; i != G_N_ELEMENTS (ctx->x); i++)
    regs->regs[i] = ctx->x[i];
  regs->regs[29] = ctx->fp;
  regs->regs[30] = ctx->lr;
#elif defined (HAVE_MIPS)
  regs->regs[1] = ctx->at;

  regs->regs[2] = ctx->v0;
  regs->regs[3] = ctx->v1;

  regs->regs[4] = ctx->a0;
  regs->regs[5] = ctx->a1;
  regs->regs[6] = ctx->a2;
  regs->regs[7] = ctx->a3;

  regs->regs[8] = ctx->t0;
  regs->regs[9] = ctx->t1;
  regs->regs[10] = ctx->t2;
  regs->regs[11] = ctx->t3;
  regs->regs[12] = ctx->t4;
  regs->regs[13] = ctx->t5;
  regs->regs[14] = ctx->t6;
  regs->regs[15] = ctx->t7;

  regs->regs[16] = ctx->s0;
  regs->regs[17] = ctx->s1;
  regs->regs[18] = ctx->s2;
  regs->regs[19] = ctx->s3;
  regs->regs[20] = ctx->s4;
  regs->regs[21] = ctx->s5;
  regs->regs[22] = ctx->s6;
  regs->regs[23] = ctx->s7;

  regs->regs[24] = ctx->t8;
  regs->regs[25] = ctx->t9;

  regs->regs[26] = ctx->k0;
  regs->regs[27] = ctx->k1;

  regs->regs[28] = ctx->gp;
  regs->regs[29] = ctx->sp;
  regs->regs[30] = ctx->fp;

  regs->regs[31] = ctx->ra;

  regs->hi = ctx->hi;
  regs->lo = ctx->lo;

  regs->cp0_epc = ctx->pc;
#else
# error Unsupported architecture
#endif
}

static gchar *
gum_thread_read_name (GumThreadId thread_id)
{
  gchar * name = NULL;
  gchar * path;
  gchar * comm = NULL;

  path = g_strdup_printf ("/proc/self/task/%" G_GSIZE_FORMAT "/comm",
      thread_id);
  if (!g_file_get_contents (path, &comm, NULL, NULL))
    goto beach;
  name = g_strchomp (g_steal_pointer (&comm));

beach:
  g_free (comm);
  g_free (path);

  return name;
}

static gboolean
gum_thread_read_state (GumThreadId tid,
                       GumThreadState * state)
{
  gboolean success = FALSE;
  gchar * path, * info = NULL;

  path = g_strdup_printf ("/proc/self/task/%" G_GSIZE_FORMAT "/stat", tid);
  if (g_file_get_contents (path, &info, NULL, NULL))
  {
    gchar * p;

    p = strrchr (info, ')') + 2;

    *state = gum_thread_state_from_proc_status_character (*p);
    success = TRUE;
  }

  g_free (info);
  g_free (path);

  return success;
}

static GumThreadState
gum_thread_state_from_proc_status_character (gchar c)
{
  switch (g_ascii_toupper (c))
  {
    case 'R': return GUM_THREAD_RUNNING;
    case 'S': return GUM_THREAD_WAITING;
    case 'D': return GUM_THREAD_UNINTERRUPTIBLE;
    case 'Z': return GUM_THREAD_UNINTERRUPTIBLE;
    case 'T': return GUM_THREAD_STOPPED;
    case 'W':
    default:
      return GUM_THREAD_UNINTERRUPTIBLE;
  }
}

static GumPageProtection
gum_page_protection_from_proc_perms_string (const gchar * perms)
{
  GumPageProtection prot = GUM_PAGE_NO_ACCESS;

  if (perms[0] == 'r')
    prot |= GUM_PAGE_READ;
  if (perms[1] == 'w')
    prot |= GUM_PAGE_WRITE;
  if (perms[2] == 'x')
    prot |= GUM_PAGE_EXECUTE;

  return prot;
}

static gssize
gum_get_regs (pid_t pid,
              GumRegs * regs)
{
  if (gum_is_regset_supported)
  {
    struct iovec io = {
      .iov_base = regs,
      .iov_len = sizeof (GumRegs)
    };
    gssize ret = gum_libc_ptrace (PTRACE_GETREGSET, pid,
        GSIZE_TO_POINTER (NT_PRSTATUS), &io);
    if (ret >= 0)
      return ret;
    else if (ret == -EPERM || ret == -ESRCH)
      return ret;
    else
      gum_is_regset_supported = FALSE;
  }

  return gum_libc_ptrace (PTRACE_GETREGS, pid, NULL, regs);
}

static gssize
gum_set_regs (pid_t pid,
              const GumRegs * regs)
{
  if (gum_is_regset_supported)
  {
    struct iovec io = {
      .iov_base = (void *) regs,
      .iov_len = sizeof (GumRegs)
    };
    gssize ret = gum_libc_ptrace (PTRACE_SETREGSET, pid,
        GSIZE_TO_POINTER (NT_PRSTATUS), &io);
    if (ret >= 0)
      return ret;
    else if (ret == -EPERM || ret == -ESRCH)
      return ret;
    else
      gum_is_regset_supported = FALSE;
  }

  return gum_libc_ptrace (PTRACE_SETREGS, pid, NULL, (gpointer) regs);
}

static gssize
gum_libc_clone (GumCloneFunc child_func,
                gpointer child_stack,
                gint flags,
                gpointer arg,
                pid_t * parent_tidptr,
                GumUserDesc * tls,
                pid_t * child_tidptr)
{
  gssize result;
  gpointer * child_sp = child_stack;

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  *(--child_sp) = arg;
  *(--child_sp) = child_func;

  {
    register          gint ebx asm ("ebx") = flags;
    register    gpointer * ecx asm ("ecx") = child_sp;
    register       pid_t * edx asm ("edx") = parent_tidptr;
    register GumUserDesc * esi asm ("esi") = tls;
    register       pid_t * edi asm ("edi") = child_tidptr;

    asm volatile (
        "int $0x80\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"

        /* child: */
        "popl %%eax\n\t"
        "call *%%eax\n\t"
        "movl %%eax, %%ebx\n\t"
        "movl %[exit_syscall], %%eax\n\t"
        "int $0x80\n\t"

        /* parent: */
        "1:\n\t"
        : "=a" (result)
        : "0" (__NR_clone),
          "r" (ebx),
          "r" (ecx),
          "r" (edx),
          "r" (esi),
          "r" (edi),
          [exit_syscall] "i" (__NR_exit)
        : "cc", "memory"
    );
  }
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  *(--child_sp) = arg;
  *(--child_sp) = child_func;
  *(--child_sp) = tls;

  {
    register          gint rdi asm ("rdi") = flags;
    register    gpointer * rsi asm ("rsi") = child_sp;
    register       pid_t * rdx asm ("rdx") = parent_tidptr;
    register GumUserDesc * r10 asm ("r10") = tls;
    register       pid_t *  r8 asm ( "r8") = child_tidptr;

    asm volatile (
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"

        /* child: */
        "movq %[prctl_syscall], %%rax\n\t"
        "movq %[arch_set_fs], %%rdi\n\t"
        "popq %%rsi\n\t"
        "syscall\n\t"

        "popq %%rax\n\t"
        "popq %%rdi\n\t"
        "call *%%rax\n\t"
        "movq %%rax, %%rdi\n\t"
        "movq %[exit_syscall], %%rax\n\t"
        "syscall\n\t"

        /* parent: */
        "1:\n\t"
        : "=a" (result)
        : "0" (__NR_clone),
          "r" (rdi),
          "r" (rsi),
          "r" (rdx),
          "r" (r10),
          "r" (r8),
          [prctl_syscall] "i" (__NR_arch_prctl),
          [arch_set_fs] "i" (ARCH_SET_FS),
          [exit_syscall] "i" (__NR_exit)
        : "rcx", "r11", "cc", "memory"
    );
  }
#elif defined (HAVE_ARM) && defined (__ARM_EABI__)
  *(--child_sp) = child_func;
  *(--child_sp) = arg;

  {
    register        gssize r6 asm ("r6") = __NR_clone;
    register          gint r0 asm ("r0") = flags;
    register    gpointer * r1 asm ("r1") = child_sp;
    register       pid_t * r2 asm ("r2") = parent_tidptr;
    register GumUserDesc * r3 asm ("r3") = tls;
    register       pid_t * r4 asm ("r4") = child_tidptr;

    asm volatile (
        "push {r7}\n\t"
        "mov r7, r6\n\t"
        "swi 0x0\n\t"
        "cmp r0, #0\n\t"
        "bne 1f\n\t"

        /* child: */
        "pop {r0, r1}\n\t"
        "blx r1\n\t"
        "mov r7, %[exit_syscall]\n\t"
        "swi 0x0\n\t"

        /* parent: */
        "1:\n\t"
        "pop {r7}\n\t"
        : "+r" (r0)
        : "r" (r1),
          "r" (r2),
          "r" (r3),
          "r" (r4),
          "r" (r6),
          [exit_syscall] "i" (__NR_exit)
        : "cc", "memory"
    );

    result = r0;
  }
#elif defined (HAVE_ARM)
  *(--child_sp) = child_func;
  *(--child_sp) = arg;

  {
    register          gint r0 asm ("r0") = flags;
    register    gpointer * r1 asm ("r1") = child_sp;
    register       pid_t * r2 asm ("r2") = parent_tidptr;
    register GumUserDesc * r3 asm ("r3") = tls;
    register       pid_t * r4 asm ("r4") = child_tidptr;

    asm volatile (
        "swi %[clone_syscall]\n\t"
        "cmp r0, #0\n\t"
        "bne 1f\n\t"

        /* child: */
        "ldmia sp!, {r0, r1}\n\t"
        "blx r1\n\t"
        "swi %[exit_syscall]\n\t"

        /* parent: */
        "1:\n\t"
        : "+r" (r0)
        : "r" (r1),
          "r" (r2),
          "r" (r3),
          "r" (r4),
          [clone_syscall] "i" (__NR_clone),
          [exit_syscall] "i" (__NR_exit)
        : "cc", "memory"
    );

    result = r0;
  }
#elif defined (HAVE_ARM64)
  *(--child_sp) = child_func;
  *(--child_sp) = arg;

  {
    register        gssize x8 asm ("x8") = __NR_clone;
    register          gint x0 asm ("x0") = flags;
    register    gpointer * x1 asm ("x1") = child_sp;
    register       pid_t * x2 asm ("x2") = parent_tidptr;
    register GumUserDesc * x3 asm ("x3") = tls;
    register       pid_t * x4 asm ("x4") = child_tidptr;

    asm volatile (
        "svc 0x0\n\t"
        "cbnz x0, 1f\n\t"

        /* child: */
        "ldp x0, x1, [sp], #16\n\t"
        "blr x1\n\t"
        "mov x8, %x[exit_syscall]\n\t"
        "svc 0x0\n\t"

        /* parent: */
        "1:\n\t"
        : "+r" (x0)
        : "r" (x1),
          "r" (x2),
          "r" (x3),
          "r" (x4),
          "r" (x8),
          [exit_syscall] "i" (__NR_exit)
        : "cc", "memory"
    );

    result = x0;
  }
#elif defined (HAVE_MIPS)
  *(--child_sp) = child_func;
  *(--child_sp) = arg;

  {
    register          gint a0 asm ("$4") = flags;
    register    gpointer * a1 asm ("$5") = child_sp;
    register       pid_t * a2 asm ("$6") = parent_tidptr;
    register GumUserDesc * a3 asm ("$7") = tls;
    register       pid_t * a4 asm ("$8") = child_tidptr;
    int status;
    gssize retval;

    asm volatile (
        ".set noreorder\n\t"
        "addiu $sp, $sp, -24\n\t"
        "sw $8, 16($sp)\n\t"
        "li $2, %[clone_syscall]\n\t"
        "syscall\n\t"
        ".set reorder\n\t"
        "bne $7, $0, 1f\n\t"
        "bne $2, $0, 1f\n\t"

        /* child: */
        "lw $4, 0($sp)\n\t"
        "lw $8, 4($sp)\n\t"
        "addiu $sp, $sp, 8\n\t"
        "jalr $8\n\t"
        "move $4, $2\n\t"
        "li $2, %[exit_syscall]\n\t"
        "syscall\n\t"

        /* parent: */
        "1:\n\t"
        "addiu $sp, $sp, 24\n\t"
        "move %0, $7\n\t"
        "move %1, $2\n\t"
        : "=r" (status),
          "=r" (retval)
        : "r" (a0),
          "r" (a1),
          "r" (a2),
          "r" (a3),
          "r" (a4),
          [clone_syscall] "i" (__NR_clone),
          [exit_syscall] "i" (__NR_exit)
        : "$1", "$2", "$3",
          "$10", "$11", "$12", "$13", "$14", "$15",
          "$24", "$25",
          "hi", "lo",
          "memory"
    );

    if (status == 0)
    {
      result = retval;
    }
    else
    {
      result = -1;
      errno = retval;
    }
  }
#endif

  return result;
}

static gssize
gum_libc_read (gint fd,
               gpointer buf,
               gsize count)
{
  return gum_libc_syscall_3 (__NR_read, fd, GPOINTER_TO_SIZE (buf), count);
}

static gssize
gum_libc_write (gint fd,
                gconstpointer buf,
                gsize count)
{
  return gum_libc_syscall_3 (__NR_write, fd, GPOINTER_TO_SIZE (buf), count);
}

static pid_t
gum_libc_waitpid (pid_t pid,
                  int * status,
                  int options)
{
#ifdef __NR_waitpid
  return gum_libc_syscall_3 (__NR_waitpid, pid, GPOINTER_TO_SIZE (status),
      options);
#else
  return gum_libc_syscall_4 (__NR_wait4, pid, GPOINTER_TO_SIZE (status),
      options, 0);
#endif
}

static gssize
gum_libc_ptrace (gsize request,
                 pid_t pid,
                 gpointer address,
                 gpointer data)
{
  return gum_libc_syscall_4 (__NR_ptrace, request, pid,
      GPOINTER_TO_SIZE (address), GPOINTER_TO_SIZE (data));
}

static gssize
gum_libc_syscall_4 (gsize n,
                    gsize a,
                    gsize b,
                    gsize c,
                    gsize d)
{
  gssize result;

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  {
    register gsize ebx asm ("ebx") = a;
    register gsize ecx asm ("ecx") = b;
    register gsize edx asm ("edx") = c;
    register gsize esi asm ("esi") = d;

    asm volatile (
        "int $0x80\n\t"
        : "=a" (result)
        : "0" (n),
          "r" (ebx),
          "r" (ecx),
          "r" (edx),
          "r" (esi)
        : "cc", "memory"
    );
  }
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  {
    register gsize rdi asm ("rdi") = a;
    register gsize rsi asm ("rsi") = b;
    register gsize rdx asm ("rdx") = c;
    register gsize r10 asm ("r10") = d;

    asm volatile (
        "syscall\n\t"
        : "=a" (result)
        : "0" (n),
          "r" (rdi),
          "r" (rsi),
          "r" (rdx),
          "r" (r10)
        : "rcx", "r11", "cc", "memory"
    );
  }
#elif defined (HAVE_ARM) && defined (__ARM_EABI__)
  {
    register gssize r6 asm ("r6") = n;
    register  gsize r0 asm ("r0") = a;
    register  gsize r1 asm ("r1") = b;
    register  gsize r2 asm ("r2") = c;
    register  gsize r3 asm ("r3") = d;

    asm volatile (
        "push {r7}\n\t"
        "mov r7, r6\n\t"
        "swi 0x0\n\t"
        "pop {r7}\n\t"
        : "+r" (r0)
        : "r" (r1),
          "r" (r2),
          "r" (r3),
          "r" (r6)
        : "memory"
    );

    result = r0;
  }
#elif defined (HAVE_ARM)
  {
    register gssize r0 asm ("r0") = n;
    register  gsize r1 asm ("r1") = a;
    register  gsize r2 asm ("r2") = b;
    register  gsize r3 asm ("r3") = c;
    register  gsize r4 asm ("r4") = d;

    asm volatile (
        "swi %[syscall]\n\t"
        : "+r" (r0)
        : "r" (r1),
          "r" (r2),
          "r" (r3),
          "r" (r4),
          [syscall] "i" (__NR_syscall)
        : "memory"
    );

    result = r0;
  }
#elif defined (HAVE_ARM64)
  {
    register gssize x8 asm ("x8") = n;
    register  gsize x0 asm ("x0") = a;
    register  gsize x1 asm ("x1") = b;
    register  gsize x2 asm ("x2") = c;
    register  gsize x3 asm ("x3") = d;

    asm volatile (
        "svc 0x0\n\t"
        : "+r" (x0)
        : "r" (x1),
          "r" (x2),
          "r" (x3),
          "r" (x8)
        : "memory"
    );

    result = x0;
  }
#elif defined (HAVE_MIPS)
  {
    register gssize v0 asm ("$16") = n;
    register  gsize a0 asm ("$4") = a;
    register  gsize a1 asm ("$5") = b;
    register  gsize a2 asm ("$6") = c;
    register  gsize a3 asm ("$7") = d;
    int status;
    gssize retval;

    asm volatile (
        ".set noreorder\n\t"
        "move $2, %1\n\t"
        "syscall\n\t"
        "move %0, $7\n\t"
        "move %1, $2\n\t"
        ".set reorder\n\t"
        : "=r" (status),
          "=r" (retval)
        : "r" (v0),
          "r" (a0),
          "r" (a1),
          "r" (a2),
          "r" (a3)
        : "$1", "$2", "$3",
          "$10", "$11", "$12", "$13", "$14", "$15",
          "$24", "$25",
          "hi", "lo",
          "memory"
    );

    if (status == 0)
    {
      result = retval;
    }
    else
    {
      result = -1;
      errno = retval;
    }
  }
#endif

  return result;
}
