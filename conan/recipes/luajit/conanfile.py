"""LuaJIT 2.1, commit-pinned, built from upstream.

Only the static variant is offered on Windows. msvcbuild.bat compiles the DLL
build with /MD while mcode uses /MT, and mixing runtimes is undefined behaviour
rather than a link error.
"""

import os

from conan import ConanFile
from conan.errors import ConanException, ConanInvalidConfiguration
from conan.tools.files import copy, get
from conan.tools.microsoft import VCVars, is_msvc


class LuaJITConan(ConanFile):
    name = "luajit"
    version = "2.1.0-mcode.1"
    description = "LuaJIT 2.1 (commit-pinned) - a just-in-time compiler for Lua"
    license = "MIT"
    url = "https://github.com/mcode-dev/mcode"
    homepage = "https://luajit.org/"
    topics = ("lua", "jit", "scripting", "interpreter")

    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    _commit = "c6ffc141a8762b41703f9287d63d93622a13dd8f"

    _public_headers = (
        "lua.h",
        "luaconf.h",
        "lualib.h",
        "lauxlib.h",
        "luajit.h",
        "lua.hpp",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def validate(self):
        if self.settings.os == "Windows" and self.options.shared:
            raise ConanInvalidConfiguration(
                "luajit/2.1.0-mcode.1: only the static variant is supported on Windows."
            )

    def source(self):
        get(
            self,
            f"https://github.com/LuaJIT/LuaJIT/archive/{self._commit}.tar.gz",
            destination=self.source_folder,
            strip_root=True,
        )

    def generate(self):
        # Must run in the generate phase: it writes conanvcvars.bat and appends a
        # call to it from conanbuild.bat, which self.run() sources. Calling it
        # from build() is too late, and msvcbuild.bat fails with
        # "You must open a Visual Studio Command Prompt".
        VCVars(self).generate()

    def build(self):
        if is_msvc(self):
            self.run("msvcbuild.bat static", cwd=os.path.join(self.source_folder, "src"))

            # msvcbuild.bat prints its failure and then falls through to :END, so
            # it exits 0 even when the build failed.
            library = os.path.join(self.source_folder, "src", "lua51.lib")
            if not os.path.isfile(library):
                raise ConanException("msvcbuild.bat produced no lua51.lib")
        else:
            jobs = os.cpu_count() or 4
            mode = "shared" if self.options.shared else "static"
            self.run(f"make -j{jobs} BUILDMODE={mode}", cwd=self.source_folder)

            library = os.path.join(self.source_folder, "src", "libluajit.a")
            if not os.path.isfile(library):
                raise ConanException("LuaJIT build produced no libluajit.a")

    def package(self):
        src = os.path.join(self.source_folder, "src")
        include = os.path.join(self.package_folder, "include")
        lib = os.path.join(self.package_folder, "lib")
        binary = os.path.join(self.package_folder, "bin")

        for header in self._public_headers:
            copy(self, header, src=src, dst=include)

        copy(self, "COPYRIGHT", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

        if self.settings.os == "Windows":
            copy(self, "lua51.lib", src=src, dst=lib)
            copy(self, "luajit.exe", src=src, dst=binary)
        else:
            if self.options.shared:
                copy(self, "libluajit.so*", src=src, dst=lib)
                copy(self, "libluajit*.dylib", src=src, dst=lib)
            else:
                copy(self, "libluajit.a", src=src, dst=lib)

            copy(self, "luajit", src=src, dst=binary)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "luajit")
        self.cpp_info.set_property("cmake_target_name", "luajit::luajit")

        if self.settings.os == "Windows":
            # msvcbuild.bat names the static archive lua51.lib even in the
            # non-DLL build; the name is historical.
            self.cpp_info.libs = ["lua51"]

            if self.options.shared:
                self.cpp_info.defines = ["LUA_BUILD_AS_DLL"]
        else:
            self.cpp_info.libs = ["luajit"]

            if self.settings.os in ("Linux", "FreeBSD"):
                self.cpp_info.system_libs = ["m", "dl"]
