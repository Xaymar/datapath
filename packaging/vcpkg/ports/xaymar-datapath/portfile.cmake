vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Xaymar/datapath
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF root
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" DATAPATH_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DDATAPATH_SHARED=${DATAPATH_SHARED}
        -DDATAPATH_BUILD_TESTS=OFF
        -DDATAPATH_BUILD_SAMPLES=OFF
        -DDATAPATH_BUILD_PYTHON=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME datapath CONFIG_PATH lib/cmake/datapath)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/licenses"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
