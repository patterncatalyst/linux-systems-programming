"""Conan 2 consumer for ch56's opt-in P2300 arm.

Deliberately its own conanfile, separate from the default cpp/ build -- exactly
as ch46 did. `./demo.sh cpp build` and the `release` preset never evaluate this
file, so the six-model core needs no Conan, no network, and no third-party
package at all, and fedora:44 CI (which has no Conan) stays green.

The dependency is ch56's own `stdexec` recipe in conan/recipe/, which must be
exported into the local cache first:

    conan export cpp/conan/recipe
    conan install cpp/conan --output-folder=cpp/build/conan --build=missing \\
        --lockfile=cpp/conan/conan.lock
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class CapstoneSendersDemo(ConanFile):
    name = "capstone-senders-demo"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("stdexec/nvhpc-26.05")

    def layout(self):
        # Generated files land in build/conan/generators/ (relative to the
        # --output-folder passed on the CLI), matching the "conan" preset's
        # CMAKE_TOOLCHAIN_FILE path.
        self.folders.generators = "generators"

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
