/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>

#if defined(__APPLE__)
  /* macOS 10.8 and later have a working posix_spawn */
# if MAC_OS_X_VERSION_MIN_REQUIRED >= 1080
#  define UV_USE_APPLE_POSIX_SPAWN
#  include <spawn.h>
# endif
# include <paths.h>
# include <sys/kauth.h>
# include <sys/types.h>
# include <sys/sysctl.h>
# include <dlfcn.h>
# include <crt_externs.h>
# include <xlocale.h>
# define environ (*_NSGetEnviron())

/* macOS 10.14 back does not define this constant */
# ifndef POSIX_SPAWN_SETSID
#  define POSIX_SPAWN_SETSID 1024
# endif

#else
extern char **environ;
#endif

#if defined(__linux__) || defined(__GLIBC__)
# include <grp.h>
#endif

#if defined(__MVS__)
# include "zos-base.h"
#endif

#ifndef CMAKE_BOOTSTRAP
#if defined(__linux__)
# define uv__cpu_set_t cpu_set_t
#elif defined(__FreeBSD__)
# include <sys/param.h>
# include <sys/cpuset.h>
# include <pthread_np.h>
# define uv__cpu_set_t cpuset_t
#endif
#endif

#ifdef CMAKE_BOOTSTRAP
#define UV_USE_SIGCHLD
#elif defined(UV_HAVE_KQUEUE)
#include <sys/event.h>
#elif !defined(__amigaos4__)
#define UV_USE_SIGCHLD
#endif

#ifdef UV_USE_SIGCHLD
static void uv__chld(uv_signal_t* handle, int signum) {
  assert(signum == SIGCHLD);
  uv__wait_children(handle->loop);
}
#endif

#ifdef __amigaos4__
#include <proto/dos.h>
static APTR
hook_function(struct Hook *hook, APTR userdata, struct Process *process) {
    uint32 pid = (uint32) userdata;
    (void) (hook);

    if (process->pr_ProcessID == pid) {
        return process;
    }

    return 0;
}
#endif

void uv__wait_children(uv_loop_t* loop) {
  uv_process_t* process;
  int exit_status;
  int term_signal;
#if 1//ndef __amigaos4__
  int status;
  int options;
#endif
  pid_t pid;
  QUEUE pending;
  QUEUE* q;
  QUEUE* h;

  QUEUE_INIT(&pending);

  h = &loop->process_handles;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

#if 0//def __amigaos4__
    pid = 0;
    struct Hook h_ook = {{NULL, NULL}, (HOOKFUNC) hook_function, NULL, NULL};

    int32 r = IDOS->ProcessScan(&h_ook, (CONST_APTR) process->pid, 0);
    if (r > 0)
      IDOS->WaitForChildExit(process->pid);

    pid = process->pid;
#else // __amigaos4__

#if !defined(UV_USE_SIGCHLD)
    if ((process->flags & UV_HANDLE_REAP) == 0)
      continue;
    options = 0;
    process->flags &= ~UV_HANDLE_REAP;
#else
    options = WNOHANG;
#endif

    do
      pid = waitpid(process->pid, &status, options);
    while (pid == -1 && errno == EINTR);


#ifdef UV_USE_SIGCHLD
    if (pid == 0) /* Not yet exited */
      continue;
#endif
#endif //__amigaos4__

    if (pid == -1) {
      if (errno != ECHILD)
        abort();
      /* The child died, and we missed it. This probably means someone else
       * stole the waitpid from us. Handle this by not handling it at all. */
      continue;
    }

    assert(pid == process->pid);
#if 1//ndef __amigaos4__
    // process->status is assigned in the FinalCode
    process->status = status;
#endif
    QUEUE_REMOVE(&process->queue);
    QUEUE_INSERT_TAIL(&pending, &process->queue);
  }

  h = &pending;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    QUEUE_REMOVE(&process->queue);
    QUEUE_INIT(&process->queue);
    uv__handle_stop(process);

    if (process->exit_cb == NULL)
      continue;

#if 0 //def __amigaos4__
    exit_status = process->status;
    term_signal = 0; //todo
#else
    exit_status = 0;
    if (WIFEXITED(process->status))
      exit_status = WEXITSTATUS(process->status);

    term_signal = 0;
    if (WIFSIGNALED(process->status))
      term_signal = WTERMSIG(process->status);
#endif

    process->exit_cb(process, exit_status, term_signal);
  }
  assert(QUEUE_EMPTY(&pending));
}

/*
 * Used for initializing stdio streams like options.stdin_stream. Returns
 * zero on success. See also the cleanup section in uv_spawn().
 */
static int uv__process_init_stdio(uv_stdio_container_t* container, int fds[2]) {
  int mask;
  int fd;

  mask = UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM;

  switch (container->flags & mask) {
  case UV_IGNORE:
    return 0;

  case UV_CREATE_PIPE:
    assert(container->data.stream != NULL);
    if (container->data.stream->type != UV_NAMED_PIPE)
      return UV_EINVAL;
    else
      return uv_socketpair(SOCK_STREAM, 0, fds, 0, 0);

  case UV_INHERIT_FD:
  case UV_INHERIT_STREAM:
    if (container->flags & UV_INHERIT_FD)
      fd = container->data.fd;
    else
      fd = uv__stream_fd(container->data.stream);

    if (fd == -1)
      return UV_EINVAL;

    fds[1] = fd;
    return 0;

  default:
    assert(0 && "Unexpected flags");
    return UV_EINVAL;
  }
}


