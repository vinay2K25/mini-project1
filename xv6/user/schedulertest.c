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

// Test round-robin scheduling in Q3.
//
// Two long-running CPU-bound processes are created. Both processes
// should eventually reach Q3. Since Q3 is the lowest-priority queue,
// they should alternate after exhausting their 16-tick time slices.
//
// We avoid the 48-tick priority-boost boundary while starting the
// test so that the observed Q3 behavior is not confused by a boost.
static void
q3_test(void)
{
  int pid;
  int i;

  printf("=== Q3 round-robin test ===\n");

  // Avoid starting too close to a 48-tick priority boost.
  while ((uptime() % 48) >= 24)
    pause(1);

  printf("Test starting at tick %d\n", (int)uptime());

  // Create two long-running CPU-bound processes.
  for (i = 0; i < 2; i++) {
    pid = fork();

    if (pid < 0) {
      printf("q3 test: fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      uint64 j;

      printf("Q3 process %d (pid %d) started at tick %d\n",
             i + 1, getpid(), (int)uptime());

      // Long enough to reach Q3 and consume several Q3 slices.
      for (j = 0; j < 1000000000ULL; j++) {
        if ((j % 100000000ULL) == 0)
          asm volatile("" ::: "memory");
      }

      printf("Q3 process %d (pid %d) finished at tick %d\n",
             i + 1, getpid(), (int)uptime());
      exit(0);
    }
  }

  // Wait for both Q3 processes.
  wait(0);
  wait(0);

  printf("=== Q3 round-robin test complete at tick %d ===\n",
         (int)uptime());
}

// Test MLFQ behavior with processes having different CPU-burst lengths.
//
// The processes deliberately perform different amounts of CPU work.
// Shorter jobs should finish earlier, while longer jobs should
// progressively move through the MLFQ queues.
//
// This workload is also suitable for generating the scheduler
// timeline required in the project report.
static void
burst_test(void)
{
  int pid;
  int i;

  // Different CPU-burst lengths.
  //
  // The values are deliberately separated so that the processes
  // exhibit noticeably different execution times.
  uint64 bursts[4] = {
    100000000ULL,
    250000000ULL,
    500000000ULL,
    1000000000ULL
  };

  printf("=== Varying CPU-burst test ===\n");
  printf("Test starting at tick %d\n", (int)uptime());

  for (i = 0; i < 4; i++) {
    pid = fork();

    if (pid < 0) {
      printf("burst test: fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      uint64 j;
      uint64 start;
      uint64 finish;

      start = uptime();

      printf("Burst process %d (pid %d) started at tick %d\n",
             i + 1, getpid(), (int)start);

      // CPU-bound burst of the selected length.
      for (j = 0; j < bursts[i]; j++) {
        if ((j % 100000000ULL) == 0)
          asm volatile("" ::: "memory");
      }

      finish = uptime();

      printf("Burst process %d (pid %d) finished at tick %d "
             "(CPU time %d ticks)\n",
             i + 1, getpid(), (int)finish,
             (int)(finish - start));

      exit(0);
    }
  }

  // Wait for all burst processes.
  for (i = 0; i < 4; i++)
    wait(0);

  printf("=== Varying CPU-burst test complete at tick %d ===\n",
         (int)uptime());
}

// Compare FIFO, RR, and MLFQ using the same CPU-burst workload.
//
// Each child receives a different CPU-burst length. We record:
//   - arrival: when the parent creates the child
//   - first_run: when the child first gets to execute
//   - completion: when the child finishes
//
// From these timestamps we can calculate:
//   response   = first_run - arrival
//   turnaround = completion - arrival
//
// Waiting time will be calculated later using kernel-side CPU
// service-time accounting.
static void
comparison_test(void)
{
  int pid;
  int i;

  uint64 bursts[4] = {
    50000000ULL,
    100000000ULL,
    200000000ULL,
    400000000ULL
  };

  uint64 arrival[4];

  printf("=== Scheduler comparison test ===\n");
  printf("Test starting at tick %d\n", (int)uptime());

  for (i = 0; i < 4; i++) {
    // Record the approximate arrival tick immediately before fork.
    arrival[i] = uptime();

    pid = fork();

    if (pid < 0) {
      printf("comparison test: fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      uint64 j;
      uint64 first_run;
      uint64 finish;
      uint64 response;
      uint64 turnaround;

      uint64 service;
      uint64 waiting;

      // The first instructions executed by the child occur when
      // the scheduler gives it CPU time.
      first_run = uptime();

      printf("Comparison process %d (pid %d): "
             "arrival=%d first_run=%d response=%d\n",
             i + 1, getpid(), (int)arrival[i],
             (int)first_run,
             (int)(first_run - arrival[i]));

      // CPU-bound workload.
      for (j = 0; j < bursts[i]; j++) {
        if ((j % 100000000ULL) == 0)
          asm volatile("" ::: "memory");
      }

      finish = uptime();
      turnaround = finish - arrival[i];

      response = first_run - arrival[i];

      service = getrunticks();
      waiting = turnaround - service;

      printf("Comparison process %d (pid %d): "
       "finish=%d response=%d turnaround=%d "
       "service=%d waiting=%d\n",
       i + 1, getpid(), (int)finish,
       (int)response,
       (int)turnaround,
       (int)service,
       (int)waiting);

      exit(0);
    }
  }

  for (i = 0; i < 4; i++)
    wait(0);

  printf("=== Scheduler comparison test complete at tick %d ===\n",
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

  if (argc == 2 && strcmp(argv[1], "q3") == 0) {
    q3_test();
    exit(0);
  }

  // Run the varying CPU-burst workload when requested.
  if (argc == 2 && strcmp(argv[1], "bursts") == 0) {
    burst_test();
    exit(0);
  }

  // Run the common workload used for comparing FIFO, RR, and MLFQ.
  if (argc == 2 && strcmp(argv[1], "comparison") == 0) {
    comparison_test();
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