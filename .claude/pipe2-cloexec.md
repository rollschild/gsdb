# `pipe2()` and `O_CLOEXEC`

## Overview

The standard `pipe()` has no `close_on_exec` parameter. Use `pipe2()` instead, which accepts flags including `O_CLOEXEC`.

## What `O_CLOEXEC` does

When a process calls `fork()` + `exec()`, all open file descriptors are **inherited** by the new program by default. `O_CLOEXEC` marks the file descriptors so the kernel **automatically closes them** when `exec()` is called.

Without it, you'd have to manually call `fcntl(fd, F_SETFD, FD_CLOEXEC)` on each fd after `pipe()` — and there's a race window between `pipe()` and `fcntl()` where another thread could `fork()` and leak the fd.

## Example

```cpp
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>

int main() {
    int fds[2];

    // Without O_CLOEXEC: both fds survive across exec()
    // pipe(fds);

    // With O_CLOEXEC: both fds are automatically closed on exec()
    pipe2(fds, O_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: fds[0] and fds[1] are still open here (fork copies them).
        // But once we call exec(), the kernel closes them automatically
        // because of O_CLOEXEC.
        close(fds[1]);  // close write end, we only need read in child pre-exec

        // After this exec(), fds[0] is gone — the new program never sees it
        execlp("/bin/sleep", "sleep", "10", nullptr);
    }

    // Parent: use the pipe normally
    close(fds[0]);            // close read end
    write(fds[1], "hi", 2);   // write to child
    close(fds[1]);
}
```

## Relevance to gsdb

In `process::launch()` (`src/process.cpp:225`), we do `fork()` + `execlp()`. This is now wired up: `gsdb::pipe`'s constructor takes a `close_on_exec` flag and calls `pipe2(fds_, close_on_exec ? O_CLOEXEC : 0)` (`src/pipe.cpp:13`), and `launch()` creates its error-reporting channel with `pipe channel(/*close_on_exec=*/true)` (`src/process.cpp:227`). That ensures the pipe fds don't leak into the debuggee — the tracee only has its own stdin/stdout/stderr, not stray fds from the debugger's internals. Tests that capture the child's stdout pass their own write fd via `launch(path, true, channel.get_write())`, which the child installs with `dup2()` (`src/process.cpp:256`); the `anti_debugger` target deliberately constructs its pipe with `close_on_exec = false` because it must keep writing across `exec`.