static int uv__process_open_stream(uv_stdio_container_t* container,
                                   int pipefds[2]) {
  int flags;
  int err;

  if (!(container->flags & UV_CREATE_PIPE) || pipefds[0] < 0)
    return 0;

  err = uv__close(pipefds[1]);
  if (err != 0)
    abort();

  pipefds[1] = -1;
  uv__nonblock(pipefds[0], 1);

  flags = 0;
  if (container->flags & UV_WRITABLE_PIPE)
    flags |= UV_HANDLE_READABLE;
  if (container->flags & UV_READABLE_PIPE)
    flags |= UV_HANDLE_WRITABLE;

  return uv__stream_open(container->data.stream, pipefds[0], flags);
}


static void uv__process_close_stream(uv_stdio_container_t* container) {
  if (!(container->flags & UV_CREATE_PIPE)) return;
  uv__stream_close(container->data.stream);
}


static void uv__write_int(int fd, int val) {
  ssize_t n;

  do
    n = write(fd, &val, sizeof(val));
  while (n == -1 && errno == EINTR);

  /* The write might have failed (e.g. if the parent process has died),
   * but we have nothing left but to _exit ourself now too. */
  _exit(127);
}


static void uv__write_errno(int error_fd) {
  uv__write_int(error_fd, UV__ERR(errno));
}


#if !(defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH))
/* execvp is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED, so must be
 * avoided. Since this isn't called on those targets, the function
 * doesn't even need to be defined for them.
 */
static void uv__process_child_init(const uv_process_options_t* options,
                                   int stdio_count,
                                   int (*pipes)[2],
                                   int error_fd) {
  sigset_t signewset;
  int close_fd;
  int use_fd;
  int fd;
  int n;
#ifndef CMAKE_BOOTSTRAP
#if defined(__linux__) || defined(__FreeBSD__)
  int r;
  int i;
  int cpumask_size;
  uv__cpu_set_t cpuset;
#endif
#endif

  /* Reset signal disposition first. Use a hard-coded limit because NSIG is not
   * fixed on Linux: it's either 32, 34 or 64, depending on whether RT signals
   * are enabled. We are not allowed to touch RT signal handlers, glibc uses
   * them internally.
   */
  for (n = 1; n < 32; n += 1) {
    if (n == SIGKILL || n == SIGSTOP)
      continue;  /* Can't be changed. */

#if defined(__HAIKU__)
    if (n == SIGKILLTHR)
      continue;  /* Can't be changed. */
#endif

    if (SIG_ERR != signal(n, SIG_DFL))
      continue;

    uv__write_errno(error_fd);
  }

  if (options->flags & UV_PROCESS_DETACHED)
    setsid();

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0 || use_fd >= fd)
      continue;
#ifdef F_DUPFD_CLOEXEC /* POSIX 2008 */
    pipes[fd][1] = fcntl(use_fd, F_DUPFD_CLOEXEC, stdio_count);
#else
    pipes[fd][1] = fcntl(use_fd, F_DUPFD, stdio_count);
#endif
    if (pipes[fd][1] == -1)
      uv__write_errno(error_fd);
#ifndef F_DUPFD_CLOEXEC /* POSIX 2008 */
    n = uv__cloexec(pipes[fd][1], 1);
    if (n)
      uv__write_int(error_fd, n);
#endif
  }

  for (fd = 0; fd < stdio_count; fd++) {
    close_fd = -1;
    use_fd = pipes[fd][1];

    if (use_fd < 0) {
      if (fd >= 3)
        continue;
      else {
        /* Redirect stdin, stdout and stderr to /dev/null even if UV_IGNORE is
         * set. */
        uv__close_nocheckstdio(fd); /* Free up fd, if it happens to be open. */
        use_fd = open("/dev/null", fd == 0 ? O_RDONLY : O_RDWR);
        close_fd = use_fd;

        if (use_fd < 0)
          uv__write_errno(error_fd);
      }
    }

    if (fd == use_fd) {
      if (close_fd == -1) {
        n = uv__cloexec(use_fd, 0);
        if (n)
          uv__write_int(error_fd, n);
      }
    }
    else {
      fd = dup2(use_fd, fd);
    }

    if (fd == -1)
      uv__write_errno(error_fd);

    if (fd <= 2 && close_fd == -1)
      uv__nonblock_fcntl(fd, 0);

    if (close_fd >= stdio_count)
      uv__close(close_fd);
  }

  if (options->cwd != NULL && chdir(options->cwd))
    uv__write_errno(error_fd);

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* When dropping privileges from root, the `setgroups` call will
     * remove any extraneous groups. If we don't call this, then
     * even though our uid has dropped, we may still have groups
     * that enable us to do super-user things. This will fail if we
     * aren't root, so don't bother checking the return value, this
     * is just done as an optimistic privilege dropping function.
     */
    SAVE_ERRNO(setgroups(0, NULL));
  }

  if ((options->flags & UV_PROCESS_SETGID) && setgid(options->gid))
    uv__write_errno(error_fd);

  if ((options->flags & UV_PROCESS_SETUID) && setuid(options->uid))
    uv__write_errno(error_fd);

