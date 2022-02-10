include (FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_Srtp2 QUIET Srtp2)

find_path(Srtp2_INCLUDE_DIR
    NAMES srtp2/srtp.h
    HINTS ${PC_Srtp2_INCLUDE_DIRS}
)

mark_as_advanced(Srtp2_INCLUDE_DIR)

find_library(Srtp2_LIBRARY
    NAMES srtp2
    HINTS ${PC_Srtp2_LIBRARY_DIRS}
)

mark_as_advanced(Srtp2_LIBRARY)

find_package_handle_standard_args(Srtp2
    REQUIRED_VARS Srtp2_LIBRARY Srtp2_INCLUDE_DIR
    HANDLE_COMPONENTS
    FAIL_MESSAGE "Srt2 was not find installed in your system"
)

if(Srtp2_FOUND)
    set(Srtp2_LIBRARIES ${Srtp2_LIBRARY})
    set(Srtp2_INCLUDE_DIRS ${Srtp2_INCLUDE_DIR})
    set(Srtp2_DEFINITIONS ${PC_Srtp2_CFLAGS_OTHER})

    if(NOT TARGET Srtp2::Srtp)
        add_library(Srtp2::Srtp UNKNOWN IMPORTED)
        set_target_properties(Srtp2::Srtp PROPERTIES
            IMPORTED_LOCATION "${Srtp2_LIBRARY}"
            INTERFACE_COMPILE_OPTIONS "${PC_Srtp2_CFLAGS_OTHER}"
            INTERFACE_INCLUDE_DIRECTORIES "${Srtp2_INCLUDE_DIR}"
        )
    endif()
endif()