"""mcode dependency manifest (Conan 2).

LuaJIT is not resolved from Conan Center: that recipe is frozen at 2.1.0-beta3
(2017) and does not build against a modern MSVC. Build the local recipe first:

    conan create conan/recipes/luajit --profile conan/profiles/windows-msvc --build=missing

Boost is NOT header-only -- Boost.Process v2 ships compiled sources.
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class McodeConan(ConanFile):
    name = "mcode"
    version = "0.0.1"
    description = "Extensible C++23 coding-agent harness with a LuaJIT extension layer"
    license = "Apache-2.0"
    package_type = "application"

    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {
        "fPIC": True,
        "boost/*:header_only": False,
        "boost/*:without_graph": True,
        "boost/*:without_iostreams": True,
        "boost/*:without_locale": True,
        "boost/*:without_log": True,
        "boost/*:without_math": True,
        "boost/*:without_mpi": True,
        "boost/*:without_nowide": True,
        "boost/*:without_numeric": True,
        "boost/*:without_program_options": True,
        "boost/*:without_python": True,
        "boost/*:without_random": True,
        "boost/*:without_serialization": True,
        "boost/*:without_stacktrace": True,
        "boost/*:without_test": True,
        "boost/*:without_type_erasure": True,
        "boost/*:without_url": True,
        "boost/*:without_uuid": True,
        "boost/*:without_wave": True,
        "boost/*:without_yaml": True,
        "boost/*:without_json": True,
        "boost/*:without_contract": True,
        "boost/*:without_fiber": True,
        "boost/*:without_cobalt": True,
        "boost/*:without_coroutine": True,
        "boost/*:without_wserialization": True,
        "boost/*:without_pfr": True,
        "fmt/*:header_only": False,
        "spdlog/*:header_only": False,
        "spdlog/*:shared": False,
        "fmt/*:shared": False,
        "mimalloc/*:shared": False,
        "simdutf/*:shared": False,
        "yyjson/*:shared": False,
        "luajit/*:shared": False,
    }

    def requirements(self):
        # Center lags docs/14 on three patch versions: yyjson 0.12 vs 0.13,
        # simdutf 9.0 vs 9.2, unordered_dense 5.0.1 vs 5.2. fmt follows spdlog's
        # exact pin (12.1.0), since Conan rejects the conflict.
        self.requires("fmt/12.1.0")
        self.requires("spdlog/1.17.0")
        self.requires("yyjson/0.12.0")
        self.requires("boost/1.91.0")
        self.requires("mimalloc/3.5.1")
        self.requires("simdutf/9.0.0")
        self.requires("unordered_dense/5.0.1")
        self.requires("luajit/2.1.0-mcode.1")

    def build_requirements(self):
        self.test_requires("catch2/3.16.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["MCODE_VERSION"] = str(self.version)
        tc.generate()