#ifndef CMAKE_BOOTSTRAP
#if defined(__linux__) || defined(__FreeBSD__)
  if (options->cpumask != NULL) {
    cpumask_size = uv_cpumask_size();
    assert(options->cpumask_size >= (size_t)cpumask_size);

    CPU_ZERO(&cpuset);
    for (i = 0; i < cpumask_size; ++i) {
      if (options->cpumask[i]) {
        CPU_SET(i, &cpuset);
      }
    }

    r = -pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (r != 0) {
      uv__write_int(error_fd, r);
      _exit(127);
    }
  }
#endif
#endif

  if (options->env != NULL)
    environ = options->env;

  /* Reset signal mask just before exec. */
  sigemptyset(&signewset);
  if (sigprocmask(SIG_SETMASK, &signewset, NULL) != 0)
    abort();

#ifdef __MVS__
  execvpe(options->file, options->args, environ);
#else
  execvp(options->file, options->args);
#endif

  uv__write_errno(error_fd);
}
#endif


#if defined(UV_USE_APPLE_POSIX_SPAWN)
typedef struct uv__posix_spawn_fncs_tag {
  struct {
    int (*addchdir_np)(const posix_spawn_file_actions_t *, const char *);
  } file_actions;
} uv__posix_spawn_fncs_t;


static uv_once_t posix_spawn_init_once = UV_ONCE_INIT;
static uv__posix_spawn_fncs_t posix_spawn_fncs;
static int posix_spawn_can_use_setsid;


static void uv__spawn_init_posix_spawn_fncs(void) {
  /* Try to locate all non-portable functions at runtime */
  posix_spawn_fncs.file_actions.addchdir_np =
    dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addchdir_np");
}


static void uv__spawn_init_can_use_setsid(void) {
  int which[] = {CTL_KERN, KERN_OSRELEASE};
  unsigned major;
  unsigned minor;
  unsigned patch;
  char buf[256];
  size_t len;

  len = sizeof(buf);
  if (sysctl(which, ARRAY_SIZE(which), buf, &len, NULL, 0))
    return;

  /* NULL specifies to use LC_C_LOCALE */
  if (3 != sscanf_l(buf, NULL, "%u.%u.%u", &major, &minor, &patch))
    return;

  posix_spawn_can_use_setsid = (major >= 19);  /* macOS Catalina */
}


static void uv__spawn_init_posix_spawn(void) {
  /* Init handles to all potentially non-defined functions */
  uv__spawn_init_posix_spawn_fncs();

  /* Init feature detection for POSIX_SPAWN_SETSID flag */
  uv__spawn_init_can_use_setsid();
}


static int uv__spawn_set_posix_spawn_attrs(
    posix_spawnattr_t* attrs,
    const uv__posix_spawn_fncs_t* posix_spawn_fncs,
    const uv_process_options_t* options) {
  int err;
  unsigned int flags;
  sigset_t signal_set;

  err = posix_spawnattr_init(attrs);
  if (err != 0) {
    /* If initialization fails, no need to de-init, just return */
    return err;
  }

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* kauth_cred_issuser currently requires exactly uid == 0 for these
     * posixspawn_attrs (set_groups_np, setuid_np, setgid_np), which deviates
     * from the normal specification of setuid (which also uses euid), and they
     * are also undocumented syscalls, so we do not use them. */
    err = ENOSYS;
    goto error;
  }

  /* Set flags for spawn behavior
   * 1) POSIX_SPAWN_CLOEXEC_DEFAULT: (Apple Extension) All descriptors in the
   *    parent will be treated as if they had been created with O_CLOEXEC. The
   *    only fds that will be passed on to the child are those manipulated by
   *    the file actions
   * 2) POSIX_SPAWN_SETSIGDEF: Signals mentioned in spawn-sigdefault in the
   *    spawn attributes will be reset to behave as their default
   * 3) POSIX_SPAWN_SETSIGMASK: Signal mask will be set to the value of
   *    spawn-sigmask in attributes
   * 4) POSIX_SPAWN_SETSID: Make the process a new session leader if a detached
   *    session was requested. */
  flags = POSIX_SPAWN_CLOEXEC_DEFAULT |
          POSIX_SPAWN_SETSIGDEF |
          POSIX_SPAWN_SETSIGMASK;
  if (options->flags & UV_PROCESS_DETACHED) {
    /* If running on a version of macOS where this flag is not supported,
     * revert back to the fork/exec flow. Otherwise posix_spawn will
     * silently ignore the flag. */
    if (!posix_spawn_can_use_setsid) {
      err = ENOSYS;
      goto error;
    }

    flags |= POSIX_SPAWN_SETSID;
  }
  err = posix_spawnattr_setflags(attrs, flags);
  if (err != 0)
    goto error;

  /* Reset all signal the child to their default behavior */
  sigfillset(&signal_set);
  err = posix_spawnattr_setsigdefault(attrs, &signal_set);
  if (err != 0)
    goto error;

  /* Reset the signal mask for all signals */
  sigemptyset(&signal_set);
  err = posix_spawnattr_setsigmask(attrs, &signal_set);
  if (err != 0)
    goto error;

  return err;

error:
  (void) posix_spawnattr_destroy(attrs);
  return err;
}


