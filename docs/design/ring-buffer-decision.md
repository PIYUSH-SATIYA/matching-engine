# Decision: the first queue is a blocking ring buffer

The first queue implementation keeps the `not_empty` / `not_full` condition
variables, so `push` blocks while the buffer is full and `pop` blocks while it is
empty.

**Why.** The blocking version is the baseline the project needs: it makes the
kernel do the scheduling. Every empty→non-empty transition becomes a wakeup and
every full→not-full transition a context switch, which is exactly the cost the
lock-free SPSC version is measured against. A spinning or overwriting ring buffer
would have removed the mechanism the comparison is about, so the baseline would
no longer answer the question.

The costs this choice creates — 157 000–199 000 voluntary context switches in the
producer, 8–9 % engine CPU at 200 000 orders/s, and the 32 ms medians at
6.4 M orders/s — are measured and reported in the final report, Sections 6.4,
6.8 and 6.9.
