<!--
SPDX-FileCopyrightText: 2026 EfficiOS, Inc

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# libside instrumentation benchmark

What one event costs when it is taken through libside into LTTng-UST,
next to what the same event costs through an LTTng-UST tracepoint.

The event carries a single 32-bit integer, which is the smallest
payload that still has to be described, filtered and serialized.

    lttng-sessiond --daemonize
    ./run-benchmark

`bench-side` depends on libside only and does not link against the
tracer: `run-benchmark` preloads it, which is how libside
instrumentation is used. `bench-tp` links against LTTng-UST, which is
how a tracepoint is used.

## What it measures

Both programs are timed the same way. A warmup loop primes the ring
buffer pages and settles the caches and the branch predictors, then a
timed loop reports nanoseconds per event; the figure is the median of
several repetitions. The session is a snapshot session, so its
sub-buffers are overwrite buffers and nothing is written to disk while
the loop runs: what is measured is the path from the instrumentation
site to the committed record.

Four cases, per instrumentation:

| case | how |
|---|---|
| disabled | no session at all, so the event has no subscriber |
| recorded | `enable-event`, no filter |
| filter `1 == 0` | a filter which rejects without reading the payload |
| filter on the field | `v == 4000000000`, which the loop never emits, so the filter reads the field and rejects every instance |

The two filter cases are what separate the cost of *reaching* the event
from the cost of *recording* it, and `1 == 0` against a filter which
reads the field separates the interpreter's field load from the rest.

With `--field-loads`, the number of times a filter loads the payload is
varied instead, over filters which all reject every instance:

    ./run-benchmark --field-loads

`||` does not short-circuit a false left operand, so every term is
evaluated. The slope of nanoseconds against the number of loads is the
cost of one load; the intercept is what reaching the filter costs on its
own.

## Environment

| variable | default | |
|---|---|---|
| `BENCH_ITERS` | 5000000 | events in the timed loop |
| `BENCH_WARMUP` | 1000000 | events in the warmup loop |
| `BENCH_REPS` | 7 | repetitions, of which the median is reported |
| `BENCH_CPU` | 3 | CPU to pin to |
| `UST_LIB` | the build tree | tracer to preload into `bench-side` |

## Reading the numbers

Frequency scaling and other work on the machine move these figures
around, so compare the two columns of one run rather than one column
across runs.

A profile of a single case attributes the difference to the functions
which make it up:

    lttng -q create bench --snapshot
    lttng -q enable-event --userspace 'side_benchmark:u32' --filter '1 == 0'
    lttng -q start
    LD_PRELOAD=../../../src/lib/lttng-ust/.libs/liblttng-ust.so.1 \
        perf record -F 5000 -- taskset -c 3 ./bench-side
    perf report --stdio --sort symbol

## Note

The cost of a filter fetching fields, and the cost of recording to the
ring buffer, are not optimized: the side bytecode specializer leaves
field loads on the generic dynamic `LOAD_FIELD`, which the interpreter
resolves at run time from the self-describing SIDE payload ABI, and
serialization walks the description with a two-pass visitor. Both are
deliberate.