static int uv__spawn_set_posix_spawn_file_actions(
    posix_spawn_file_actions_t* actions,
    const uv__posix_spawn_fncs_t* posix_spawn_fncs,
    const uv_process_options_t* options,
    int stdio_count,
    int (*pipes)[2]) {
  int fd;
  int fd2;
  int use_fd;
  int err;

  err = posix_spawn_file_actions_init(actions);
  if (err != 0) {
    /* If initialization fails, no need to de-init, just return */
    return err;
  }

  /* Set the current working directory if requested */
  if (options->cwd != NULL) {
    if (posix_spawn_fncs->file_actions.addchdir_np == NULL) {
      err = ENOSYS;
      goto error;
    }

    err = posix_spawn_fncs->file_actions.addchdir_np(actions, options->cwd);
    if (err != 0)
      goto error;
  }

  /* Do not return ENOSYS after this point, as we may mutate pipes. */

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0 || use_fd >= fd)
      continue;
    use_fd = stdio_count;
    for (fd2 = 0; fd2 < stdio_count; fd2++) {
      /* If we were not setting POSIX_SPAWN_CLOEXEC_DEFAULT, we would need to
       * also consider whether fcntl(fd, F_GETFD) returned without the
       * FD_CLOEXEC flag set. */
      if (pipes[fd2][1] == use_fd) {
        use_fd++;
        fd2 = 0;
      }
    }
    err = posix_spawn_file_actions_adddup2(
      actions,
      pipes[fd][1],
      use_fd);
    assert(err != ENOSYS);
    if (err != 0)
      goto error;
    pipes[fd][1] = use_fd;
  }

  /* Second, move the descriptors into their respective places */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0) {
      if (fd >= 3)
        continue;
      else {
        /* If ignored, redirect to (or from) /dev/null, */
        err = posix_spawn_file_actions_addopen(
          actions,
          fd,
          "/dev/null",
          fd == 0 ? O_RDONLY : O_RDWR,
          0);
        assert(err != ENOSYS);
        if (err != 0)
          goto error;
        continue;
      }
    }

    if (fd == use_fd)
        err = posix_spawn_file_actions_addinherit_np(actions, fd);
    else
        err = posix_spawn_file_actions_adddup2(actions, use_fd, fd);
    assert(err != ENOSYS);
    if (err != 0)
      goto error;

    /* Make sure the fd is marked as non-blocking (state shared between child
     * and parent). */
    uv__nonblock_fcntl(use_fd, 0);
  }

  /* Finally, close all the superfluous descriptors */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < stdio_count)
      continue;

    /* Check if we already closed this. */
    for (fd2 = 0; fd2 < fd; fd2++) {
      if (pipes[fd2][1] == use_fd)
          break;
    }
    if (fd2 < fd)
      continue;

    err = posix_spawn_file_actions_addclose(actions, use_fd);
    assert(err != ENOSYS);
    if (err != 0)
      goto error;
  }

  return 0;

error:
  (void) posix_spawn_file_actions_destroy(actions);
  return err;
}

char* uv__spawn_find_path_in_env(char** env) {
  char** env_iterator;
  const char path_var[] = "PATH=";

  /* Look for an environment variable called PATH in the
   * provided env array, and return its value if found */
  for (env_iterator = env; *env_iterator != NULL; env_iterator++) {
    if (strncmp(*env_iterator, path_var, sizeof(path_var) - 1) == 0) {
      /* Found "PATH=" at the beginning of the string */
      return *env_iterator + sizeof(path_var) - 1;
    }
  }

  return NULL;
}


static int uv__spawn_resolve_and_spawn(const uv_process_options_t* options,
                                       posix_spawnattr_t* attrs,
                                       posix_spawn_file_actions_t* actions,
                                       pid_t* pid) {
  const char *p;
  const char *z;
  const char *path;
  size_t l;
  size_t k;
  int err;
  int seen_eacces;

  path = NULL;
  err = -1;
  seen_eacces = 0;

  /* Short circuit for erroneous case */
  if (options->file == NULL)
    return ENOENT;

  /* The environment for the child process is that of the parent unless overriden
   * by options->env */
  char** env = environ;
  if (options->env != NULL)
    env = options->env;

  /* If options->file contains a slash, posix_spawn/posix_spawnp should behave
   * the same, and do not involve PATH resolution at all. The libc
   * `posix_spawnp` provided by Apple is buggy (since 10.15), so we now emulate it
   * here, per https://github.com/libuv/libuv/pull/3583. */
  if (strchr(options->file, '/') != NULL) {
    do
      err = posix_spawn(pid, options->file, actions, attrs, options->args, env);
    while (err == EINTR);
    return err;
  }

  /* Look for the definition of PATH in the provided env */
  path = uv__spawn_find_path_in_env(env);

  /* The following resolution logic (execvpe emulation) is copied from
   * https://git.musl-libc.org/cgit/musl/tree/src/process/execvp.c
   * and adapted to work for our specific usage */

  /* If no path was provided in env, use the default value
   * to look for the executable */
  if (path == NULL)
    path = _PATH_DEFPATH;

  k = strnlen(options->file, NAME_MAX + 1);
  if (k > NAME_MAX)
    return ENAMETOOLONG;

  l = strnlen(path, PATH_MAX - 1) + 1;

  for (p = path;; p = z) {
    /* Compose the new process file from the entry in the PATH
     * environment variable and the actual file name */
    char b[PATH_MAX + NAME_MAX];
    z = strchr(p, ':');
    if (!z)
      z = p + strlen(p);
    if ((size_t)(z - p) >= l) {
      if (!*z++)
        break;

      continue;
    }
    memcpy(b, p, z - p);
    b[z - p] = '/';
    memcpy(b + (z - p) + (z > p), options->file, k + 1);

    /* Try to spawn the new process file. If it fails with ENOENT, the
     * new process file is not in this PATH entry, continue with the next
     * PATH entry. */
    do
      err = posix_spawn(pid, b, actions, attrs, options->args, env);
    while (err == EINTR);

    switch (err) {
    case EACCES:
      seen_eacces = 1;
      break; /* continue search */
    case ENOENT:
    case ENOTDIR:
      break; /* continue search */
    default:
      return err;
    }

    if (!*z++)
      break;
  }

  if (seen_eacces)
    return EACCES;
  return err;
}


