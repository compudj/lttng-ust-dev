// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// A libside-instrumented C++ application.
//
// libside has one instrumentation API and it is the C one: the macros
// below are the same ones side-example.c uses. What differs is what
// they expand to and what a C++ program hands them, which is what this
// example is about.
//
//   side_example_cxx:request     the types of a C++ program: a
//                                std::string, an enum class, a bool
//   side_example_cxx:scope_begin a scope guard, whose constructor and
//   side_example_cxx:scope_end   destructor emit the two events, so
//                                that an early return or an exception
//                                still closes the scope
//   side_example_cxx:buffer      the elements of a std::vector, read
//                                from the memory of the application
//                                rather than copied one by one
//
// Two things to know before instrumenting a C++ translation unit:
//
//   - The macros need the GNU dialect, which is what a compiler uses
//     when no -std is given. They rely on the `, ## __VA_ARGS__`
//     extension to leave out an optional attribute list, so -std=c++17
//     fails to compile where -std=gnu++17 succeeds. This is not a
//     property of C++: -std=c11 fails just the same.
//
//     -Wpedantic does not work either, and that one is specific to
//     C++: an event or a type with no attributes declares an array of
//     none, and C++ rejects a zero size array where C accepts it.
//
//   - side_static_event() cannot expand to a static variable, since C++
//     has no way to forward declare one. It defines the event in an
//     anonymous namespace instead, which gives it the same scope. An
//     event is therefore defined at namespace scope, never inside a
//     class, and a member function refers to it like any other name.

#include <side/trace.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

//
// side_example_cxx:request
// ------------------------
//

// An enum class is recorded as the integer it is, and the mappings name
// its values in the trace. The underlying type is what the description
// has to agree with, so it is worth naming it rather than leaving it to
// the compiler.
enum class Outcome : uint32_t {
	Ok = 0,
	Retried = 1,
	Failed = 2,
};

static side_define_enum(outcome_mappings,
	side_enum_mapping_list(
		side_enum_mapping_value("ok", static_cast<uint32_t>(Outcome::Ok)),
		side_enum_mapping_value("retried", static_cast<uint32_t>(Outcome::Retried)),
		side_enum_mapping_value("failed", static_cast<uint32_t>(Outcome::Failed)),
	)
);

side_static_event(request_event, "side_example_cxx", "request", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_string("method"),
		side_field_string("path"),
		side_field_enum("outcome", &outcome_mappings, side_elem(side_type_u32())),
		side_field_bool("cached"),
		side_field_u16("nr_samples"),
	)
);

//
// side_example_cxx:scope_begin, side_example_cxx:scope_end
// --------------------------------------------------------
//
// The pair a scope guard emits. They are at a lower log level than the
// events above, so that a session can record the work of the
// application without recording its scopes:
//
//   lttng enable-event --userspace 'side_example_cxx:*' --loglevel=INFO
//

side_static_event(scope_begin_event, "side_example_cxx", "scope_begin", SIDE_LOGLEVEL_DEBUG,
	side_field_list(
		side_field_string("name"),
	)
);

side_static_event(scope_end_event, "side_example_cxx", "scope_end", SIDE_LOGLEVEL_DEBUG,
	side_field_list(
		side_field_string("name"),
		side_field_u64("duration_us"),
	)
);

// The destructor runs however the scope is left, so the two events are
// paired on an early return and on an exception as well as on falling
// off the end.
class Scope {
public:
	explicit Scope(const char *name) :
		_name(name),
		_begin(std::chrono::steady_clock::now())
	{
		side_event(scope_begin_event, side_arg_list(side_arg_string(_name)));
	}

	~Scope()
	{
		const auto elapsed = std::chrono::steady_clock::now() - _begin;
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
			elapsed).count();

		side_event(scope_end_event, side_arg_list(
			side_arg_string(_name),
			side_arg_u64(static_cast<uint64_t>(us))));
	}

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	const char *_name;
	std::chrono::steady_clock::time_point _begin;
};

//
// side_example_cxx:buffer
// -----------------------
//
// The elements of a std::vector are not known when the event is
// described and an argument list is a literal, so they cannot be pushed
// one by one. Describe where to read them from instead: the gather
// types name an offset from a base address, which is what
// side-example.c uses for the state of a C structure.
//
// The layout being described has to be one the standard guarantees, so
// this reads a view the caller fills rather than the innards of the
// vector, which no offset can name portably.
//

struct BufferView {
	const uint32_t *samples;
	uint16_t nr_samples;
	uint32_t total;
};

// offsetof() is defined for a standard layout type. Say so where it is
// declared rather than finding out on a compiler which rejects it.
static_assert(std::is_standard_layout<BufferView>::value,
	"BufferView must be a standard layout type for offsetof() to name its members");

side_static_define_struct(buffer_desc,
	side_field_list(
		side_field_gather_unsigned_integer("total",
			offsetof(BufferView, total),
			side_struct_field_sizeof(BufferView, total), 0, 0,
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		// The elements live behind a pointer and their number is
		// read from the view which holds it.
		side_field_gather_vla("samples",
			side_elem(side_type_gather_unsigned_integer(0,
				sizeof(uint32_t), 0, 0,
				SIDE_TYPE_GATHER_ACCESS_DIRECT)),
			offsetof(BufferView, samples),
			SIDE_TYPE_GATHER_ACCESS_POINTER,
			side_length(side_type_gather_unsigned_integer(
				offsetof(BufferView, nr_samples),
				side_struct_field_sizeof(BufferView, nr_samples),
				0, 0, SIDE_TYPE_GATHER_ACCESS_DIRECT))),
	)
);

side_static_event(buffer_event, "side_example_cxx", "buffer", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_gather_struct("stats", buffer_desc, 0,
			sizeof(BufferView), SIDE_TYPE_GATHER_ACCESS_DIRECT),
	)
);

//
// The application
// ---------------
//

static uint32_t sum(const std::vector<uint32_t>& samples)
{
	uint32_t total = 0;

	for (const auto sample : samples) {
		total += sample;
	}

	return total;
}

static void handle_request(const std::string& method, const std::string& path,
		Outcome outcome, bool cached, const std::vector<uint32_t>& samples)
{
	Scope scope("handle_request");

	// A std::string is recorded as the string it holds: what the
	// event takes is the pointer c_str() returns, which stays valid
	// for as long as the string is not modified. Nothing is copied
	// out of it until the event is recorded, and only then if a
	// tracer subscribed to it.
	side_event(request_event, side_arg_list(
		side_arg_string(method.c_str()),
		side_arg_string(path.c_str()),
		side_arg_u32(static_cast<uint32_t>(outcome)),
		side_arg_bool(cached),
		side_arg_u16(static_cast<uint16_t>(samples.size()))));

	const BufferView view = {
		samples.data(),
		static_cast<uint16_t>(samples.size()),
		sum(samples),
	};

	// The argument is the address the description reads from: the
	// elements of the vector are never copied by the caller.
	side_event(buffer_event, side_arg_list(side_arg_gather_struct(&view)));
}

int main(void)
{
	handle_request("GET", "/index.html", Outcome::Ok, true, { 10, 20, 30 });
	handle_request("POST", "/submit", Outcome::Retried, false, { 7, 7 });
	handle_request("GET", "/missing", Outcome::Failed, false, {});
	std::printf("emitted 3 requests, 3 buffers and 3 pairs of scope events\n");
	return 0;
}
