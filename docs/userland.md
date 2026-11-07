# POSIX-ish userland (0.6)

Release 0.6 lets ordinary portable C programs be cross-compiled and run
unmodified: a C library (newlib), a POSIX process model (fork, execve,
posix_spawn, waitpid, threads), descriptors, pipes, signals, permissions, a
pseudo-terminal with a line discipline, a shell with pipelines, small
utilities and a ported interpreter (Lua 5.4.9). Decisions are in
[decisions.md](decisions.md) (D-0.6-*), test evidence in `STATUS.md`.

## ABI and toolchain (0.6a)

* **Target:** `clang --target=x86_64-unknown-none-elf -D__aurora__`, static,
  non-PIE, loaded at 0x400000 (`user/linker.ld`: text RX, rodata R, data RW
  and a PT_TLS header). `tools/aurora_cc.py` compiles and links a program;
  programs in `user/bin/*.c` are built by `make`.
* **Sysroot:** `build/sysroot/include` (newlib headers, Aurora's overrides in
  `user/libc/include`, `aurora_abi.h`) and `build/sysroot/lib` (`crt0.o`,
  `libaurora.a`, `libc.a`, `libm.a`). `tools/userland.py` builds it.
* **System calls:** `int $0x80`, number in rax, arguments in rdi, rsi, rdx,
  r10, r8, r9, result or `-errno` in rax (`kernel/include/abi/aurora_abi.h`).
* **Process entry:** RSP points at `argc`, followed by `argv[]`, NULL,
  `envp[]`, NULL and an auxiliary vector (AT_PHDR, AT_PHNUM, AT_PAGESZ,
  AT_ENTRY, AT_UID, AT_GID, AT_RANDOM); the program headers are copied onto
  the stack so AT_PHDR always works.
* **TLS:** x86-64 variant II. `start.c` copies the PT_TLS image below a
  thread control block whose first word points at itself and loads it with
  `SYS_SET_FSBASE`; the kernel saves and restores FS_BASE per thread.
  newlib is built with `_REENT_THREAD_LOCAL`, so `errno` is thread-local.
* **FPU/SSE:** CR4.OSFXSR/OSXMMEXCPT on every CPU; each user thread has an
  FXSAVE area saved and restored on every context switch (the kernel itself
  is compiled with `-mgeneral-regs-only`). Signal frames carry it too.

## C library (0.6b)

newlib 4.6.0 supplies ISO C, stdio, stdlib, string, time, regex, glob,
getopt, fnmatch, `dirent` (on `getdents`) and libm. Aurora's layer
(`user/libc/src`) provides the rest:

| File | Contents |
|---|---|
| `crt0.S`, `start.c` | entry, argv/envp/auxv, TLS for the main thread, constructors, `exit(main())` |
| `syscalls.c` | file I/O, `stat`, directories, `getcwd`/`realpath`, descriptors, `poll`/`select`, termios, processes, credentials, `sbrk`/`mmap`, clocks, `nanosleep`, `uname`, `sysconf` |
| `signal.c` | `sigaction`, `signal`, masks, `sigsuspend`, `raise`, the sigreturn trampoline |
| `pthread.c` | threads, futex mutexes (normal, recursive, error-checking), condition variables, once, keys, rwlocks, spinlocks, barriers |
| `locks.c` | newlib's retargetable locks (stdio, malloc, atexit, env) |
| `spawn.c` | `posix_spawn`/`posix_spawnp` on SYS_SPAWN, `system()` |
| `misc.c` | user table (`root`, `user`), `getline`, `flockfile`, `__errno` |
| `aurora.c` | `<aurora.h>`: the 0.4 socket API on descriptors, `aurora_sysinfo`; 0.7: shared memory, local sockets, `aurora_spawn`, power, kernel statistics |
| `abi_check.c` | build-time check that newlib's constants equal the kernel ABI |

## Process model (0.6c)

* **posix_spawn** is native: the kernel loads the image and gives the child
  exactly the descriptors in a map built from the file actions.
* **fork** shares every page copy-on-write (`memory/uvm.c`): both PTEs
  become read-only, the first write to a page still owned by both copies
  it, and a page whose other owner is gone is simply taken over. Page share
  counts live in the physical allocator.
