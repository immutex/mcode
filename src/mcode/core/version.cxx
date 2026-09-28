#include "mcode/core/version.hxx"

// Every feature-test macro below is defined by its own header. Including them is
// what makes `detect_library_support` report the truth: without these, GCC 14 and
// Apple Clang 16 both report false for all four, because the standard library
// only defines the macro once the header is pulled in.
#include <expected>
#include <version>

// <generator> and <print> are C++23 library features that a given standard
// library may simply not ship yet. Their absence is what the probe reports, so
// they are included only when the header exists.
#if __has_include( <generator> )
#include <generator>
#endif

#if __has_include( <print> )
#include <print>
#endif

#include <functional>

namespace mcode {

	auto detect_library_support( ) noexcept -> library_support {
		auto support = library_support{ };

	#ifdef __cpp_lib_generator
		support.generator = true;
	#endif

	#ifdef __cpp_lib_move_only_function
		support.move_only_function = true;
	#endif

	#ifdef __cpp_lib_print
		support.print = true;
	#endif

	#ifdef __cpp_lib_expected
		support.expected = true;
	#endif

		return support;
	}

}