static int uv__spawn_and_init_child_posix_spawn(
    const uv_process_options_t* options,
    int stdio_count,
    int (*pipes)[2],
    pid_t* pid,
    const uv__posix_spawn_fncs_t* posix_spawn_fncs) {
  int err;
  posix_spawnattr_t attrs;
  posix_spawn_file_actions_t actions;

  err = uv__spawn_set_posix_spawn_attrs(&attrs, posix_spawn_fncs, options);
  if (err != 0)
    goto error;

  /* This may mutate pipes. */
  err = uv__spawn_set_posix_spawn_file_actions(&actions,
                                               posix_spawn_fncs,
                                               options,
                                               stdio_count,
                                               pipes);
  if (err != 0) {
    (void) posix_spawnattr_destroy(&attrs);
    goto error;
  }

  /* Try to spawn options->file resolving in the provided environment
   * if any */
  err = uv__spawn_resolve_and_spawn(options, &attrs, &actions, pid);
  assert(err != ENOSYS);

  /* Destroy the actions/attributes */
  (void) posix_spawn_file_actions_destroy(&actions);
  (void) posix_spawnattr_destroy(&attrs);

error:
  /* In an error situation, the attributes and file actions are
   * already destroyed, only the happy path requires cleanup */
  return UV__ERR(err);
}
#endif

