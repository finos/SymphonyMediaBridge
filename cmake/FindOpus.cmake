include (FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_Opus QUIET Opus)

find_path(Opus_INCLUDE_DIR
    NAMES opus/opus.h
    HINTS ${PC_Opus_INCLUDE_DIRS}
)

mark_as_advanced(Opus_INCLUDE_DIR)

find_library(Opus_LIBRARY
    NAMES opus
    HINTS ${PC_Opus_LIBRARY_DIRS}
)

mark_as_advanced(Opus_LIBRARY)

find_package_handle_standard_args(Opus
    REQUIRED_VARS Opus_LIBRARY Opus_INCLUDE_DIR
    HANDLE_COMPONENTS
    FAIL_MESSAGE "Opus was not find installed in your system"
)

if(Opus_FOUND)
    set(Opus_LIBRARIES ${Opus_LIBRARY})
    set(Opus_INCLUDE_DIRS ${Opus_INCLUDE_DIR})
    set(Opus_DEFINITIONS ${PC_Opus_CFLAGS_OTHER})

    if(NOT TARGET Opus::Opus)
        add_library(Opus::Opus UNKNOWN IMPORTED)
        set_target_properties(Opus::Opus PROPERTIES
            IMPORTED_LOCATION "${Opus_LIBRARY}"
            INTERFACE_COMPILE_OPTIONS "${PC_Opus_CFLAGS_OTHER}"
            INTERFACE_INCLUDE_DIRECTORIES "${Opus_INCLUDE_DIR}"
        )
    endif()
endif()