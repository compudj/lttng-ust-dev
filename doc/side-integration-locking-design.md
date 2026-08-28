# libside / lttng-ust integration: fork handling and lock ordering design

Status: DRAFT (adversarially reviewed)
Date: 2026-08-27
Scope: locking architecture for integrating libside instrumentation into
lttng-ust (branch `side-poc`), covering (1) fork handling and (2) global
lock ordering between libside and lttng-ust. Companion trees:
`lttng-ust` and `libside` (side.c line references at libside master
`7f095a2`).

---

## 1. Background

### 1.1 lttng-ust locking (today)

- `ust_mutex` (`ust_lock()`/`ust_unlock()`, lttng-ust-comm.c): central
  mutex for tracing control and probe registration. Held by the ust
  listener threads while handling sessiond commands. Nests within the
  glibc dynamic loader lock (taken from library constructors).
  Per-thread `ust_mutex_nest` allows same-thread recursion (statedump
  tracing, perf lazy init on the listener).
- `ust_fork_mutex`, `ust_exit_mutex`: never nest inside `ust_mutex`.
  `ust_exit_mutex` protects listener thread creation/join/exit
  (comm.c:2643-2680, 2787-2816); `ust_fork_mutex` serializes fork
  against statedump handling (comm.c:926-928) and other forks.
- fd tracker lock: nests inside `ust_mutex`; also taken by arbitrary
  application threads through the liblttng-ust-fd `close()` wrapper
  (lttng-ust-fd.c:122 → fd-tracker.c:364,383).
- `ust_perf_mutex` (perf-counters.c:79): taken lazily on first perf
  context use per thread, from the tracing fast path
  (perf-counters.c:389 via `perf_counter_record`).
- lttng-ust urcu: `rcu_gp_lock` + `rcu_registry_lock`
  (lttng-ust-urcu.c:109,119). First-use read-side registration of a
  thread takes `rcu_registry_lock` (urcu-ust.h:148-153 → urcu.c:534).
- Fork today: LD_PRELOAD `liblttng-ust-fork.so` (ustfork.c). Its
  `fork()` wrapper calls `lttng_ust_before_fork()` (comm.c:2865):
  block all signals → `ust_fork_mutex` → `ust_lock_nocheck()` →
  `lttng_ust_urcu_before_fork()` (takes BOTH rcu locks, urcu.c:668-669)
  → fd tracker lock → `lttng_perf_lock()`; all held across `fork()`;
  released by `lttng_ust_after_fork_parent()/child()`.

### 1.2 libside locking (today)

- `side_event_lock` (recursive, side.c:118): protects
  `side_events_list`, `side_tracer_list`, per-event callback arrays.
  Held while invoking tracer notification callbacks
  (side.c:461-467, 517-532, 553-561, 573-580). Recursive so callbacks
  can call side APIs (e.g. `side_tracer_callback_register`).
  `side_rcu_wait_grace_period()` runs under it (side.c:341, 416).
- `side_statedump_lock` (recursive, side.c:119) and
  `side_agent_thread_lock` (side.c:128): statedump request queues and
  agent thread lifetime. Statedump callbacks run WITHOUT
  `side_statedump_lock` held (side.c:633-648).
- Fork today: `pthread_atfork` handlers registered by lazy
  `side_init()` (side.c:979). `side_before_fork` (side.c:919): takes
  `side_agent_thread_lock` (held across fork), sets PAUSE, busy-waits
  for the agent thread's PAUSE_ACK. Child handler recreates the agent
  thread immediately (side.c:957-971).

---

## 2. Problems

### P1 — fork deadlock through the statedump agent thread [CONFIRMED]

With both libraries active, fork thread F (via the ustfork wrapper)
holds `ust_mutex`, both urcu locks, the fd tracker lock and
`ust_perf_mutex` when glibc's `fork()` runs `side_before_fork`, which
busy-waits for the agent's PAUSE_ACK. The agent only ACKs at the top of
its loop, never mid-statedump (side.c:660-686). An agent mid-statedump
tracing through lttng-ust probes can block on:

- (a) first-use urcu thread registration → `rcu_registry_lock`
  (held by F);
- (b) perf context lazy per-thread init → `ust_perf_mutex` (held by F);
- (c) a future lttng side-statedump callback taking `ust_lock`
  (the pattern exists: lttng-ust-statedump.c:367,530,618,625).

The per-thread nest counters do not help: they are per-thread, and the
agent is not F. Result: F spins on PAUSE_ACK with all signals blocked;
process frozen. Note: with the printf-only POC callbacks this is not
yet reachable; it becomes real exactly when ring-buffer/context
integration lands.

This is a *progress-dependency cycle through a condition wait*, not a
lock-order cycle: it is invisible to lock-ordering (lockdep-style)
analysis. The lock-order component of today's fork path is actually
consistent (all side fork locks nest inside all ust locks).

### P2 — ABBA between `side_event_lock` and `ust_mutex` [CONFIRMED]

- App thread: `side_events_register()`/`side_events_unregister()` hold
  `side_event_lock` → notification callback → must take `ust_mutex`
  (create/tear down lttng events). This edge is FORCED: REMOVE_EVENTS
  is synchronous because event descriptors are static data in a DSO
  about to be dlclosed (side.c:502-535); deferred teardown cannot keep
  that memory alive.
- Listener thread: command handling under `ust_lock` → enabler sync →
  `side_tracer_callback_register()` → `side_event_lock`.

Classic ABBA; the recursive mutex does not help across threads.
Alternatives reviewed: deferred teardown / generation counters cannot
satisfy the synchronous-REMOVE contract (descriptors are static data in
a DSO about to be dlclosed); exporting the lock and imposing a global
"side outer" order works but couples the control plane to app
registration and freezes big-lock semantics into ABI. RESOLUTION
(chosen): split the lock — the forced edge only constrains the lock
held across notification delivery; giving the per-event callback
arrays their own leaf lock removes the listener's need for any outer
side lock entirely. See 3.2.

---

## 3. Design

### 3.1 Fork: pthread_atfork with state-based quiescence

Replace the preload-wrapper fork protocol and `ust_fork_mutex` with
phased-atfork coordination (3.1.1): a neutral coordination library
owns the single `pthread_atfork` registration and stages all
participants around *quiescence before locking*:

