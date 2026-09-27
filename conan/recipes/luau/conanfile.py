"""Luau, commit-pinned, built from upstream with CMake.

Static-only, deliberately. Upstream gates `LUAU_BUILD_SHARED` behind
`LUAU_EXTERN_C`, which force-enables `LUA_USE_LONGJMP=1` -- that changes how
`luaL_error` and the panic handler propagate, from C++ exceptions to longjmp.
mcode's host catches VM errors as C++ exceptions, so the shared configuration is
a different API contract, not a packaging variant we want to support.
"""

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
from conan.tools.microsoft import is_msvc

class LuauConan(ConanFile):
    name = "luau"
    version = "0.0.0-mcode.c0e346ed"
    description = "Luau (commit-pinned) - a fast, small, safe, gradually typed Lua derivative"
    license = "MIT"
    url = "https://github.com/mcode-dev/mcode"
    homepage = "https://luau.org/"
    topics = ("lua", "scripting", "interpreter", "sandbox")

    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    # Upstream publishes no version tags, so the pin is a commit.
    _commit = "c0e346edd89066b44dca174c9f54ce84c746a540"

    # Luau.Compiler depends on Luau.Ast and Luau.Bytecode, both of which depend
    # on Luau.Common; Luau.VM also depends on Luau.Common. Static link order is
    # dependents-first, so Common is last.
    _libs = ("Luau.Compiler", "Luau.Ast", "Luau.Bytecode", "Luau.VM", "Luau.Common")

    _targets = "Luau.VM Luau.Compiler Luau.Ast Luau.Bytecode Luau.Common"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(
            self,
            f"https://github.com/luau-lang/luau/archive/{self._commit}.tar.gz",
            destination=self.source_folder,
            strip_root=True,
        )

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["LUAU_BUILD_CLI"] = False
        toolchain.variables["LUAU_BUILD_TESTS"] = False
        toolchain.variables["LUAU_BUILD_WEB"] = False
        toolchain.variables["LUAU_BUILD_SHARED"] = False

        # Match the consumer's CRT. Mismatching it across the VM and the host is
        # heap corruption, not a link error.
        static_crt = is_msvc(self) and self.settings.compiler.runtime == "static"
        toolchain.variables["LUAU_STATIC_CRT"] = bool(static_crt)

        # Upstream compiles a hot interpreter file with /d2ssa-pre- to work around
        # an MSVC codegen regression; it is set in their CMakeLists already, so
        # nothing to do here.
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()

        # Build only the five libraries we ship. `all` would also pull in
        # Analysis, Config, CodeGen, Inliner, Require and isocline.
        for target in self._targets.split():
            cmake.build(target=target)

    def package(self):
        include = os.path.join(self.package_folder, "include")
        lib = os.path.join(self.package_folder, "lib")

        copy(self, "LICENSE.txt", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "lua_LICENSE.txt", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

        for directory in ("VM/include", "Compiler/include", "Common/include"):
            copy(self, "*.h", src=os.path.join(self.source_folder, directory),
                 dst=include, keep_path=True)

        for library in self._libs:
            copy(self, f"lib{library}.a", src=self.build_folder, dst=lib)
            copy(self, f"{library}.lib", src=self.build_folder, dst=lib)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "luau")
        self.cpp_info.set_property("cmake_target_name", "luau::luau")
        self.cpp_info.set_property("cmake_find_mode", "config")

        # All three upstream include roots are flattened into one `include`
        # directory, so both components resolve headers from it and a consumer
        # writes `#include "lua.h"` / `#include "luacode.h"` / `#include
        # <Luau/Compiler.h>`, exactly as upstream does.
        self.cpp_info.components["vm"].set_property(
            "cmake_target_name", "luau::vm")
        self.cpp_info.components["vm"].includedirs = ["include"]
        self.cpp_info.components["vm"].libs = ["Luau.VM", "Luau.Common"]

        self.cpp_info.components["compiler"].set_property(
            "cmake_target_name", "luau::compiler")
        self.cpp_info.components["compiler"].includedirs = ["include"]
        self.cpp_info.components["compiler"].libs = [
            "Luau.Compiler", "Luau.Ast", "Luau.Bytecode", "Luau.Common"]

        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.components["vm"].system_libs = ["m"]
        if self.settings.os == "Macos":
            self.cpp_info.components["vm"].frameworks = ["CoreFoundation"]
