"""Conan 2 consumer for the isolated toolbox-conan-demo sub-target (ch46
"Conan 2 lockfiles" section). Pulls exactly one small dependency (fmt) via
CMakeDeps + CMakeToolchain. Deliberately its own conanfile, separate from
the default cpp/ build -- ./demo.sh cpp build and the release preset never
evaluate this, so fedora:44 CI build-smoke (no Conan installed) stays green.
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class ToolboxConanDemo(ConanFile):
    name = "toolbox-conan-demo"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("fmt/11.0.2")

    def layout(self):
        # Generated files land in build/conan/generators/ (relative to the
        # --output-folder passed on the CLI), matching the "conan"
        # CMakePresets.json preset's CMAKE_TOOLCHAIN_FILE path.
        self.folders.generators = "generators"

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