> INVARIANT F1: process-wide, every quiescence ACK-wait completes
> before the first trace-path-blocking lock is acquired in the fork
> prepare path.

> CONTRACT (semantics of the whole fork machinery): fork() behaves as
> if the tracing runtime was not loaded. Runtime-added service
> threads (listeners, statedump agent) are quiesced before fork and
> recreated after, so a process single-threaded by its own accounting
> gets single-threaded fork semantics — the child may continue traced
> without exec. Locks covertly taken within application threads
> (tracepoint first-use urcu registration, perf lazy init, fd-tracker
> interposition, ctor probe registration) are owned across fork so
> hidden critical sections never straddle it. A process forking amid
> its own thread activity gets stock POSIX fork semantics: tracer
> state stays consistent in the child, but child viability is no
> worse than untraced and not guaranteed better.

Requires glibc ≥ 2.24 (prepare handlers may wait on threads that
malloc; glibc commit 8a727af9, cf. side.c:912-917). This is the same
floor libside already assumes.

#### 3.1.1 Phased-atfork: a neutral fork coordination library

DECIDED (2026-08-27; supersedes both the tracer-driven cooperative
`side_fork_prepare/parent/child` API and a libside-owned
coordinator): fork coordination is a process-wide singleton concern
and belongs to NEITHER a tracer NOR an instrumentation layer. A
tracer-owned sequence breaks with multiple tracers; an
instrumentation-layer-owned sequence breaks with multiple
instrumentation layers (a tracer consumes several instrumentation
sources — lttng tracepoints, side events, user-events — and two
layers claiming the coordinator role recreate the uncoordinated
ordering problem one level up).

The need is "phased pthread_atfork": glibc's atfork is one flat LIFO
prepare list, unable to express "all quiescence before any lock
acquisition" or lock-order levels. A tiny neutral, dependency-free
library (shipped in the side dist alongside librcu/libsmp; the
`fork_*` prefix below is a naming placeholder) registers
pthread_atfork ONCE and dispatches phases:

```
struct fork_participant_ops {
	void (*quiesce)(void *priv); /* park own threads; ACK-waits OK;
	                                must not need other participants'
	                                locks */
	void (*acquire)(void *priv); /* take own locks; no waits on
	                                thread progress */
	void (*parent)(void *priv);  /* release own locks, resume own
	                                threads */
	void (*child)(void *priv);   /* reinit own state */
};
handle = fork_participant_register(ops, priv, level);
void fork_participant_unregister(handle);
/* Whole-sequence entry points, nest-counted per owning thread
   (old-preload wrapper compat; the atfork-fired dispatch no-ops on
   the nest count; concurrent forkers serialize on ownership): */
void fork_phases_prepare(void);
void fork_phases_parent(void);
void fork_phases_child(void);
```

Dispatch:
- prepare: block all signals (mask in TLS) → ALL quiesce callbacks
  (order immaterial by contract) → acquire callbacks in ASCENDING
  level → fork().
- parent: parent callbacks in DESCENDING level → restore signals.
- child: child callbacks in ASCENDING level → restore signals.

Why quiesce is a global stage rather than per-participant
quiesce+acquire: the shared side RCU read-side couples otherwise
independent participants. Counterexample with tracer-driven phases:
T1 quiesces and takes its locks (e.g. `ust_perf_mutex`); T2 then
parks its listener, which sits mid-command in
`side_rcu_wait_grace_period` under T2's control lock; the GP never
completes because an app thread inside a side read-side section is
blocked on T1's perf lock (lazy init in tracer_call); T2's park ACK
never arrives. Staging ALL quiescence before ANY lock acquisition
(F1, globally) is the only fix.

Level assignment mirrors L1 (a library may register several
participants at different levels — intended usage):

- level 0 — instrumentation outer (libside):
  quiesce = agent pause (PAUSE/PAUSE_ACK protocol; replaces today's
  static atfork handlers with their held-across-fork
  `side_agent_thread_lock` and asymmetric early-return unlock,
  side.c:972+ at current HEAD);
  acquire = `side_notification_lock`;
  parent = release notif lock, resume agent (descending order makes
  this run last);
  child = repair (see 3.1.3; ascending order makes this run first).
- level 1 — tracers (lttng-ust):
  quiesce = park listener threads (atfork-migration track; empty in
  the wrapper era, where `ust_mutex` acquisition provides the
  exclusion per 3.1.6);
  acquire = `ust_fork_mutex` → `ust_lock_nocheck()` → urcu
  gp+registry → fd tracker → `lttng_perf_lock()`;
  parent = release in reverse, resume listeners;
  child = lttng child reinit — may call side APIs, since level 0
  already repaired them.
- level 2 — instrumentation leaves (libside):
  acquire = `side_event_lock` → `side_statedump_lock` →
  `side_key_lock`; parent = release.
- level 3 — service-thread restart (libside):
  child = recreate the agent thread (if referenced) — ascending
  order makes this run after every tracer's child reinit.

Participant contracts: `quiesce` may wait for its own threads but
must not acquire or depend on anything another participant's
quiescence — or a side read-side section — can hold or need;
`acquire` performs lock acquisitions only (no ACK-waits, no grace
periods); `child` runs single-threaded with lower levels already
repaired. Participants must remain loaded for process lifetime
(NODELETE). Registration happens at participant init;
unregistration serializes against an in-flight fork sequence via the
coordinator's state lock.

A second tracer registers its own level-1 participant with no
knowledge of lttng; a second instrumentation layer registers its own
outer/leaf/restart participants at the same levels. One tracer
registration covers ALL of that tracer's instrumentation sources.

Singleton caveat: the coordinator's value depends on being a
process-wide singleton (one shared soname). Two vendored copies in
one process recreate the ordering problem between coordinators.
Long-term this is glibc-wishlist material (phased atfork); the
library is the userspace proof of concept.

#### 3.1.2 Listener quiescence (new lttng-ust state machine)