* **Address spaces** are lists of regions (image segments, heap, stack,
  mmap). Pages appear on first touch: the stack region is 8 MiB but costs
  only what is used. `brk`, `mmap` (anonymous and private file mappings),
  `munmap` and `mprotect` edit regions; changes to live translations use
  TLB shootdowns before pages are freed.
* **execve** builds the new address space completely before switching, so a
  failing exec leaves the caller intact; other threads are ended first;
  close-on-exec descriptors close and caught signals reset to default.
* **waitpid** (pid, -1, process group, WNOHANG) with newlib's status
  encoding: exit code << 8, or the signal number. A process with a live
  parent stays a zombie until waited for (or is freed at once if the
  parent ignores SIGCHLD). Orphans are re-parented to **init** (pid 1,
  `/system/bin/init`), which reaps them; programs the Terminal starts have
  the kernel as parent and report their exit to the Terminal.
* **Threads:** up to 16 per process, created with SYS_THREAD_CREATE (entry,
  stack, argument, TLS pointer, clear-tid address). At thread exit the
  kernel zeroes the clear-tid word and wakes a futex waiter, which is how
  `pthread_join` waits. **Futexes** are keyed by the physical address of the
  word (made private first), with timeouts.

## Descriptors (0.6d)

Each process has 64 descriptors pointing at shared, reference-counted open
file descriptions (`core/file.c`): VFS files, directories, pipe ends,
sockets, `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/tty` and pty
slaves. `dup`, `dup2`/`dup3`, `fcntl` (F_DUPFD, F_GETFD/SETFD, F_GETFL/SETFL
for O_APPEND and O_NONBLOCK), `O_CLOEXEC`, `pipe2`, `poll` (pipes and ptys
wake pollers; sockets are re-checked every 10 ms) and `select` on top of
poll.

## Pipes and signals (0.6e)

* **Pipes:** 4096-byte buffer; writes of up to 4096 bytes are atomic; EOF
  once every writer has closed; a write with no readers raises SIGPIPE and
  fails with EPIPE; O_NONBLOCK gives EAGAIN.
* **Signals:** 31 signals with newlib's numbering. Process- and
  thread-directed pending sets, per-thread masks, `sigaction` with
  SA_RESTART, SA_NODEFER, SA_RESETHAND. Delivery happens on every return to
  user mode: the kernel pushes a frame (all general registers, RIP, RFLAGS,
  RSP, the old mask, FS base and the FXSAVE image) below the red zone and
  enters the handler with the libc trampoline as its return address;
  SYS_SIGRETURN restores everything (sanitising RFLAGS and MXCSR). A signal
  for a thread running on another CPU interrupts that CPU with an IPI.
  Blocking calls (pipe and pty reads, `waitpid`, futex, `nanosleep`, `poll`,
  sockets, `sigsuspend`) return EINTR; with SA_RESTART most are restarted.
  CPU exceptions become SIGSEGV, SIGILL, SIGFPE, SIGTRAP or SIGBUS for the
  faulting thread; a blocked or ignored synchronous signal still kills.
* Default actions: terminate, or ignore for SIGCHLD, SIGURG, SIGWINCH,
  SIGCONT and the job-control stop signals (no job control, D-0.6-6).

## Files (0.6f, 0.6g)

* `unlink`, `rename` (atomic; replaces an existing file; one PFS commit, so
  it is all-or-nothing on disk), `mkdir`, `rmdir`, `truncate`/`ftruncate`,
  `getdents`, per-process working directory with `..` resolution.
* `/tmp` is a second RAM file system (mode 1777, sticky). Renames between
  `/home` and `/tmp` fail with EXDEV.
* Every node has a mode, owner, group and modification time; PFS stores
  them (catalog entries carry a metadata flag, older catalogs load with
  defaults). Checks: read/write/execute on the target for `open` and
  `execve`; write+search on the parent directory to create, remove or
  rename; the sticky rule; owner or root for `chmod`; root for `chown`.
  Root bypasses read/write bits but needs an execute bit to run a file.
* Initramfs files are root-owned, `/system/bin` programs 0755, the rest
  0644. The desktop session runs as root (D-0.6-5).

## Terminal, pty and shell (0.6h)

