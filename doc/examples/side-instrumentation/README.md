<!--
SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# libside instrumentation

An application instrumented with [libside](https://github.com/efficios/libside),
recorded by LTTng and decoded by Babeltrace.

`side-example` depends on libside only. It does not link against
LTTng-UST, and it does not know a tracer exists: an event which nobody
subscribed to costs a predicted-not-taken branch. `run-side-example`
preloads `liblttng-ust.so.1`, which subscribes to those same events and
records them.

    make
    ./run-side-example

`run-side-example` needs `lttng` and `babeltrace2` in `PATH`. It writes
its traces to a temporary directory and removes the recording session
and the trigger it creates on the way out.

## What the three events are for

`side_example:reading` — the scalar types: unsigned and signed
integers, a string, an enumeration, a `binary64`, a boolean. It also
carries one value three times, held in host, little-endian and
big-endian order. Each is recorded exactly as it is emitted, and the
metadata says which byte order it is in, so a trace keeps the bytes
which were on the wire rather than a conversion of them.

`side_example:frame` — the compound types: a structure, an array of
structures each holding an array, a sequence of structures each holding
a sequence, and a variant. The two frames share one event description
and record a different number of bytes: their sequences have different
lengths and their variants select different options. Only the selected
option of a variant occupies the trace.

`side_example:snapshot` — the gather types. The application does not
lay its state out for the tracer: it describes the layout it already
has, and each field says where to read itself from as an offset from a
base address, and whether that address is the value or a pointer to it.
Nothing is marshalled by the caller — the two arguments of the event
are the addresses to read from. It includes a sequence of structures
reached through a pointer whose length is itself read from the
structure which holds it.

## What the script shows

**1. Record everything.** `lttng enable-event --userspace
'side_example:*'`, run the application, and hand the trace to
`babeltrace2`.

**2. Filter below the top level of the payload.** A filter reaches the
member of a structure, the element of an array or of a sequence, and
any mix of the two, whether the values are copied onto the argument
vector or gathered from memory:

    id_be == 4097
    channels[1].samples[2] == 61
    sensor.name == "gpu0" && sensor.axes[2].reading == 42

The first shows the byte order handled: the buffer keeps the bytes as
they were emitted, and the filter converts to the byte order of the
host before comparing, so the expression matches the same reading on
any host. The last two descend through indices and member names into a
stack-copy payload and into application memory respectively. Three of
the eight events are recorded.

**3. Capture subfields with a trigger.** A capture descriptor names
what a notification carries when an event rule matches, and it reaches
the same fields a filter does:

    lttng add-trigger --condition event-rule-matches --type=user \
        --name='side_example:snapshot' \
        --capture 'sensor.id' \
        --capture 'sensor.name' \
        --capture 'sensor.samples[0]' \
        --capture 'sensor.axes[0].reading' \
        --capture 'window[3]' \
        --action notify

The script prints the descriptors with `lttng list-triggers`. The
values themselves reach a program which subscribes to the condition of
the trigger over a notification channel, with
`lttng_notification_channel_create()` and
`lttng_evaluation_event_rule_matches_get_captured_values()`.

## Expected output

The first section decodes eight events. Abridged, and with the
timestamps and the packet context removed:

    side_example:reading: { seq = 0, sensor = "cpu0", state = ( "warming" : container = 1 ), celsius = 41.5, calibrated = 0, id_host = 4096, id_le = 4096, id_be = 4096, volts_be = 1.25 }
    side_example:reading: { seq = 1, sensor = "cpu1", ... id_host = 4097, id_le = 4097, id_be = 4097, volts_be = 2.5 }
    side_example:reading: { seq = 2, sensor = "gpu0", ... id_host = 4098, id_le = 4098, id_be = 4098, volts_be = 3.75 }
    side_example:frame: { header = { magic = 51966, version = 1 }, channels = [ [0] = { id = 1, samples = [ [0] = 10, [1] = 20, [2] = 30 ] }, [1] = { id = 2, samples = [ [0] = 40, [1] = 50, [2] = 60 ] } ], _bursts_length = 2, bursts = [ [0] = { mark = 1, _points_length = 2, points = [ [0] = 101, [1] = 102 ] }, [1] = { mark = 2, _points_length = 1, points = [ [0] = 103 ] } ], _payload_selector = ( "option_0" : container = 0 ), payload = { 12648430 } }
    side_example:frame: { header = { magic = 51966, version = 2 }, ... _bursts_length = 1, bursts = [ [0] = { mark = 9, _points_length = 3, points = [ [0] = 201, [1] = 202, [2] = 203 ] } ], _payload_selector = ( "option_1" : container = 1 ), payload = { "degraded" } }
    side_example:snapshot: { sensor = { id = 1, name = "cpu0", value = 41.5, active = 1, _samples_length = 3, samples = [ [0] = 5, [1] = 15, [2] = 25 ], _axes_length = 2, axes = [ [0] = { axis_id = 0, reading = -3 }, [1] = { axis_id = 1, reading = 7 } ] }, window = [ [0] = 7, [1] = 14, [2] = 21, [3] = 28 ] }
    side_example:snapshot: { sensor = { id = 2, name = "cpu1", ... _axes_length = 1, axes = [ [0] = { axis_id = 0, reading = 12 } ] }, ... }
    side_example:snapshot: { sensor = { id = 3, name = "gpu0", ... _axes_length = 3, axes = [ [0] = { axis_id = 0, reading = -1 }, [1] = { axis_id = 1, reading = 0 }, [2] = { axis_id = 2, reading = 42 } ] }, ... }

The three `id_` fields of a reading decode to the same number: they
hold it in three byte orders and the metadata says which.

The second section decodes three of those eight: the reading whose
sequence number is 1, the second frame, and the `gpu0` snapshot.

## Restrictions

An event whose description contains a field type the tracer does not
support is skipped rather than partly recorded: the tracer logs which
one at `LTTNG_UST_DEBUG` level and leaves the event unregistered.

The elements of a *gathered* sequence must be of a size the description
fixes: the gather scalars, and a gathered structure or array built out
of them. A gathered string and a gathered sequence are not, since the
length of a gathered sequence is read from memory on each of the two
serialization passes and can change in between. A stack-copy sequence
has no such restriction.