IMPLEMENTED (side-poc 8ca38f94) as the level-1 quiesce callback:
per-listener fd-tracked wakeup pipe + poll() before the command
header receive; pause-aware wait_shm futex loop (futex wakeup, other
processes' listeners tolerate the spurious wake); interruptible
reconnect delay; refcounted pause with listener
registration/unregistration; cancellation-disabled parked state; exit
wakes parked listeners before cancelling; child-side state reset with
inherited pipes closed for isolation (a shared open file description
could lose wakeups across processes). Deviations from the sketch
below: mid-command payload receive is WAITED OUT (bounded by the
sessiond socket timeouts) rather than interrupted, and connect()
still runs under the fd tracker lock — both bounded by the same
timeouts that already bound the fork acquire stage.

ust_fork_mutex RETIRED (side-poc d6881db7): dl-lock-vs-fork
exclusion subsumed by parking (no statedump can straddle a fork
sequence); fork-vs-fork serialization by coordinator ownership;
dl_state_table protection was always ust_mutex (proven by
lttng_ust_dl_update() being callable from app dl instrumentation
without ust_fork_mutex). H5 FIXED (side-poc 77395ed6): the
get_wait_shm() helper uses _Fork() (glibc >= 2.34,
configure-detected), running no atfork handlers and bypassing fork()
interposers.

The traced application runs at most TWO concurrent listener threads
out of three `sock_info` instances: `global` + `local` normally, or the
single `ust_app` listener when `LTTNG_UST_APP_PATH` is set (which
disables global/local; comm.c:611-614, 643-676, 2643-2680). All three
are in-process in the traced application — `ust_app` is the
alternate-path sessiond connection, NOT the lttng-ust-ctl/consumerd
side — and all are covered by the same quiescence machinery.
(lttng-ust-ctl users such as sessiond/consumerd are out of scope for
this design: the ring-buffer timer/RT-signal threads exist only there,
never in traced apps.) A pause request parks each listener at the top
of its command loop, holding no locks, ACKs, and waits on a cond for
resume.

Blocking points that must become interruptible / bounded:

- command-header `recvmsg` (comm.c:2377): restructure as `poll()` on
  {command socket, wakeup pipe/eventfd}.
- mid-command payload/fd `recvmsg` (comm.c:2406 → 2082; EINTR-looped in
  ustcomm.c:319-360): today a stalled sessiond here does NOT block fork
  (no lock held); under park-based quiescence it would block fork
  forever. These reads get the same poll/timeout treatment. A parked
  request arriving mid-command is honored at the next loop top, and the
  ACK-wait is bounded by the (now bounded) command I/O.
- futex wait on the wait_shm page (comm.c:1961-1992): FUTEX_WAKE alone
  is insufficient — the loop re-sleeps while the shm word is unchanged.
  Add an explicit pause-flag check in the loop condition. (The wake
  word is a shared mapping: a wake also wakes other traced processes'
  listeners; harmless.)
- reconnect-path `sleep(5)` (comm.c:2150): replace with an
  interruptible wait (same wakeup pipe), else park latency is up to 5s.
- `connect()` currently runs while HOLDING the fd tracker lock
  (comm.c:2189-2220, 2273-2305) and is unbounded with
  `LTTNG_UST_REGISTER_TIMEOUT=-1`: connect must move outside the
  quiescence-blocking window or get a dedicated bound.

State machine bits (modeled on `statedump_agent_thread.state`):
PAUSE / PAUSE_ACK / RESUME / EXIT, refcounted pause requests
(two threads may fork concurrently; atfork provides no serialization —
this subsumes one role of `ust_fork_mutex`). Exit integration: the
pause path must consult `thread_active`/`lttng_ust_comm_should_quit`
(under `ust_exit_mutex`) so a fork during/after `lttng_ust_exit` does
not ACK-wait on cancelled/exited listeners, and the parked state runs
with cancellation disabled (cf. `lttng_ust_cancelstate_disable_push`,
comm.c:172). The statedump-vs-fork exclusion previously provided by
`ust_fork_mutex` (comm.c:926-928) is preserved by (a) the park point
being outside `handle_pending_statedump` and (b) a pending pause
inhibiting the start of a new statedump.

#### 3.1.3 Sequence walk-through (libside + lttng-ust)

The coordinator dispatch of 3.1.1 produces the following concrete
order with both libraries' participants registered:

Prepare:

1. Block all signals; save mask in TLS (coordinator).
2. Quiesce stage (all ACK-waits, NO locks held anywhere): agent pause
   (level 0) — the agent may finish in-flight statedump tracing
   freely, since no trace-path lock is held yet; listener park
   (level 1, migration track only).
3. Acquire stage, ascending level:
   a. level 0: `side_notification_lock` — gates new notification
      deliveries and waits out in-flight ones (they only need
      `ust_mutex`, which nobody holds at this point); closes H2 for
      the notification state (no dead-thread-owned lock in the
      child).
   b. level 1: `ust_mutex` (`ust_lock_nocheck`), urcu gp + registry
      locks (`lttng_ust_urcu_before_fork`), fd tracker lock,
      `lttng_perf_lock()` (preceded by `ust_fork_mutex` until it is
      retired).
   c. level 2: side leaf locks (`side_event_lock`,
      `side_statedump_lock`, `side_key_lock`) — after `ust_mutex`
      per L1. Statedump/key locks are takable by non-quiesced app
      threads (statedump provider registration, key requests) so
      they must be owned across fork; `side_event_lock` is provably
      unheld once quiescence + `side_notification_lock` hold (its
      takers are the parked listener under `ust_mutex` and
      delivery-context calls under `side_notification_lock`), but
      owning it keeps the invariant robust.
4. `fork()` runs; any other atfork prepare handlers follow.

Parent (descending level): release side leaves (level 2); release ust
locks in reverse and resume listeners (level 1); release
`side_notification_lock` and resume the agent (level 0); restore
sigmask.

Child (ascending level):

- level 0 repair: RESET the side RCU grace-period state
  (`event_rcu_gp`, `statedump_rcu_gp` per-CPU reader counts, plus
  their gp_lock/futex reinit — note the statedump GP wait at
  side.c:830 runs under NO lock, so its gp_lock can be dead-owned):
  a parent thread inside a side read-side section at fork time
  otherwise leaves `begin != end` in the child forever, hanging
  every child-side `side_rcu_wait_grace_period()` — including the
  child's own `side_tracer_event_notification_unregister` path.
  Resetting is safe because fork from within a side read-side
  section / tracer callback is forbidden by contract (section 5).
  Also: reinit all side mutexes;
  `statedump_agent_thread_fini()/init()` (no thread creation yet).
- level 1: lttng-ust child-side reinit (urcu, fd tracker, perf, comm
  state, listener restart / re-registration) — may call side APIs,
  since level 0 already repaired them.
- level 3: recreate the agent thread last; restore sigmask
  (coordinator).

#### 3.1.4 Backward compatibility with deployed liblttng-ust-fork.so

An old preload wrapper calls `lttng_ust_before_fork()` explicitly, then
`fork()` fires the new atfork prepare in the same thread. Double
execution does NOT self-deadlock on `ust_mutex` (nest-counted per
thread, comm.c:110,212-213) but WOULD on the plain urcu mutexes
(urcu.c:109,119 taken at 668-669) and would double-ACK-wait. Fixes, all
in new liblttng-ust (the old .so cannot be changed):

- Reimplement `lttng_ust_before_fork()`/`after_fork_*()` (exported,
  signatures unchanged) as thin wrappers over the NEW
  quiesce-then-lock sequence — required for correctness, not just
  anti-recursion: keeping the old body would retain P1 verbatim under
  an old preload.
- A TLS "fork handling armed by this thread" flag makes whichever of
  {explicit call, atfork prepare} runs second a no-op, symmetrically in
  parent/child (the skipped side must not double-decrement the
  coordinator nest count). Note `lttng_ust_nest_count` guards a
  different case (the
  internal `get_wait_shm` fork, comm.c:1782-1784; ustfork.c never
  touches it) — the atfork prepare checks both.
- Runtime `dlsym` detection of the coordinator (`fork_phases_prepare`
  / `fork_participant_register`) so new lttng-ust degrades gracefully
  against an old libside dist lacking it (falling back to
  documented old-libside limitations).

#### 3.1.5 Coverage, link flags, internal forks

- `pthread_atfork` covers `fork()` and everything routed through libc
  fork — including `daemon()` — making fork correctness independent of
  LD_PRELOAD for the common case. Today, forking without the preload
  while a listener is mid-command hands the child a locked `ust_mutex`
  and torn session state; the atfork design fixes that by default.
- The preload shim REMAINS REQUIRED (not optional) for: `clone()`
  (glibc clone never runs atfork handlers; ustfork.c:337-379),
  `rfork()`, and the `setns`/`unshare`/`set*uid`/`set*gid` re-
  registration and namespace/credential context-cache resets
  (ustfork.c:217-449). Its `fork()`/`daemon()` wrappers were DROPPED
  (side-poc f0a83385): both route through libc fork, which fires the
  coordinator's atfork handlers. Pairing caveat: the slimmed preload
  requires a liblttng-ust registering the phased-atfork participant
  (2341f53c); with an older liblttng-ust, plain fork() gets no
  tracing fork handling. clone() keeps its wrapper because the child
  starts in a user-supplied function on a caller-provided stack —
  only an interposer trampoline can run child-side fork handling.
- liblttng-ust is NOT currently NODELETE at link time; only a fallible
  runtime self-`dlopen(RTLD_NODELETE)` exists (comm.c:2559-2564), and
  glibc auto-unregisters a dlclosed DSO's atfork handlers (silent loss
  of fork handling, not a crash). Since this design makes fork
  correctness depend on the handlers, add `-Wl,-z,nodelete` to both
  liblttng-ust and libside link flags.
