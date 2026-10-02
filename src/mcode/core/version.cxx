#include "mcode/core/version.hxx"

// Each feature-test macro is defined by its own header; without these the probe reports false.
#include <expected>
#include <version>

// Included only when present: a standard library may not ship these C++23 headers yet.
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
