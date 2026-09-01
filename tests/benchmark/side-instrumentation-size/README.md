<!--
SPDX-FileCopyrightText: 2026 EfficiOS, Inc

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# libside instrumentation size benchmark

What instrumentation costs a program in code and in data, for the same
1000 events described once as LTTng-UST tracepoints and once as libside
events.

    make
    ./measure-size

Every event has between one and eight fields, and no two events have the
same shape: the number of fields and the types they use both walk
through their cycles, so nothing the compiler sees is a duplicate of
something it has already seen. `gen-instrumentation` writes them; set
`NR_EVENTS` in the makefile to build a different number.

The programs contain nothing else. Their `main()` either returns
immediately or does nothing but the call sites, so every section is
attributable to the instrumentation.

## The six programs

|  | probe provider | call sites |
|---|---|---|
| `tp-defs` | no | no |
| `tp-defs-probes` | yes | no |
| `tp-calls` | no | yes |
| `tp-calls-probes` | yes | yes |
| `side-defs` | — | no |
| `side-calls` | — | yes |

A tracepoint needs its probe provider somewhere; compiling it into the
program is the common case, and building without it (with
`LTTNG_UST_TRACEPOINT_PROBE_DYNAMIC_LINKAGE`, which leaves the provider
to be loaded separately) is what tells the cost of the probes apart from
the cost of the definitions and of the call sites.

libside has no equivalent to attribute: the description of an event is
all there is, and the tracer which reads it is loaded separately.

## What is counted

Only the sections the loader maps. Debug information is left out, since
it costs a file on disk rather than a program in memory, and how much of
it there is says more about the compiler flags than about the
instrumentation.

Reading `.text`, `.data` and `.bss` alone would miss most of what a
description weighs, because both projects keep theirs elsewhere:

- the tracepoints of a provider live in `lttng_ust_tracepoints`,
  `lttng_ust_tracepoints_ptrs` and `lttng_ust_tracepoints_strings`;
- the descriptions of side events live in `side_event_description`,
  `side_event_description_ptr` and `side_event_state`;
- a description made of pointers ends up in `.data.rel.ro`, and each of
  those pointers needs an entry in `.rela.dyn`.

`measure-size` groups all of them, so the totals are comparable.

## Note

The relocations are worth reading as their own line rather than folded
into the data. A description built out of pointers pays for each one
twice: the pointer itself in `.data.rel.ro`, and the relocation which
fills it in at load time.