- The internal `get_wait_shm()` helper fork (comm.c:1783) runs foreign
  atfork handlers while holding the fd tracker lock (taken at
  comm.c:1902-1920), pausing the side agent on every ctor-time fork and
  creating a deadlock once the lttng side-statedump callback performs
  fd-tracker-locked I/O; the transient child also pointlessly
  recreates an agent thread before `_exit`. Switch this helper to
  `_Fork()` (glibc ≥ 2.34) or a raw clone/fork syscall, bypassing
  atfork handlers entirely.

#### 3.1.6 Minimal integration variant (wrapper retained)

Consequence of the 3.2 lock split: the listener's `ust_lock` fork
handling needs NO changes for libside integration. The listener only
touches side state (leaf locks) while holding `ust_mutex`, so the
existing wrapper protocol — F owning `ust_mutex` across fork —
already excludes listener-held side and ust locks from the child. The
notification-callback path is likewise excluded (`ust_mutex` taken
before the leaf). The integration-required fork changes reduce to the
`lttng_ust_before_fork()/after_fork_*()` sequence plus the phased-
atfork coordinator and its participant registrations (3.1.1):

1. Both libraries register their fork participants (3.1.1) at init;
   in the wrapper era lttng's level-1 `quiesce` is empty — the
   `ust_mutex` acquisition in `acquire` provides the listener
   exclusion analyzed above.
2. `lttng_ust_before_fork()` reduces to `fork_phases_prepare()` (plus
   the compat armed flag, 3.1.4); `after_fork_parent/child` reduce to
   `fork_phases_parent()/child()`. The dispatch order (3.1.3) kills
   P1 (agent paused before any trace-path lock) and closes H2
   (`side_notification_lock` and the leaves owned across fork; child
   repair for the rest).
3. The atfork-registered dispatch fires inside `fork()` and no-ops on
   the nest count when the wrapper already drove the sequence.

The listener park/quiescence machinery (3.1.2) and the atfork
migration retiring `ust_fork_mutex` and the preload are needed ONLY
for the fork-correct-without-LD_PRELOAD goal — an independent track
that can land before or after the integration. H5 (`get_wait_shm`
helper fork → `_Fork()`/raw syscall) applies to both variants.

#### 3.1.7 Follow-on track: async-signal-safe child reinit
(retiring the acquire stage)

Status: DESIGN ONLY — the acquire stage stays until this lands.

