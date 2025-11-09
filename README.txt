# OS_Assignment2

Names: Tom Moore
Emails: thomoore@chapman.edu
ID: Tom: 2444464
Course: CPSC380-02
Programming Assignment 3
-----
Submitted Files:
- rw_log.c
- rw_main.c
- README.txt
-----
References:

https://man7.org/linux/man-pages/man3/errno.3.html
https://man7.org/linux/man-pages/man7/pthreads.7.html
https://pubs.opengroup.org/onlinepubs/7908799/xsh/string.h.html

----- 
How to run:
- gcc -pthread -o schedsim schedsim.c
- ./schedsim [options]
    - ./schedsim -f -i workload.csv                  # First-Come, First-Served
    - ./schedsim -s -i workload.csv                  # Shortest Job First
    - ./schedsim -p -i workload.csv                  # Priority Scheduling
    - ./schedsim -r -q 2 -i workload.csv             # Round Robin with quantum = 2
- Arguments
    -f, --fcfs            Use FCFS scheduling\n"
    -s, --sjf             Use SJF scheduling (non-preemptive)\n"
    -r, --rr              Use Round Robin scheduling\n"
    -p, --priority        Use Priority scheduling (preemptive, lower value = higher priority)\n"
    -i, --input <file>    Input CSV filename (pid,arrival,burst,priority)\n"
    -q, --quantum <n>     Time quantum for RR (required for -r)\n"

-----
Expected Output (Example for priority):

Console:
gcc -pthread -o schedsim schedsim.c
./schedsim -p -i jobs.csv
===== Schedule Results =====

Timeline (Gantt Chart):
0-1: P1  1-8: P2  8-27: P1  27-37: P4  37-55: P3  

PID     Arr     Burst   Start   Finish  Wait    Resp    Turn
P1      0       5       2       27      22      2       27
P2      1       3       5       8       4       4       7
P3      2       8       47      55      45      45      53
P4      3       6       31      37      28      28      34

Avg Wait = 24.75
Avg Resp = 19.75
Avg Turn = 30.25
Throughput = 0.0727 jobs/unit time
CPU Utilization = 100.00%