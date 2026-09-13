import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict

# ============================================================
# GRAPH 1: EXPERIMENTAL MLFQ EXECUTION TRACE
# ============================================================

trace_text = """
14 4 1 0
15 5 1 0
16 6 1 0
17 7 1 0
18 4 1 1
19 4 1 2
20 4 1 3
21 4 2 0
22 5 1 1
23 5 1 2
24 5 1 3
25 5 2 0
26 6 1 1
27 6 1 2
28 6 1 3
29 6 2 0
30 7 1 1
31 7 1 2
32 7 1 3
33 7 2 0
34 4 2 1
35 4 2 2
36 4 2 3
37 4 2 4
38 4 2 5
39 4 2 6
40 4 2 7
41 4 3 0
42 5 2 1
43 5 2 2
44 5 2 3
45 5 2 4
46 5 2 5
47 5 2 6
48 5 2 7
49 5 3 0
50 4 1 0
51 5 1 0
52 6 1 0
53 7 1 0
54 4 1 1
55 4 1 2
56 4 1 3
57 4 2 0
58 5 1 1
59 5 1 2
60 5 1 3
61 5 2 0
62 6 1 1
63 6 1 2
64 6 1 3
65 6 2 0
66 7 1 1
67 7 1 2
68 7 1 3
69 7 2 0
70 4 2 1
71 4 2 2
72 4 2 3
73 4 2 4
74 4 2 5
75 4 2 6
76 4 2 7
77 4 3 0
78 5 2 1
79 5 2 2
80 5 2 3
81 5 2 4
82 5 2 5
83 5 2 6
84 5 2 7
85 5 3 0
86 6 2 1
87 6 2 2
88 6 2 3
89 6 2 4
90 6 2 5
91 6 2 6
92 6 2 7
93 6 3 0
94 7 2 1
95 7 2 2
96 7 2 3
97 7 2 4
98 7 2 5
99 7 2 6
100 7 2 7
101 7 3 0
102 4 1 0
103 5 1 0
104 6 1 0
105 7 1 0
106 4 1 1
107 4 1 2
108 4 1 3
109 4 2 0
110 5 1 1
111 5 1 2
112 5 1 3
113 6 1 1
114 6 1 2
115 6 1 3
116 6 2 0
117 7 1 1
118 7 1 2
119 7 1 3
120 7 2 0
121 6 2 1
122 6 2 2
123 6 2 3
124 6 2 4
125 6 2 5
126 6 2 6
127 6 2 7
128 6 3 0
129 7 2 1
130 7 2 2
131 7 2 3
132 7 2 4
133 7 2 5
134 7 2 6
135 7 2 7
"""

completion_ticks = {
    5: 112,
    4: 120,
    6: 127,
    7: 135,
}

process_names = {
    4: "Process 1",
    5: "Process 2",
    6: "Process 3",
    7: "Process 4",
}

trace = []
for line in trace_text.strip().splitlines():
    tick, pid, queue, slice_ticks = map(int, line.split())
    trace.append((tick, pid, queue, slice_ticks))

# mlfq_tick() reports the state AFTER the timer tick.
# Hence each record at tick T represents execution in the
# previous queue during the interval ending at T.
queue_by_pid = defaultdict(list)
previous_queue = {4: 0, 5: 0, 6: 0, 7: 0}

for tick, pid, new_queue, slice_ticks in trace:
    old_queue = previous_queue[pid]
    queue_by_pid[pid].append((tick - 1, old_queue))
    previous_queue[pid] = new_queue

fig, ax = plt.subplots(figsize=(14, 7))

for pid in sorted(queue_by_pid):
    points = queue_by_pid[pid]
    ticks = [x for x, _ in points]
    queues = [q for _, q in points]

    if pid in completion_ticks and ticks[-1] < completion_ticks[pid]:
        ticks.append(completion_ticks[pid])
        queues.append(queues[-1])

    ax.step(
        ticks,
        queues,
        where="post",
        marker="o",
        markersize=3,
        label=f"PID {pid} ({process_names[pid]})",
    )

# Actual priority boosts observed during the experiment.
for boost_tick in [49, 101]:
    ax.axvline(boost_tick, linestyle="--", linewidth=1)
    ax.text(
        boost_tick + 0.8,
        3.15,
        f"Priority boost\n(tick {boost_tick})",
        va="top",
    )

for pid, finish_tick in completion_ticks.items():
    queue = queue_by_pid[pid][-1][1]
    ax.annotate(
        f"PID {pid} finished\n(tick {finish_tick})",
        xy=(finish_tick, queue),
        xytext=(finish_tick + 2, queue - 0.35),
        arrowprops=dict(arrowstyle="->"),
        fontsize=9,
    )

ax.set_title("Experimental MLFQ Execution Trace\nxv6 schedulertest")
ax.set_xlabel("Scheduler Tick")
ax.set_ylabel("Queue")
ax.set_yticks([0, 1, 2, 3])
ax.set_yticklabels([
    "Q0 (highest)",
    "Q1",
    "Q2",
    "Q3 (lowest)",
])
ax.set_xlim(12, 137)
ax.set_ylim(3.4, -0.4)
ax.grid(True, alpha=0.3)
ax.legend()

plt.text(
    0.95, 0.95, "vinay.das",
    ha='right', va='top',
    transform=plt.gca().transAxes,
    fontsize=10, color="gray", alpha=0.7
)

plt.tight_layout()
plt.savefig("MLFQTrace.png", dpi=200)
plt.close()


# ============================================================
# GRAPH 2: SCHEDULER PERFORMANCE COMPARISON
# ============================================================

schedulers = ["FIFO", "Round Robin", "MLFQ"]

response = np.array([6.50, 1.50, 1.50])
turnaround = np.array([14.75, 20.50, 17.25])
waiting = np.array([6.50, 12.50, 9.25])

x = np.arange(len(schedulers))
width = 0.25

fig, ax = plt.subplots(figsize=(11, 7))

bars_response = ax.bar(
    x - width, response, width, label="Average Response Time"
)
bars_turnaround = ax.bar(
    x, turnaround, width, label="Average Turnaround Time"
)
bars_waiting = ax.bar(
    x + width, waiting, width, label="Average Waiting Time"
)

def add_labels(bars):
    for bar in bars:
        height = bar.get_height()
        ax.annotate(
            f"{height:.2f}",
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

add_labels(bars_response)
add_labels(bars_turnaround)
add_labels(bars_waiting)

ax.set_xticks(x)
ax.set_xticklabels(schedulers, fontsize=11)
ax.set_ylabel("Average Time (ticks)", fontsize=12)
ax.set_xlabel("Scheduling Policy", fontsize=12)
ax.set_title("Scheduler Performance Comparison", fontsize=14, pad=15)
ax.legend()
ax.grid(axis="y", linestyle=":", alpha=0.5)

plt.text(
    0.95, 0.95, "vinay.das",
    ha='right', va='top',
    transform=plt.gca().transAxes,
    fontsize=10, color="gray", alpha=0.7
)

fig.tight_layout()
plt.savefig("SchedulerComparison.png", dpi=300, bbox_inches="tight")
plt.close()

print("Generated graphs for the following:")
print("MLFQ Trace!")
print("Scheduler Comparison!")