Derivation. With listener parking and agent pause in place, the
justification for the acquire stage (owning `ust_mutex`, the urcu
locks, the fd tracker lock and the perf lock across fork()) narrows
to a single load-bearing role. For a process single-threaded by its
own accounting, quiescence alone leaves every tracer lock provably
free at fork(): the forking thread is the only thread left and is
not inside a tracepoint or interposed call while calling fork(). The
scenarios where the acquire stage matters all require an application
thread racing the fork — i.e. an application which IS multithreaded
by its own accounting, where POSIX already restricts the child to
async-signal-safe operations until exec. The one party those
restrictions do not excuse is the tracer itself: OUR atfork child
callback runs in every child, before any exec, and currently
performs non-async-signal-safe cleanup on real data structures.
Owning the locks across fork is today's cheap way to keep our own
child callback from hanging a child which, untraced, would have
exec'd fine ("no worse than untraced").

The consistent endpoint of the contract (3.1, CONTRACT block) is to
remove that dependency instead of feeding it:

> The atfork child callback performs ONLY async-signal-safe
> reinitialization: plain stores which reset locks and
> synchronization state. It walks no data structure, allocates
> nothing, and frees nothing. Real reconstruction and cleanup are
> deferred to the explicit continue path
> (lttng_ust_after_fork_child() -> constructor rerun) or to lazy
> first use. The acquire stage is then retired entirely: fork()
> performs no tracer lock acquisition at all.

Case analysis:
- Single-threaded-by-own-accounting app: quiescence guarantees
  nothing is torn, so reinit-by-stores is equivalent to today's
  unlock path, and the continue path can safely perform the deferred
  cleanup/reconstruction (nothing was mid-mutation). Behavior
  unchanged.
- Multithreaded app: the child callback performs only stores and
  gets out of the way; the child execs exactly as it would untraced.
  Torn tracer data is ABANDONED, not cleaned: leaks are irrelevant
  in a child which execs (tracker-registered fds are CLOEXEC), and a
  continue-child in this case was never promised working tracing.

Per-structure reinit inventory (lttng-ust):
- `ust_mutex` + `ust_mutex_nest` TLS: reset by assignment; the
  forking thread's nest count is 0 by contract (fork() is not called
  under ust_lock).
- lttng_ust_urcu: `rcu_gp_lock`/`rcu_registry_lock` reset; gp
  counter/futex state reset (side_rcu_gp_after_fork_child
  equivalent); the registry LIST may be torn by a dead thread's
  first-use registration — reset the list head and re-register the
  surviving thread lazily (its registration node lives in its own
  TLS) rather than walking the inherited list.
- fd tracker: mutex + nest TLS reset; the fd table may be torn by a
  dead thread's close() interposition — reset to empty and abandon
  entries (leak-tolerant; fds are CLOEXEC); Case A deferred cleanup
  happens on the continue path where the table is known-consistent.
- perf: `ust_perf_mutex` + nest reset; dead threads' per-thread
  counter state abandoned; lazy rebuild on first use.
- Listener park state: already reset by assignment (established
  pattern), including wakeup pipe closing — the pipe close() calls
  are async-signal-safe.
- comm state (sock_info sockets, handle tables, sessions): nothing
  touched in the child callback; the continue path performs today's
  lttng_ust_cleanup() + constructor rerun.

Cross-library extension: the same track applies to libside — its
child repair is already store-based, but statedump agent recreation
(level-3 child callback: pthread_create) is not async-signal-safe
and is equally unjustified in a Case B child; it should likewise
move to the continue path / lazy first use.

CAVEAT — locking validators. Reinitializing a possibly-locked mutex
by assignment (or pthread_mutex_init on it) is formally undefined
per POSIX and is hostile to lock validators and checkers:
helgrind/DRD model lock identity and state, and will either report
errors (reinit/destroy of a held lock) or silently lose their model
of the lock, causing false positives or missed races afterwards;
ThreadSanitizer similarly; error-checking and robust mutex types
(PTHREAD_MUTEX_ERRORCHECK, robust futexes) have owner state which
by-assignment reset bypasses, defeating dead-owner detection; any
future lockdep-style instrumentation would need explicit
reinitialization annotations. Mitigation: funnel every child-side
reset through one annotated helper (candidate: exported by
libphased-atfork for all participants, e.g.
phased_atfork_mutex_reinit_child()), carrying the validator client
requests/annotations (Valgrind ANNOTATE_*/helgrind client requests,
__tsan_mutex_* annotations) and documenting child-after-fork as the
single sanctioned use. Note this caveat is not introduced by this
track: it already applies to the existing store-based reinit sites
(libside child repair, listener park state, coordinator state); the
shared helper would cover those uniformly.

Sequencing: land the inventory piece by piece behind the existing
acquire stage (which masks mistakes), convert the child callback,
then retire the acquire stage as its own commit. Test matrix: Case A
continue-traced child; Case B fork+exec under load with racing
tracepoint/close() threads; validator runs (helgrind, TSan) over the
fork paths with and without the annotations helper.

### 3.2 Lock ordering: split `side_event_lock`; `ust_mutex` stays the
control-plane lock

DECIDED (stage 1): libside splits `side_event_lock` into two locks
with distinct roles; nothing is exported.

- `side_notification_lock` (recursive): protects `side_events_list` and
  `side_tracer_list`; held across INSERT/REMOVE notification delivery.
  All four notification paths keep their current structure — the
  exactly-once, synchronous-REMOVE, drain-on-tracer-unregister,
  ordering and reentrancy guarantees are unchanged by construction.
- `side_event_lock` (name retained; demoted from recursive big lock to
  a plain non-recursive leaf with reduced scope): protects the
  per-event callback arrays (`es0->callbacks`, `nr_callbacks`,
  `enabled`), including the RCU grace-period waits in
  `side_tracer_callback_register/unregister` and
  `side_event_remove_callbacks`.

> INVARIANT L1 (partial order):
> dl loader lock → `side_notification_lock` → `ust_mutex` → {fd tracker,
> urcu locks, perf lock, `side_event_lock`,
> `side_statedump_lock`, `side_key_lock`, ...existing ust nesting};
> plus the direct edge `side_notification_lock` → `side_event_lock`
> (`side_events_unregister` → `side_event_remove_callbacks`).
> Acyclic.

