<!--
SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# libside instrumentation

Two applications instrumented with
[libside](https://github.com/efficios/libside), one in C and one in
C++, recorded by LTTng and decoded by Babeltrace.

`side-example` and `side-example-cxx` depend on libside only. They do
not link against LTTng-UST, and they do not know a tracer exists: an
event which nobody subscribed to costs a predicted-not-taken branch.
`run-side-example` preloads `liblttng-ust.so.1`, which subscribes to
those same events and records them.

    make
    ./run-side-example

`run-side-example` needs `lttng` and `babeltrace2` in `PATH`. It writes
its traces to a temporary directory and removes the recording session
and the trigger it creates on the way out.

Its third section renders network addresses only with a `babeltrace2`
whose `sink.text.pretty` acts on the `lttng.fmt` attributes; every
other section, and the trace itself, are the same either way. The
script says which of the two it found.

## What the four events of `side-example` are for

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

`side_example:packet` — the attributes. An attribute says how a field
is meant to be read, which its type does not: `saddr`, `daddr` and
`peer.addr` are 32-bit unsigned integers, `saddr6` and `daddr6` are
sixteen bytes each, and nothing about those types makes them network
addresses. The application says so with `lttng.fmt.ipv4` and
`lttng.fmt.ipv6`, and a reader which knows those attributes renders the
fields as addresses.

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
the eight events those three rules match are recorded.

**3. An attribute says how a field is meant to be read.** The
application attaches `lttng.fmt.ipv4` to the type of three fields of
`side_example:packet` and `lttng.fmt.ipv6` to the type of two others:

    side_field_u32_be("saddr",
        side_attr_list(side_attr("lttng.fmt.ipv4", side_attr_bool(true)))),

An attribute belongs to the type it describes, so it travels with it:
libside hands it to LTTng-UST, which reports it to the session daemon,
which writes it to the metadata of the trace as a property of the field
class. Nothing along that path knows what an address is. The script
prints the attributes as Babeltrace reads them back, with
`sink.text.details`:

    saddr: Unsigned integer (32-bit, Base 10):
      User attributes:
        lttng.fmt:
          ipv4: Yes

A Babeltrace whose `sink.text.pretty` acts on those attributes then
renders `saddr = 10.0.0.1` where another prints `saddr = 167772161`.
The bytes in the trace are the same either way: an attribute says how
to read a value, not what is recorded, and a reader which does not know
`lttng.fmt` still decodes every one of those fields. The script says
which of the two it got.

An IPv4 address is declared `side_field_u32_be()`, so the trace holds
the four bytes which were on the wire whatever the byte order of the
host, and the reader converts them once. An IPv6 address is an array of
`side_type_byte`, which the tracer records as a blob.

**4. The same instrumentation from C++.** `side-example-cxx` is a C++
program. libside has one instrumentation API and it is the C one, so
the macros are the same ones `side-example.c` uses; what differs is
what a C++ program hands them.

`side_example_cxx:request` — a `std::string` recorded through
`c_str()`, and an `enum class` through its underlying type, with the
mappings naming its values in the trace.

`side_example_cxx:scope_begin` and `side_example_cxx:scope_end` — the
pair a scope guard emits. The destructor runs however the scope is
left, so an early return or an exception closes the scope as falling
off the end does. They are at the `DEBUG` log level and the other
events at `INFO`, so a session can record the work of the application
without its scopes, with `--loglevel=INFO`.

`side_example_cxx:buffer` — the elements of a `std::vector`. They are
not known when the event is described and an argument list is a
literal, so they cannot be pushed one by one: the description says
where to read them from instead, through a view whose layout the
standard guarantees rather than the innards of the vector, which no
offset can name portably.

Two things to know before instrumenting a C++ translation unit:

- The macros need the GNU dialect, which is what a compiler uses when
  no `-std` is given. They rely on the `, ## __VA_ARGS__` extension to
  leave out an optional attribute list, so `-std=c++17` fails to
  compile them where `-std=gnu++17` succeeds. This is not a property of
  C++: `-std=c11` fails just the same. `-Wpedantic` does not work on
  the C++ program either, and that one is specific to C++: an event or
  a type with no attributes declares an array of none, and C++ rejects
  a zero size array where C accepts it.
- `side_static_event()` cannot expand to a static variable, since C++
  has no way to forward declare one. It defines the event in an
  anonymous namespace instead, which gives it the same scope. An event
  is therefore defined at namespace scope, never inside a class, and a
  member function refers to it like any other name.

**5. Capture subfields with a trigger.** A capture descriptor names
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

The first section decodes eleven events. Abridged, and with the
timestamps and the packet context removed:

    side_example:reading: { seq = 0, sensor = "cpu0", state = ( "warming" ), celsius = 41.5, calibrated = 0, id_host = 4096, id_le = 4096, id_be = 4096, volts_be = 1.25 }
    side_example:reading: { seq = 1, sensor = "cpu1", ... id_host = 4097, id_le = 4097, id_be = 4097, volts_be = 2.5 }
    side_example:reading: { seq = 2, sensor = "gpu0", ... id_host = 4098, id_le = 4098, id_be = 4098, volts_be = 3.75 }
    side_example:frame: { header = { magic = 51966, version = 1 }, channels = [ [0] = { id = 1, samples = [ [0] = 10, [1] = 20, [2] = 30 ] }, [1] = { id = 2, samples = [ [0] = 40, [1] = 50, [2] = 60 ] } ], bursts = [ [0] = { mark = 1, points = [ [0] = 101, [1] = 102 ] }, [1] = { mark = 2, points = [ [0] = 103 ] } ], payload = { 12648430 } }
    side_example:frame: { header = { magic = 51966, version = 2 }, ... bursts = [ [0] = { mark = 9, points = [ [0] = 201, [1] = 202, [2] = 203 ] } ], payload = { "degraded" } }
    side_example:snapshot: { sensor = { id = 1, name = "cpu0", value = 41.5, active = 1, samples = [ [0] = 5, [1] = 15, [2] = 25 ], axes = [ [0] = { axis_id = 0, reading = -3 }, [1] = { axis_id = 1, reading = 7 } ] }, window = [ [0] = 7, [1] = 14, [2] = 21, [3] = 28 ] }
    side_example:snapshot: { sensor = { id = 2, name = "cpu1", ... axes = [ [0] = { axis_id = 0, reading = 12 } ] }, ... }
    side_example:snapshot: { sensor = { id = 3, name = "gpu0", ... axes = [ [0] = { axis_id = 0, reading = -1 }, [1] = { axis_id = 1, reading = 0 }, [2] = { axis_id = 2, reading = 42 } ] }, ... }
    side_example:packet: { seq = 0, saddr = 10.0.0.1, daddr = 192.168.1.1, saddr6 = 2001:db8::1, daddr6 = 2001:db8::2, peer = { addr = 10.0.0.254, port = 443 }, length = 1500 }
    side_example:packet: { seq = 1, saddr = 172.16.5.4, daddr = 8.8.8.8, saddr6 = fe80::1, daddr6 = ::ffff:8.8.8.8, peer = { addr = 172.16.5.1, port = 53 }, length = 590 }
    side_example:packet: { seq = 2, saddr = 127.0.0.1, daddr = 255.255.255.255, saddr6 = ::, daddr6 = ff02::1, peer = { addr = 127.0.0.1, port = 9 }, length = 64 }

The three `id_` fields of a reading decode to the same number: they
hold it in three byte orders and the metadata says which.

The addresses of a packet are what the attributes make of them. A
Babeltrace which does not act on `lttng.fmt` prints the same three
events as

    side_example:packet: { seq = 0, saddr = 167772161, daddr = 3232235777, saddr6 = { 20 01 0d b8 00 00 00 00 00 00 00 00 00 00 00 01 }, ... }

The second section decodes three of the eight events its three rules
match: the reading whose sequence number is 1, the second frame, and
the `gpu0` snapshot.

The fourth section decodes the twelve events of the C++ program, four
per request:

    side_example_cxx:scope_begin: { name = "handle_request" }
    side_example_cxx:request: { method = "GET", path = "/index.html", outcome = ( "ok" ), cached = 1, nr_samples = 3 }
    side_example_cxx:buffer: { stats = { total = 60, samples = [ [0] = 10, [1] = 20, [2] = 30 ] } }
    side_example_cxx:scope_end: { name = "handle_request", duration_us = 43 }

The third request passes an empty vector, which is recorded as the
sequence of no elements it is: `samples = [ ]`. The durations vary from
one run to the next.

## Fields which do not belong to the application

CTF requires the length of a sequence and the selector of a variant to
precede the field they belong to, so the tracer synthesizes a field for
each: `_bursts_length` before `bursts`, `_payload_selector` before
`payload`. The application never described them, and they repeat what
the field itself already shows.

They carry the `lttng.visibility.hidden` attribute, which says that a
reader showing the payload to a person may leave them out. Nothing else
changes: they are recorded, they are decoded, `sink.text.details`
writes them along with the attribute, and the filters of the second
section reach into the sequences they measure.

A `babeltrace2` whose `sink.text.pretty` does not act on the attribute
prints them, and one which does prints them again when asked to, with
`-p print-hidden=true`:

    ..., _bursts_length = 2, bursts = [ [0] = { mark = 1, _points_length = 2, points = [ ... ] } ], _payload_selector = ( "option_0" : container = 0 ), payload = { 12648430 }

That parameter also writes back the value behind the labels of an
enumeration field, which the same `sink.text.pretty` leaves out for the
same reason: `state = ( "warming" )` rather than
`state = ( "warming" : container = 1 )`. A value which matches no label
keeps it either way, since nothing else says what it is:
`state = ( <unknown> : container = 42 )`.

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