#ifdef __amigaos4__
#include <proto/exec.h>
#include <proto/dos.h>
#include <unistd.h>
#include <dos.h>
struct EntryData {
  uint8 signal;
  struct Task *mainTask, *me;
  pid_t pid;
};
VOID amiga_EntryCode(int32 entry_data)
{
  struct EntryData *ed = (struct EntryData *)entry_data;

  ed->me = IExec->FindTask(0);
  struct Process *p = (struct Process *)ed->me;
  ed->pid = (pid_t)p->pr_ProcessID;

  // NOTES:
  // We need to make sure, that the spawned process exists, before the parent can continue.
  // See the notes for : uv__spawn_and_init_child_posix_spawn
  if(ed) IExec->Signal(ed->mainTask, 1 << ed->signal);

}
struct FinalData {
  uv_process_t *process;
};
VOID amiga_FinalCode(int32 return_code, APTR final_data)
{
  struct FinalData *fd = (struct FinalData *)final_data;
  fd->process->status = return_code;
  IExec->DebugPrintF("[amiga_FinalCode :] Ending child w\\return_code == 0x%lx\n", return_code);
  IExec->FreeVec(fd);
}
static int uv__do_create_new_process_amiga(uv_loop_t* loop,
                                          const uv_process_options_t* options,
                                          const uv_process_t *_process,
                                          int stdio_count,
                                          int (*pipes)[2],
                                          pid_t* pid)
{
  struct EntryData *ed = (struct EntryData*)malloc(sizeof(struct EntryData));

  ed->signal = IExec->AllocSignal(-1);
  ed->mainTask = IExec->FindTask(0);

  struct FinalData *fd = (struct FinalData*)IExec->AllocVecTags(sizeof(struct FinalData), 0);
  fd->process = _process;

IExec->DebugPrintF("[uv__process :] Calling spawnvpe.\n");

  int result = spawnvpe_callback(options->file, options->args, options->env, options->cwd, pipes[0][1], pipes[1][1], pipes[2][1], amiga_FinalCode, fd, amiga_EntryCode, ed);

IExec->DebugPrintF("[uv__process :] spawnvpe returned : %ld\n", result);

  if(result <= 0) return UV__ERR(result);

  // wait for the entry signal from the child :
  IExec->Wait(1 << ed->signal | SIGBREAKF_CTRL_C);
  IExec->FreeSignal(ed->signal);

  IExec->DebugPrintF("[uv__process :] Received signal from spawnee.\n");

  *pid = (pid_t)result;

  return 0;

#if ROEVBANAN

  struct name_translation_info nti_name;
  const char *name = options->file;

IExec->DebugPrintF("[uv__do_create_new_process_amiga :] New process \'%s\'\n", name);

  int error;
  if(error = __translate_unix_to_amiga_path_name(&name, &nti_name)) {
    printf("%s\n", strerror(error));
    return -1;
  }

  struct name_translation_info nti_cwd;
  const char *cwd = options->cwd;

  if(cwd && (error = __translate_unix_to_amiga_path_name(&cwd, &nti_cwd))) {
    printf("%s\n", strerror(error));
    return -1;
  }
  BPTR cwdLock = cwd ? IDOS->Lock(cwd, SHARED_LOCK) : 0;

  BPTR progdirLock = 0;
  BPTR fileLock = IDOS->Lock(name, SHARED_LOCK);
  if(fileLock) { progdirLock = IDOS->ParentDir(fileLock); IDOS->UnLock(fileLock); }

#if 0
	BPTR seglist = IDOS->LoadSeg(name);
	if (!seglist)
		return -1;
#endif

#if 0
  char** args = options->args;
  int argc = 1;
  int totalLen = 0;
  while(args[argc])
    totalLen += strlen(args[argc++])+1;

	char *mergeArgs = malloc(totalLen+1);
	memset(mergeArgs, 0, totalLen);

  char *mergePtr = mergeArgs;
  mergePtr[0] = '\0';
	for (int i = 1; i < argc; i++) {
    char *argPtr = args[i];
    while(*argPtr) *(mergePtr++) = *(argPtr++);
    if (i < argc-1) *(mergePtr++) = ' ';
	}
  *mergePtr = '\0';
#else
  char** args = options->args;
  int argc = 1;
  int totalLen = strlen(name)+1;
  while(args[argc])
    totalLen += strlen(args[argc++])+1;

	char *mergeArgs = malloc(totalLen+1);
	memset(mergeArgs, 0, totalLen);

  char *mergePtr = mergeArgs;
  mergePtr[0] = '\0';
  char *namePtr = name;
  for(int i = 0; i < strlen(name); i++) {
    *(mergePtr++) = *(namePtr++);
  }
  *(mergePtr++) = ' ';
	for (int i = 1; i < argc; i++) {
    char *argPtr = args[i];
    while(*argPtr) *(mergePtr++) = *(argPtr++);
    if (i < argc-1) *(mergePtr++) = ' ';
	}
  *mergePtr = '\0';
#endif

	IExec->DebugPrintF("[__uv_do_cnpa :] file to execute: %s\n", name);
	IExec->DebugPrintF("[__uv_do_cnpa :] args:            \"%s\"\n", mergeArgs);

  BPTR iofh[3] = { NULL, NULL, NULL };
  int closefh[3] = { FALSE, FALSE, FALSE };
  for(int i = 0; i < 3; i++) {
    if(stdio_count > i && pipes[i][1] >= 0) {
       BPTR fh;
       int err = __get_default_file(pipes[i][1], (long *)&fh);
       if(err) return 0;
       iofh[i] = IDOS->DupFileHandle(fh); //in case this is closed by the parent
       closefh[i] = TRUE;
    } else {
	    iofh[i]= IDOS->Open("NIL:", MODE_OLDFILE);
      closefh[i] = TRUE;
    }
  }

	struct Task *me = IExec->FindTask(NULL);

  struct EntryData *ed = (struct EntryData*)malloc(sizeof(struct EntryData));

  ed->signal = IExec->AllocSignal(-1);
  ed->mainTask = me;

  struct FinalData *fd = (struct FinalData*)IExec->AllocVecTags(sizeof(struct FinalData), 0);
  fd->process = _process;

  int result = IDOS->SystemTags((STRPTR) mergeArgs,
    NP_Cli,			TRUE,
    NP_Child,		TRUE,
    NP_NotifyOnDeathSigTask, me,

    SYS_Input, iofh[0],
    SYS_Output, iofh[1],
    SYS_Error, iofh[2],

    SYS_Asynch, TRUE,
    SYS_UserShell, TRUE,

    NP_EntryCode, amiga_EntryCode,
    NP_EntryData, ed,
    NP_FinalCode,	amiga_FinalCode,
    NP_FinalData,	fd,

    NP_Name, strdup(name),
    cwdLock ? NP_CurrentDir : TAG_SKIP, cwdLock,

    TAG_END);

#if 0
  struct Process *p = IDOS->CreateNewProcTags(
    NP_Seglist,		seglist,
    NP_FreeSeglist,	TRUE,

    NP_Cli,			TRUE,
    NP_Child,		TRUE,
    NP_NotifyOnDeathSigTask, me,

#if 1
    NP_Input,		iofh[0],
    NP_CloseInput,	closefh[0],
    NP_Output,		iofh[1],
    NP_CloseOutput,	closefh[1],
    NP_Error,		iofh[2],
    NP_CloseError,	closefh[2],
#else
    NP_Input,		IDOS->Input(),
    NP_CloseInput,	FALSE,
    NP_Output,		IDOS->Output(),
    NP_CloseOutput,	FALSE,
    NP_Error,		IDOS->ErrorOutput(),
    NP_CloseError,	FALSE,
#endif

    NP_EntryCode, amiga_EntryCode,
    NP_EntryData, ed,
    NP_FinalCode,	amiga_FinalCode,
    NP_FinalData,	fd,

    NP_Name,      strdup(name),
    cwdLock ? NP_CurrentDir : TAG_SKIP, cwdLock,
    progdirLock ? NP_ProgramDir : TAG_SKIP, progdirLock,
    NP_Arguments,	mergeArgs,
    TAG_DONE
  );
#endif

  IExec->DebugPrintF("[__uv_do_cnpa :] result : %d\n", result);

	if (result < 0) { // p == 0 for CNPT
    free(ed);
    for(int i = 0; i < 3; i++)
      if(closefh[i]) IDOS->Close(iofh[i]);
		return -1;
	}

  // *pid = p->pr_ProcessID;

  // wait for the entry signal from the child :
  IExec->Wait(1 << ed->signal | SIGBREAKF_CTRL_C);
  IExec->FreeSignal(ed->signal);

  IExec->DebugPrintF("[__uv_do_cnpa :] received signal from spawnee.\n");

  struct Process *p = (struct Process*)ed->me;
  *pid = p->pr_ProcessID;

  IExec->DebugPrintF("[__uv_do_cnpa :] pid : %d\n", *pid);

  free(ed);

  // success
  return 0;
#endif
}
#endif //__amigaos4__

