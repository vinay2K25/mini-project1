#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Keep the CPU busy for a while so the process consumes
// multiple MLFQ time slices and gets demoted through the queues.
static void
cpu_burst(int id)
{
  volatile uint64 i;
  uint64 start = uptime();

  printf("schedulertest: process %d (pid %d) started at tick %d\n",
         id, getpid(), (int)start);

  // A deliberately long CPU-bound loop.
  // The exact iteration count is not important; the goal is
  // simply to keep the process runnable for many timer ticks.
  for (i = 0; i < 500000000ULL; i++) {
    // Prevent the compiler from optimizing the loop away.
    if ((i % 100000000ULL) == 0) {
      asm volatile("" ::: "memory");
    }
  }

  printf("schedulertest: process %d (pid %d) finished at tick %d\n",
         id, getpid(), (int)uptime());
}

// Test strict-priority preemption.
//
// Process A is a long-running CPU-bound process. The parent waits
// for a short period so A can move down to a lower-priority queue.
// The parent then creates Process B, which starts in Q0.
//
// The important observation is that B must run before A can finish
// its current lower-priority time slice.
static void
priority_test(void)
{
  int pid;
  uint64 start;

  printf("=== Strict-priority preemption test ===\n");
  printf("Test starting at tick %d\n", (int)uptime());

  // Create Process A.
  pid = fork();

  if (pid < 0) {
    printf("priority test: first fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    uint64 i;

    printf("Process A (pid %d) started at tick %d\n",
           getpid(), (int)uptime());

    // Long CPU-bound workload.
    for (i = 0; i < 500000000ULL; i++) {
      if ((i % 100000000ULL) == 0)
        asm volatile("" ::: "memory");
    }

    printf("Process A (pid %d) finished at tick %d\n",
           getpid(), (int)uptime());
    exit(0);
  }

  // Wait until A has had enough CPU time to reach Q2, while
  // avoiding the 48-tick priority-boost boundary.
  while ((uptime() % 48) >= 40)
    pause(1);

  pause(5);

  start = uptime();

  printf("Parent creating Process B at tick %d\n", (int)start);

  // Create Process B. A new process always starts in Q0.
  pid = fork();

  if (pid < 0) {
    printf("priority test: second fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    uint64 i;

    printf("Process B (pid %d) started at tick %d\n",
           getpid(), (int)uptime());

    // Short CPU burst.
    for (i = 0; i < 50000000ULL; i++) {
      if ((i % 10000000ULL) == 0)
        asm volatile("" ::: "memory");
    }

    printf("Process B (pid %d) finished at tick %d\n",
           getpid(), (int)uptime());
    exit(0);
  }

  // Wait for both children.
  wait(0);
  wait(0);

  printf("=== Strict-priority test complete at tick %d ===\n",
         (int)uptime());
}

int
main(int argc, char *argv[])
{
  int i;
  int pid;

  // Run the dedicated strict-priority test when requested.
  if (argc == 2 && strcmp(argv[1], "priority") == 0) {
    priority_test();
    exit(0);
  }

  printf("=== MLFQ scheduler test ===\n");
  printf("Starting CPU-bound workload at tick %d\n", (int)uptime());

  // Create several CPU-bound processes so that the scheduler
  // has to distribute CPU time among them.
  for (i = 0; i < 4; i++) {
    pid = fork();

    if (pid < 0) {
      printf("schedulertest: fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      cpu_burst(i + 1);
      exit(0);
    }
  }

  // Wait for all children.
  for (i = 0; i < 4; i++)
    wait(0);

  printf("=== MLFQ scheduler test complete at tick %d ===\n",
         (int)uptime());

  exit(0);
}