- The listener under `ust_lock` uses only leaf-safe side APIs:
  `side_tracer_callback_register/unregister`,
  `side_tracer_statedump_request`, `side_tracer_request_key`. It never
  takes `side_notification_lock`. The one rule frozen forever: NOTHING may
  take `side_notification_lock` while holding `ust_mutex` (or any other
  tracer's control lock).
- Notification callbacks (INSERT/REMOVE) run under `side_notification_lock`
  and take `ust_lock` inside — the forced edge, consistent with L1.
- Descriptor lifetime: the REMOVE callback must acquire `ust_mutex`
  and purge lttng's bookkeeping before `side_events_unregister`
  returns — hence before dlclose can free the DSO. Therefore, while
  holding `ust_mutex`, every side descriptor in lttng's bookkeeping is
  valid: `ust_mutex` is the descriptor validity domain, mirroring the
  existing `lttng_ust_probe_register` model. The listener needs no
  side lock for validity.
- `side_tracer_event_notification_register/unregister` take
  `side_notification_lock` and must only be called with no ust lock held —
  ctor/dtor context. Verified for init: `lttng_ust_side_tracer_init()`
  at comm.c:2586 runs with no ust lock (comm.c:2515-2586 takes none),
  under the dl lock — consistent with L1. `lttng_ust_side_tracer_exit`
  (comm.c:2755) must preserve the same property.
- Converse dl rule: NOTHING may take the dynamic loader lock while
  holding `side_notification_lock` — no dlopen/dlclose/dladdr (or lazy-binding
  PLT resolution of never-called symbols; consider `-z now` for the
  callback paths) from tracer notification callbacks.

Latency consequences (accepted, to be documented):

- The control plane is decoupled from app registration: sessiond
  commands no longer serialize behind dlopen registration bursts (the
  win over the rejected exported-lock design). Registrations still
  serialize among themselves on `side_notification_lock` — status quo, not a
  regression.
- GP waits in callback (un)register now run under `ust_mutex` +
  `side_event_lock`: command latency includes side grace
  periods, and N events × M keys costs N×M GPs — batching (one GP per
  batch) is a desirable libside optimization.
- With `LTTNG_UST_ALLOW_BLOCKING` (comm.c:790-801) and a stalled
  consumer, a blocked ring-buffer write inside a side read-side
  section stalls the GP → stalls command handling and fork prepare
  (via `side_event_lock`), though no longer dlopen ctors.
  Document: `LTTNG_UST_ALLOW_BLOCKING` extends its existing "may block
  tracing" contract to control-plane/fork paths.
- Multi-tracer: tracers sharing the side RCU read-side are
  liveness-coupled in normal operation — one tracer's grace periods
  can be extended by readers blocked on another tracer's locks (e.g.
  perf lazy init during the other tracer's long command). No fork or
  lock design removes this; it is a documented property of a shared
  read-side.

Stage 2 (future option, purely internal, zero ABI impact — record
only): fully decoupled delivery if notification delivery must one day
stop serializing registration paths. Shape: per-(tracer, handle) pair
records with PENDING_INSERT/INSERTED/PENDING_REMOVE states; a linkage
mutex never held across callbacks linearizing list membership and pair
creation (exactly-once by construction); per-tracer FIFO queues with a
delivery owner and per-item completion waits; REMOVE reuses the pair
record so unregister paths never allocate; tracer-unregister drains
its full queue before returning. Not needed for the initial
integration.

### 3.3 Statedump execution context

lttng-triggered side statedumps use the queue-only request API
(`side_tracer_statedump_request(key)`) which is safe under `ust_lock`
per L1 (takes only `side_statedump_lock`, a leaf here). lttng-ust must
NEVER wait for agent-thread progress (provider register/unregister in
`SIDE_STATEDUMP_MODE_AGENT_THREAD` waits on `waiter_cond`/joins the
agent, side.c:797,826) while holding `side_notification_lock`, any ust lock,
or from within a notification callback.

Outcome [IMPLEMENTED]: connecting the application statedump of side to
the statedump of lttng-ust turned out to be a small change, and the
reason is this section and the fork work of 3.1 rather than anything
about the statedump itself.

- The request is made from the loop which clears `statedump_pending`,
  with `ust_lock` held. That is only allowed because the queue-only
  request API takes a side leaf lock, which L1 already established.
  Waiting for the agent thread there would have deadlocked.
- The callbacks of the application run on the side agent thread, and
  emit events into ring buffers through the tracer callbacks. That is
  only safe across a fork because the agent thread is a quiesced
  participant of the phased atfork (level 0), and because the ust fork
  mutex, whose roles the parking and the coordinator ownership
  subsumed, is gone: an application statedump in flight during a fork
  no longer races the tracer.
- The state has to reach only the session which asked for it, which
  the side key mechanism already expresses: one key per session, and a
  statedump requested with a key is only delivered to the callbacks
  registered with it. Regular events are emitted with a key matching
  them all, so they keep reaching every session.

### 3.4 The optional type: unsupported everywhere [FUTURE WORK]

Side describes an optional value with `SIDE_TYPE_OPTIONAL`: a value
which is either present or absent. Nothing in the stack can express
it today, at any layer:

- LTTng-UST has no optional type: `enum lttng_ust_type` has integers,
  strings, floats, the dynamic type, enumerations, arrays, sequences,
  structures, the two blobs, and the variant added for side.
- The protocol has no optional either: `enum
  lttng_ust_ctl_abstract_types` has no such abstract type, so an
  application has no way to describe one to the session daemon.
- The session daemon has no optional field class, and neither writer
  emits one. CTF 2 does specify an optional field class, but nothing
  in lttng-tools implements it, for any domain. CTF 1.8 cannot express
  it at all.

Unlike the variant, where the abstract type and the decoding already
existed because the dynamic type uses them, there is nothing to reuse
here: supporting it means the whole stack again, plus a decision about
what CTF 1.8 does.

There is a cheaper mapping, if optionals are wanted before that work
is done: an optional is a variant of two options, present and absent,
and variants are supported end to end. Translating an optional into a
variant whose selector is the presence of the value reuses everything
and needs nothing from the protocol, at the price of describing it in
the trace as a variant rather than as the optional of CTF 2.

### 3.5 Attributes: migrating the special-cased metadata [DEFERRED]

Events, fields and types carry generic attributes: a name and a value
within a namespace, carried within the array of fields of the event
registration as `lttng_ust_ctl_atype_attribute` entries, and exported
to CTF 2 as the attributes of an event record class, of a structure
field member class, or of a field class (CTF 1.8 gets comments). Side
attribute keys are namespaced names, split at their last separator; a
key without a separator has an empty namespace.

Three pieces of metadata predate that mechanism and are described by
dedicated fields of the protocol and of the LTTng types:

| Today                                        | As an attribute                     |
| -------------------------------------------- | ----------------------------------- |
| `model_emf_uri` of the event description      | ns `lttng`, name `emf_uri`          |
| `loglevel` of the event description           | ns `lttng`, name `loglevel`         |
| `media_type` of the blob types                | ns `std.blob`, name `media-type`    |

They should migrate onto the generic mechanism, which would remove
their dedicated fields from the event registration message and from
`struct lttng_ust_ctl_type`, and remove the corresponding special
cases from the session daemon. Note that the CTF 2 output of the
session daemon currently describes the first two under the
`lttng.org,2009` namespace with the names `emf-uri` and `log-level`,
so the migration also changes what a consumer reads.

The complexity of the migration is on the CTF 1.8 side, and it is why
this is deferred rather than done. CTF 1.8 cannot express attributes:
the generic mechanism only describes them as comments, whereas the
three above are part of the format there. `loglevel` is a property of
an event, `model.emf.uri` is a property of an event, and the media
type of a blob is described within the declaration of a field. Once
they become attributes, the CTF 1.8 writer has to recognize those
three attributes, by namespace and name, and emit them as the legacy
constructs rather than as comments — which is the legacy special
casing moved rather than removed. That is a complexity worth carrying
in a released tracer, not in this proof of concept.

Deferred: the mechanism is in place and side attributes flow through
it, nothing depends on the migration, it changes metadata which
existing consumers already read, and it requires wiring the CTF 1.8
writer to recognize the migrated attributes.

### 3.6 RCU domains: the tracer must synchronize against both

lttng-ust and libside each have their own RCU implementation and their
own grace period domain. A tracepoint probe body runs inside a
lttng-ust RCU read-side critical section: `lttng_ust_tp_rcu_read_lock`
*is* `lttng_ust_urcu_read_lock` (`include/lttng/tracepoint-rcu.h`). A
side event callback instead runs inside a side read-side critical
section (`side_rcu_read_begin(&event_rcu_gp, ...)` around the callback
array iteration in `side.c`), and is *not* a reader of the lttng-ust
domain.

Consequence (D1): data which lttng-ust unpublishes and then reclaims
after `lttng_ust_urcu_synchronize_rcu()` is not protected against
in-flight side event callbacks. The exposure is not limited to
filters: `lttng_event_reserve()` dereferences the channel context
array, and the bytecode interpreter dereferences the session or
event-notifier-group context array. Both are reclaimed by
`lttng_ust_context_add_field()` and `lttng_ust_context_set_provider_rcu()`
with a plain assign / synchronize / free sequence, and the latter runs
whenever an application registers or unregisters an application
context provider, i.e. while events are live and firing.

Resolution: the tracer waits for a grace period of every domain it can
be called from, rather than making the side callback take a second RCU
read lock per event (which would pay for it on the fast path):

- libside exports `side_tracer_callback_synchronize()`, which waits for
  a grace period of the domain within which the tracer callbacks are
  invoked (`event_rcu_gp`; this covers both application threads and
  events emitted from statedump callbacks by the agent thread). It must
  not be called from a tracer callback.
- lttng-ust has a hidden `lttng_ust_tracer_synchronize()` calling
  `lttng_ust_urcu_synchronize_rcu()` followed by
  `side_tracer_callback_synchronize()`. It replaces the bare urcu
  synchronize wherever the data being reclaimed is reachable from a
  probe: context publication (`lttng-context.c`), session and
  event-notifier-group teardown, probe provider event unregistration,
  and the `LTTNG_UST_ABI_WAIT_QUIESCENT` command.

Note that event lifetime itself does not depend on D1: the side
callback unregistration waits for a side grace period on its own, so
an unregistered event has no in-flight callback by the time
`unregister_event()` returns. The combined helper is used in the
teardown paths anyway, so that "wait for in-flight events" means the
same thing everywhere.

Lock order (L1 unchanged): the side grace period takes a side leaf
lock, and the combined helper is called with `ust_mutex` held, so this
is `ust_mutex` → side leaf, as documented. Waiting for a side grace
period from within a side notification callback (the REMOVE path,
which unregisters probes) is `side_notification_lock` → `ust_mutex` →
side leaf, also consistent. It is safe because side read-side
critical sections take no locks.

---

## 4. Required changes

libside:

1. Fork coordination: new phased-atfork library (3.1.1; ships in the
   side dist, neutral prefix, own soname) + libside's participant
   registrations at levels 0/2/3. `side_init` registers with the
   coordinator instead of calling pthread_atfork, removing the
   held-across-fork `side_agent_thread_lock` and the asymmetric
   early-return unlock pattern of the current static handlers
   (side.c:972+ at current HEAD).
2. Child-side reset of `event_rcu_gp`/`statedump_rcu_gp` reader
   counts + gp_lock/futex reinit + side mutex reinit (H1/H2) — the
   level-0 child callback (3.1.3).
3. DONE (libside 5aabf04): stage-1 lock split — `side_notification_lock`
   introduced (recursive; lists + notification delivery);
   `side_event_lock` demoted to a plain non-recursive leaf (per-event
   callback arrays + GP waits); lock-ordering comment block added in
   side.c. Nothing exported. (Supersedes the earlier exported-lock
   plan.)
4. DONE (libside e69d098): `side_init()` concurrency hardening via
   double-checked locking on a leaf `side_init_lock`, acquire loads on
   all call-site fast paths, release store last. Background:
   `side_init` carries
   `__attribute__((constructor))` on its declaration in trace.h:214,
   but the attribute only takes effect where the function is DEFINED
   (verified: declaration-only TUs emit no .init_array entry) — i.e.
   it makes libside.so's own definition a constructor, nothing more.
   DECIDED: the constructor/destructor attributes stay on the public
   header declarations — the definition inherits the attribute via
   inclusion, giving a single point of truth and ruling out
   incompatible re-declaration between header and implementation.
   Initialization is
   still guaranteed once the constructor phase completes (libside's
   ctor runs before dependent instrumented DSOs' ctors; dlopen batches
   covered by dependency-ordered ctors + pthread_create
   happens-before). Residual exposure is limited to threads spawned by
   constructors ordered BEFORE libside's ctor: concurrent first use
   during startup can double-run side_init (double pthread_atfork →
   permanent fork self-deadlock on the non-recursive
   `side_agent_thread_lock`; racing `side_rcu_gp_init`), and later
   unordered reads of `initialized` lack release/acquire pairing on
   weakly-ordered archs. Chosen fix: double-checked locking on a new
   leaf `init_mutex` reusing the existing `initialized` flag as sole
   init state (release store last inside the critical section; acquire
   on all unlocked reads, including call-site fast paths). No
   interaction with the L1 order; fork concurrent with first side use
   remains unsupported (same window as any once-style init).
5. DONE (libside 6cb2b7e): `_side_tracer_callback_unregister` memcpy
   element-vs-byte sizing (side.c:411-414; sibling of the `_register`
   bug fixed in 7f095a2). Was a prerequisite for multi-tracer/multi-key
   operation.
6. Optional: batch grace periods per notification batch; `-z nodelete`.

lttng-ust:

1. Listener pause/resume state machine + interruptible waits
   (poll+wakeup pipe; futex-loop pause flag; bounded mid-command I/O;
   interruptible reconnect delay; connect outside fd-tracker lock or
   bounded).
2. Register the level-1 fork participant
   (quiesce/acquire/parent/child per 3.1.3); TLS armed flag for
   old-preload compat; reimplement the `lttng_ust_before_fork` family
   over `fork_phases_*`; retire `ust_fork_mutex`. (Sigmask handling
   moves into the coordinator.)
3. Notification callbacks take `ust_lock` inside `side_notification_lock`;
   the listener under `ust_lock` restricted to leaf-safe side APIs
   (callback reg/unreg, statedump request, key); no side lock in
   `handle_message`. `lttng_ust_side_tracer_exit` called without ust
   locks.
4. `get_wait_shm` helper fork → `_Fork`/raw syscall.
5. `-Wl,-z,nodelete`; dlsym-probe for old-libside fallback.
6. Update the lttng-ust-comm.c locking doc block: L1 order; F1
   invariant; agent-progress wait rules; removal of ust_fork_mutex.

---

## 5. Contracts and documented restrictions

- fork() from a signal handler: unsupported (POSIX: fork is not
  AS-safe when atfork handlers exist; libside locks do not block
  signals).
- fork() from within a side read-side section, tracer notification
  callback, or statedump callback: forbidden (required for child-side
  RCU reset safety and F1).
- fork() from a dlopen constructor while libside statedumps are in
  flight: hazardous on glibcs where lazy TLS allocation takes the
  loader lock (agent's first Global-Dynamic TLS access can need the dl
  lock the forker holds; lttng pre-allocates TLS for its own threads
  only). Documented limitation; mitigation: agent-loop pause
  checkpoints between statedump requests to shrink the window.
- Fork liveness preconditions: bounded only if sessiond I/O is bounded.
  `LTTNG_UST_REGISTER_TIMEOUT=-1` makes connect/notify/reply I/O — and
  hence worst-case fork latency — unbounded on a sick sessiond. The
  quiescence design keeps the common case (idle listener) O(1); a
  dedicated park-wait bound is an open option (section 6).
- The fork coordinator must be a process-wide singleton (one shared
  soname): vendored duplicate copies recreate the inter-coordinator
  ordering problem. All fork participants must remain loaded for
  process lifetime (NODELETE).

## 6. Open questions

1. Child-side reinit cost inside the atfork child-handler phase
   (constructor re-registration `sem_timedwait` up to 3s runs inside
   `fork()`'s handler phase, serializing concurrent forks on glibc's
   internal atfork lock). Option: defer child re-registration to a
   post-fork trampoline or first use.
2. Dedicated timeout for the park ACK-wait vs. inheriting command I/O
   bounds.
3. Whether libside should offer mid-statedump pause checkpoints
   (between requests, between events) to reduce fork latency and the
   dl-lock window (section 5).
4. Multi-session/multi-key callback fan-out (one lttng callback
   registration per key per event) magnifies the N-grace-periods cost;
   batching design in libside.
5. Phased-atfork coordination library: final naming/prefix and
   soname; whether `level` should be an enum of named stages instead
   of an int; exact unregister-during-fork semantics; whether to
   propose phased atfork upstream (glibc).
6. Async-signal-safe child reinit track (3.1.7): per-structure
   reinit inventory, deferral of child cleanup to the continue path,
   retirement of the acquire stage, and the annotated
   lock-reinitialization helper for validators (helgrind/TSan/
   errorcheck-robust mutexes) — the reinit-by-assignment pattern is
   formally UB on a held mutex and defeats validator lock-state
   models, so it must be funneled through one documented, annotated
   helper.

## 7. Review provenance

Adversarially reviewed 2026-08-27 (Fable skeptic pass over the source
of both trees). Material corrections folded in: urcu registry-lock (not
gp-lock) as the first-use blocking edge; listener thread inventory
corrected twice (skeptic said three concurrent; actually at most two
concurrent of three sock_info — ust_app is the LTTNG_UST_APP_PATH
in-app listener, not lttng-ust-ctl/consumerd); mid-command recvmsg and
reconnect-sleep park obstacles;
`ust_fork_mutex` role attribution vs `ust_exit_mutex`; nest-counted
`ust_mutex` invalidating the naive double-prepare deadlock claim (real
deadlocks: urcu mutexes, double ACK-wait); NODELETE assumption refuted
(runtime dlopen only, glibc auto-unregisters atfork handlers on
dlclose); new blockers H1 (inherited side-RCU reader counts hang child
GPs) and H2 (side mutexes not quiesced across fork) driving 3.1.3.