#ifndef __amigaos4__
static int uv__spawn_and_init_child_fork(const uv_process_options_t* options,
                                         int stdio_count,
                                         int (*pipes)[2],
                                         int error_fd,
                                         pid_t* pid) {
  sigset_t signewset;
  sigset_t sigoldset;

  /* Start the child with most signals blocked, to avoid any issues before we
   * can reset them, but allow program failures to exit (and not hang). */
  sigfillset(&signewset);
  sigdelset(&signewset, SIGKILL);
  sigdelset(&signewset, SIGSTOP);
  sigdelset(&signewset, SIGTRAP);
  sigdelset(&signewset, SIGSEGV);
  sigdelset(&signewset, SIGBUS);
  sigdelset(&signewset, SIGILL);
  sigdelset(&signewset, SIGSYS);
  sigdelset(&signewset, SIGABRT);

  if (pthread_sigmask(SIG_BLOCK, &signewset, &sigoldset) != 0)
    abort();

  *pid = fork();

  if (*pid == 0) {
    /* Fork succeeded, in the child process */
    uv__process_child_init(options, stdio_count, pipes, error_fd);
    abort();
  }

  if (pthread_sigmask(SIG_SETMASK, &sigoldset, NULL) != 0)
    abort();

  if (*pid == -1)
    /* Failed to fork */
    return UV__ERR(errno);

  /* Fork succeeded, in the parent process */
  return 0;
}
#endif //__amigaos4__

static int uv__spawn_and_init_child(
    uv_loop_t* loop,
    const uv_process_options_t* options,
#ifdef __amigaos4__
    const uv_process_t *process,
#endif
    int stdio_count,
    int (*pipes)[2],
    pid_t* pid) {
  int signal_pipe[2] = { -1, -1 };
  int status;
  int err;
  int exec_errorno;
  ssize_t r;

#if defined(UV_USE_APPLE_POSIX_SPAWN)
  uv_once(&posix_spawn_init_once, uv__spawn_init_posix_spawn);

  /* Special child process spawn case for macOS Big Sur (11.0) onwards
   *
   * Big Sur introduced a significant performance degradation on a call to
   * fork/exec when the process has many pages mmaped in with MAP_JIT, like, say
   * a javascript interpreter. Electron-based applications, for example,
   * are impacted; though the magnitude of the impact depends on how much the
   * app relies on subprocesses.
   *
   * On macOS, though, posix_spawn is implemented in a way that does not
   * exhibit the problem. This block implements the forking and preparation
   * logic with posix_spawn and its related primitives. It also takes advantage of
   * the macOS extension POSIX_SPAWN_CLOEXEC_DEFAULT that makes impossible to
   * leak descriptors to the child process. */
  err = uv__spawn_and_init_child_posix_spawn(options,
                                             stdio_count,
                                             pipes,
                                             pid,
                                             &posix_spawn_fncs);

  /* The posix_spawn flow will return UV_ENOSYS if any of the posix_spawn_x_np
   * non-standard functions is both _needed_ and _undefined_. In those cases,
   * default back to the fork/execve strategy. For all other errors, just fail. */
  if (err != UV_ENOSYS)
    return err;

#endif

#if defined(__amigaos4__)

// err = spawnvpe(options->file, options->args, 0, options->cwd, pipes[0][1], pipes[1][1], pipes[2][1]);
// if(err > 0) *pid = err;
// else return UV_ENOSYS;
// return 0;




  err = uv__do_create_new_process_amiga(loop, options, process,
                                        stdio_count,
                                        pipes,
                                        pid);
  if(err)
{    // switch(IDOS->IoErr()) {
    //   case 
    printf("[uv__do_create_new_process_amiga :] Failed to create new process.\n");                    
        return UV_ENOSYS; //UV__ERR(__translate_io_error_to_errno(IDOS->IoErr()));
    // }
   }   return 0;
#else

  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it EOFs or errors with EPIPE.
   */
  err = uv__make_pipe(signal_pipe, 0);
  if (err)
    return err;

  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);

  err = uv__spawn_and_init_child_fork(options, stdio_count, pipes, signal_pipe[1], pid);

  /* Release lock in parent process */
  uv_rwlock_wrunlock(&loop->cloexec_lock);

  uv__close(signal_pipe[1]);

  if (err == 0) {
    do
      r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno));
    while (r == -1 && errno == EINTR);

    if (r == 0)
      ; /* okay, EOF */
    else if (r == sizeof(exec_errorno)) {
      do
        err = waitpid(*pid, &status, 0); /* okay, read errorno */
      while (err == -1 && errno == EINTR);
      assert(err == *pid);
      err = exec_errorno;
    } else if (r == -1 && errno == EPIPE) {
      /* Something unknown happened to our child before spawn */
      do
        err = waitpid(*pid, &status, 0); /* okay, got EPIPE */
      while (err == -1 && errno == EINTR);
      assert(err == *pid);
      err = UV_EPIPE;
    } else
      abort();
  }

  uv__close_nocheckstdio(signal_pipe[0]);

  return err;
