import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir

required_conan_version = ">=2.0.9"


class DataPathConan(ConanFile):
    name = "com.xaymar.datapath"
    description = "High performance, low latency IPC library (named pipes, Unix domain sockets, shared memory)"
    license = "BSD-3-Clause"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/Xaymar/datapath"
    topics = ("ipc", "low-latency", "named-pipes", "unix-sockets", "shared-memory")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def validate(self):
        check_min_cppstd(self, 11)
        if self.settings.os not in ("Windows", "Linux", "Android", "Macos", "iOS"):
            raise ConanInvalidConfiguration(f"{self.ref} does not support {self.settings.os}")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["DATAPATH_SHARED"] = bool(self.options.shared)
        tc.cache_variables["DATAPATH_BUILD_TESTS"] = False
        tc.cache_variables["DATAPATH_BUILD_SAMPLES"] = False
        if self.settings.get_safe("compiler.cppstd"):
            tc.cache_variables["DATAPATH_CXX_STANDARD"] = str(self.settings.compiler.cppstd).replace("gnu", "")
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "datapath")
        self.cpp_info.set_property("cmake_target_name", "datapath::datapath")
        self.cpp_info.set_property("pkg_config_name", "datapath")
        self.cpp_info.libs = ["datapath"]
        if self.settings.os in ("Linux", "FreeBSD", "Android"):
            self.cpp_info.system_libs = ["pthread"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["advapi32", "kernel32"]