* **pty** (`core/pty.c`): the Terminal is the master. Line discipline:
  canonical mode with erase (DEL/Backspace), kill (^U) and EOF (^D); echo;
  ICRNL; ONLCR; ISIG: ^C sends SIGINT, ^\ SIGQUIT, ^Z SIGTSTP to the
  foreground process group. termios via `tcgetattr`/`tcsetattr` (raw mode
  works), window size (`TIOCGWINSZ`, SIGWINCH on resize), `tcsetpgrp`.
  Closing the Terminal window hangs up the session (SIGHUP).
* **Terminal app:** built-in commands as before; any other word runs a
  program as a session leader on the pty. Lines with shell syntax
  (`| < > ; $ * &&`) go to `/system/bin/sh -c`. Output passes through a
  VT100 subset: CR, BS, TAB, and CSI K, J (2J), H/f, G, C, D and m colours.
  While a program runs in the foreground every key goes to the pty.
* **sh:** pipelines, `< > >> 2> 2>> 2>&1 >&2`, `; && || &`, quotes and
  escapes, `$VAR ${VAR} $? $$ $!`, globbing, `#` comments, builtins `cd pwd
  exit export unset set wait jobs true false : umask exec .`; `sh -c`,
  `sh script`. Interactive sh catches SIGINT so Ctrl+C ends the foreground
  program but not the shell.
* **Utilities** in `/system/bin`: `cat grep wc echo ls mkdir rmdir rm mv cp
  touch head kill sleep env chmod id uname pwd tee printf true false`, the
  0.2-0.5 programs (ported to the POSIX API) and the test programs
  (`posixtest proctest sigtest forktest permtest rentest`).
* **Lua 5.4.9** (`/system/bin/lua`, built with `LUA_USE_POSIX`), with its
  test suite in `/system/share/lua-tests`.

## System-call coverage

| Area | Calls | Status |
|---|---|---|
| processes | exit, thread_exit, fork, execve, spawn, waitpid, getpid, getppid, gettid, kill | implemented |
| signals | sigaction, sigprocmask, sigreturn, sigpending, sigsuspend, alarm | implemented |
| threads | thread_create, futex (wait/wake, timeout), set_fsbase | implemented |
| groups/ids | setpgid, getpgid, setsid, getuid, getgid, setuid, setgid, umask | implemented (no saved/effective ids) |
| time | nanosleep, clock_gettime (REALTIME from the RTC, MONOTONIC, process/thread CPU), uptime | implemented (4 ms resolution) |
| memory | brk, mmap (anonymous, private file), munmap, mprotect | implemented |
| shared memory (0.7) | shm_create; mmap MAP_SHARED of an shm object, or MAP_SHARED\|MAP_ANONYMOUS (shared with children) | implemented; shared file mappings: ENOTSUP |
| local IPC (0.7) | socketpair, local_listen, local_connect, accept, sendmsg, recvmsg (descriptor passing) | implemented (message sockets, own namespace; no BSD AF_UNIX) |
| files | open, close, read, write, lseek, fstat, stat, getdents, truncate, ftruncate, chmod, fchmod, chown, access, sync, fsync | implemented |
| directories | mkdir, rmdir, unlink, rename, chdir, getcwd | implemented; `fchdir` ENOSYS |
| descriptors | dup, dup3, fcntl, pipe2, poll, ioctl (termios, window size, FIONREAD, process group) | implemented |
| network | socket, connect, send, recv, resolve, bind, sendto, recvfrom, netinfo, ping | 0.4 API on descriptors (no BSD sockets) |
| system | getrandom, sysinfo, uname; 0.7: power, klog, cpuinfo, proclist, test_event/testop (test builds) | implemented (getrandom is not cryptographic) |
| links | link, symlink, readlink | not supported (EMLINK / ENOSYS / EINVAL) |
| devices (0.7) | /dev/ptmx (pty masters), /dev/display, /dev/input, /dev/console | implemented (see docs/desktop.md) |
| not present | job control, ptrace, setitimer, chroot, mount, select as a system call (libc builds it on poll) | not implemented |

## Limits

64 processes, 160 tasks, 16 threads per process, 64 descriptors per
process, 256 open VFS files system-wide, 128 KiB of argv+envp, 4 KiB pipes,
64 KiB per RAM/PFS file (unchanged from 0.3), file names `[A-Za-z0-9._-]`.
