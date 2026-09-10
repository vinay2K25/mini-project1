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

int
main(void)
{
  int i;
  int pid;

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