#endif
}

int uv_spawn(uv_loop_t* loop,
             uv_process_t* process,
             const uv_process_options_t* options) {
#if defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
  /* fork is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED. */
  return UV_ENOSYS;
#else
  int pipes_storage[8][2];
  int (*pipes)[2];
  int stdio_count;
  pid_t pid;
  int err;
  int exec_errorno;
  int i;

  if (options->cpumask != NULL) {
#ifndef CMAKE_BOOTSTRAP
#if defined(__linux__) || defined(__FreeBSD__)
    if (options->cpumask_size < (size_t)uv_cpumask_size()) {
      return UV_EINVAL;
    }
#else
    return UV_ENOTSUP;
#endif
#else
    return UV_ENOTSUP;
#endif
  }

  assert(options->file != NULL);
  assert(!(options->flags & ~(UV_PROCESS_DETACHED |
                              UV_PROCESS_SETGID |
                              UV_PROCESS_SETUID |
                              UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME |
                              UV_PROCESS_WINDOWS_HIDE |
                              UV_PROCESS_WINDOWS_HIDE_CONSOLE |
                              UV_PROCESS_WINDOWS_HIDE_GUI |
                              UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS)));

  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  QUEUE_INIT(&process->queue);
  process->status = 0;

  stdio_count = options->stdio_count;
  if (stdio_count < 3)
    stdio_count = 3;

  err = UV_ENOMEM;
  pipes = pipes_storage;
  if (stdio_count > (int) ARRAY_SIZE(pipes_storage))
    pipes = uv__malloc(stdio_count * sizeof(*pipes));

  if (pipes == NULL)
    goto error;

  for (i = 0; i < stdio_count; i++) {
    pipes[i][0] = -1;
    pipes[i][1] = -1;
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_init_stdio(options->stdio + i, pipes[i]);
    if (err)
      goto error;
  }

#ifdef UV_USE_SIGCHLD
  uv_signal_start(&loop->child_watcher, uv__chld, SIGCHLD);
#endif

  /* Spawn the child */
  exec_errorno = uv__spawn_and_init_child(loop, options,
#ifdef __amigaos4__
                          process,
#endif
                          stdio_count, pipes, &pid);

#if 0
  /* This runs into a nodejs issue (it expects initialized streams, even if the
   * exec failed).
   * See https://github.com/libuv/libuv/pull/3107#issuecomment-782482608 */
  if (exec_errorno != 0)
      goto error;
#endif

  /* Activate this handle if exec() happened successfully, even if we later
   * fail to open a stdio handle. This ensures we can eventually reap the child
   * with waitpid. */
  if (exec_errorno == 0) {
#if !defined(UV_USE_SIGCHLD) && !defined(__amigaos4__)
    struct kevent event;
    EV_SET(&event, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, 0);
    if (kevent(loop->backend_fd, &event, 1, NULL, 0, NULL)) {
      if (errno != ESRCH)
        abort();
      /* Process already exited. Call waitpid on the next loop iteration. */
      process->flags |= UV_HANDLE_REAP;
      loop->flags |= UV_LOOP_REAP_CHILDREN;
    }
#endif

    process->pid = pid;
    process->exit_cb = options->exit_cb;
    QUEUE_INSERT_TAIL(&loop->process_handles, &process->queue);
    uv__handle_start(process);
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_open_stream(options->stdio + i, pipes[i]);
    if (err == 0)
      continue;

    while (i--)
      uv__process_close_stream(options->stdio + i);

    goto error;
  }

  if (pipes != pipes_storage)
    uv__free(pipes);

  return exec_errorno;

error:
  if (pipes != NULL) {
    for (i = 0; i < stdio_count; i++) {
      if (i < options->stdio_count)
        if (options->stdio[i].flags & (UV_INHERIT_FD | UV_INHERIT_STREAM))
          continue;
      if (pipes[i][0] != -1)
        uv__close_nocheckstdio(pipes[i][0]);
      if (pipes[i][1] != -1)
        uv__close_nocheckstdio(pipes[i][1]);
    }

    if (pipes != pipes_storage)
      uv__free(pipes);
  }

  return err;
#endif
}


int uv_process_kill(uv_process_t* process, int signum) {
  return uv_kill(process->pid, signum);
}


int uv_kill(int pid, int signum) {
  if (kill(pid, signum)) {
#if defined(__MVS__)
    /* EPERM is returned if the process is a zombie. */
    siginfo_t infop;
    if (errno == EPERM &&
        waitid(P_PID, pid, &infop, WNOHANG | WNOWAIT | WEXITED) == 0)
      return 0;
#endif
    return UV__ERR(errno);
  } else
    return 0;
}


void uv__process_close(uv_process_t* handle) {
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  if (QUEUE_EMPTY(&handle->loop->process_handles))
    uv_signal_stop(&handle->loop->child_watcher);
}
