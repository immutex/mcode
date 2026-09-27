#include "mcode/core/version.hxx"

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
