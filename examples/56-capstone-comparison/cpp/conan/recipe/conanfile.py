"""Conan 2 recipe for NVIDIA stdexec, the P2300 reference implementation.

Why this file exists at all: stdexec is NOT in ConanCenter (`conan search
stdexec -r conancenter` reports no recipe, and `*exec*` matches nothing), it is
not packaged by Fedora, and upstream's own conanfile is named `p2300` and takes
its version from `git.get_commit()` -- so there is no stable version to depend
on. Upstream publishes no GitHub releases either, only `nvhpc-*` snapshot tags.

So ch56 pins a tag tarball by sha256. That hash is what makes the build
reproducible; the tag alone would not be, since a tag can be moved.

Header-only: there is nothing to compile here, only headers to place.
"""

from conan import ConanFile
from conan.tools.files import get, copy
import os


class StdexecConan(ConanFile):
    name = "stdexec"
    version = "nvhpc-26.05"
    license = "Apache-2.0"
    homepage = "https://github.com/NVIDIA/stdexec"
    description = "NVIDIA's reference implementation of P2300 std::execution"
    package_type = "header-library"
    settings = "os", "compiler", "build_type", "arch"
    no_copy_source = True

    def source(self):
        get(self,
            url="https://github.com/NVIDIA/stdexec/archive/refs/tags/nvhpc-26.05.tar.gz",
            sha256="9d2396fecd604698c1eae58f0cb6e4517aa727013846240d1a7b2f35e49884dc",
            strip_root=True)

    def package(self):
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.cuh",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "LICENSE.txt",
             src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_id(self):
        self.info.clear()  # header-only: one package for every configuration

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        # Match the target name upstream's own CMake config exports, so
        # cpp/CMakeLists.txt reads the same either way.
        self.cpp_info.set_property("cmake_file_name", "stdexec")
        self.cpp_info.set_property("cmake_target_name", "STDEXEC::stdexec")
