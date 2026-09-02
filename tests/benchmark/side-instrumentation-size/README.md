<!--
SPDX-FileCopyrightText: 2026 EfficiOS, Inc

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# libside instrumentation size benchmark

What instrumentation costs a program, for the same 1000 events described
once as LTTng-UST tracepoints and once as libside events. Two things are
measured, and they must not be added together:

    make
    ./measure-size          # what the linker emitted
    ./measure-resident      # what a running process holds

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
  `side_event_state_ptr` and `side_event_state`;
- a description made of pointers ends up in `.data.rel.ro`, and each of
  those pointers needs an entry in `.rela.dyn`.

`measure-size` groups all of them, so the totals are comparable.

## Note

The relocations are worth reading as their own line rather than folded
into the data. A description built out of pointers pays for each one
twice: the pointer itself in `.data.rel.ro`, and the relocation which
fills it in at load time.

## What a process holds

`measure-size` weighs address space and disk. Most of that is never
resident, and what is resident is not all paid the same way, so
`measure-resident` weighs the other thing:

- **shared** pages are faulted in and clean, backed by the page cache.
  They are paid once for the whole system, however many processes map
  them, and they are reclaimable under pressure.
- **private** pages have been dirtied, so the copy on write already
  happened. They are paid in full by every process, and they are the
  only ones which multiply by the number of processes.

A relocation is what turns the first into the second: the loader writes
the page, so it is private and dirty in every process before `main()`
runs, whether or not tracing is ever enabled. A description which holds
no address is never written, so a process which does not trace never
faults it in at all, and when a tracer does read it, it faults in clean.

    ./measure-resident                     # what one process holds
    ./measure-resident --processes 4       # and what several then share
    ./measure-resident --maps side-defs    # mapping by mapping

`--processes` matters because a clean page is accounted `Private_Clean`
while one process alone maps it, however shareable it is. Holding
several alive at once is what moves it to `Shared_Clean`, and what shows
that the private half does not move at all.

`--maps` is where the difference is legible: the mapping holding
`side_event_description` is dirtied only where the event states are,
while the one holding `.data.rel.ro` is dirtied in full.

The numbers come from `/proc/self/smaps`, read by a constructor in
`smaps-probe.c`, which is preloaded rather than linked into the
programs: those same binaries are what `measure-size` weighs, so
anything added to them would be counted as instrumentation. A
constructor in a preloaded object runs after the loader has relocated
and before the program has touched anything of its own, which is the
moment worth reading.

Programs are run through their libtool wrappers. The binary in `.libs`
links the installed LTTng-UST rather than the one just built.
