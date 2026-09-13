# mini-project-one
# Compilation commands for c-shell:
```bash
make clean
```
```bash
make
```
```bash
./shell.out
```

# Compilation commands for xv6:
```bash
make clean
```
For the round-robin scheduler:
```bash
make qemu
```
For the FIFO scheduler:
```bash
make qemu SCHEDULER=FIFO
```
For the MLFQ scheduler:
```bash
make qemu SCHEDULER=MLFQ
```

# Project directory structure:
mini-project1/
├── c-shell/
│   ├── src/
│   ├── include/
│   └── Makefile
├── xv6/
│   ├── user/
│   ├── mkfs/
│   ├── kernel/
|   ├── graph.py
|   ├── LICENSE
│   ├── Makefile
|   ├── README.md
|   ├── test-xv6.py
│   └── report.pdf
├── AI-usage.pdf
└── README.md

# Design choices:
## C-Shell
1. Command parsing: Input is tokenized into commands and arguments, with special handling for operators such as ;, |, <, >, >>, and &. Built-in commands are handled by the shell itself, while external commands are executed using fork()/exec().
2. Pipelines and redirection: Pipelines are implemented using Unix pipes, with each pipeline stage executed as a separate child process. Input/output redirection is performed using dup2() before executing the command.
3. Background jobs: Commands ending with & are treated as background processes. The shell maintains job information such as job number, PID, command, and state so that jobs can be managed using the implemented job-control commands.
4. Signal and terminal control: The shell remains in control of the terminal while managing foreground jobs. Signals such as SIGINT and SIGTSTP are forwarded/handled appropriately so that foreground processes can be interrupted or stopped without terminating the shell.
5. Process state tracking: Job state is maintained using child-status information obtained through waitpid() and signal handling, allowing stopped, continued, and terminated processes to be tracked.

## xv6 Scheduler
1. Compile-time scheduler selection: The scheduler is selected using the SCHEDULER build variable. If no scheduler is specified, the original xv6 Round-Robin scheduler remains unchanged; SCHEDULER=MLFQ enables the MLFQ implementation.
2. MLFQ organization: Four queues are used, with Q0 as the highest priority and Q3 as the lowest. New processes enter Q0, and CPU-bound processes are demoted after exhausting their time slices.
3. Time-slice policy: The queue time slices are fixed at 1, 4, 8, and 16 ticks for Q0–Q3 respectively. Q3 remains round-robin when processes exhaust their slices.
4. Priority and queue ordering: The scheduler always chooses a runnable process from the highest-priority non-empty queue. An enqueue sequence number is used to maintain ordering among processes within the same queue.
5. Priority boosting: Every 48 ticks, processes are promoted back to Q0 to prevent starvation. Voluntary yields preserve the process's current queue instead of causing demotion.
6. Scheduler instrumentation: Additional per-process bookkeeping (queue, slice_ticks, ticks_since_boost, enqueue_seq, and run_ticks) is maintained to implement the scheduler and expose its state through procdump().
