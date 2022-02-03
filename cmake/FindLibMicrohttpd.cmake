
include (FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_LibMicrohttpd QUIET libmicrohttpd)

find_path(LibMicrohttpd_INCLUDE_DIR
    NAMES microhttpd.h
    HINTS ${PC_LibMicrohttpd_INCLUDE_DIRS}
)

mark_as_advanced(LibMicrohttpd_INCLUDE_DIR)

find_library(LibMicrohttpd_LIBRARY
    NAMES microhttpd
    HINTS ${PC_LibMicrohttpd_LIBRARY_DIRS}
)

mark_as_advanced(LibMicrohttpd_LIBRARY)

find_package_handle_standard_args(LibMicrohttpd
    REQUIRED_VARS LibMicrohttpd_LIBRARY LibMicrohttpd_INCLUDE_DIR
    HANDLE_COMPONENTS
    FAIL_MESSAGE "LibMictrohttpd was not find installed in your system."
)

if(LibMicrohttpd_FOUND)
    set(LibMicrohttpd_LIBRARIES ${LibMicrohttpd_LIBRARY})
    set(LibMicrohttpd_INCLUDE_DIRS ${LibMicrohttpd_INCLUDE_DIR})
    set(LibMicrohttpd_DEFINITIONS ${PC_LibMicrohttpd_CFLAGS_OTHER})

    if(NOT TARGET LibMicrohttpd::Microhttpd)
        add_library(LibMicrohttpd::Microhttpd UNKNOWN IMPORTED)
        set_target_properties(LibMicrohttpd::Microhttpd PROPERTIES
            IMPORTED_LOCATION "${LibMicrohttpd_LIBRARY}"
            INTERFACE_COMPILE_OPTIONS "${PC_LibMicrohttpd_CFLAGS_OTHER}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibMicrohttpd_INCLUDE_DIR}"
        )
    endif()
endif()