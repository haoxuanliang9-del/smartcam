find_path(LIVE555_INCLUDE_DIR
    NAMES liveMedia.hh
    PATHS /usr/include /usr/local/include /usr/include/liveMedia
    PATH_SUFFIXES liveMedia
)

find_library(LIVE555_liveMedia_LIBRARY
    NAMES liveMedia
    PATHS /usr/lib /usr/local/lib
)

find_library(LIVE555_groupsock_LIBRARY
    NAMES groupsock
    PATHS /usr/lib /usr/local/lib
)

find_library(LIVE555_BasicUsageEnvironment_LIBRARY
    NAMES BasicUsageEnvironment
    PATHS /usr/lib /usr/local/lib
)

find_library(LIVE555_UsageEnvironment_LIBRARY
    NAMES UsageEnvironment
    PATHS /usr/lib /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Live555 DEFAULT_MSG
    LIVE555_INCLUDE_DIR
    LIVE555_liveMedia_LIBRARY
    LIVE555_groupsock_LIBRARY
    LIVE555_BasicUsageEnvironment_LIBRARY
    LIVE555_UsageEnvironment_LIBRARY
)

if(Live555_FOUND)
    set(LIVE555_INCLUDE_DIRS ${LIVE555_INCLUDE_DIR})
    set(LIVE555_LIBRARIES
        ${LIVE555_liveMedia_LIBRARY}
        ${LIVE555_groupsock_LIBRARY}
        ${LIVE555_BasicUsageEnvironment_LIBRARY}
        ${LIVE555_UsageEnvironment_LIBRARY}
    )
endif()

mark_as_advanced(
    LIVE555_INCLUDE_DIR
    LIVE555_liveMedia_LIBRARY
    LIVE555_groupsock_LIBRARY
    LIVE555_BasicUsageEnvironment_LIBRARY
    LIVE555_UsageEnvironment_LIBRARY
)
