set(SOURCES
    constids.inc
    spartan6.cc
)

add_nextpnr_himbaechel_microarchitecture(${uarch}
    CORE_SOURCES ${SOURCES}
)

# The chip databases are built from Silinx's device graphs (packages/silinx-fpga/devices/<part>.json.gz).
set(SILINX_DEVICES "" CACHE PATH "Silinx's packages/silinx-fpga/devices directory")
# the PIPs whose bits Silinx's bit database does not know, unknown-pips-<device>.json per device, and Silinx's delay
# model, timing-<device>.json per device (Silinx's research/nextpnr-s6/export-unknown-pips.mjs, export-timing.mjs)
set(SILINX_UNKNOWN_PIPS "" CACHE PATH "Directory of the unknown-pips-<device>.json files")
set(SILINX_TIMING "" CACHE PATH "Directory of the timing-<device>.json files")
if (NOT SILINX_DEVICES)
    message(FATAL_ERROR "Set SILINX_DEVICES to Silinx's packages/silinx-fpga/devices directory")
endif()

set(ALL_HIMBAECHEL_SPARTAN6_DEVICES xc6slx16:csg324 xc6slx45:csg324)
set(HIMBAECHEL_SPARTAN6_DEVICES xc6slx16:csg324 CACHE STRING
    "Include support for these Spartan-6 devices (available: ${ALL_HIMBAECHEL_SPARTAN6_DEVICES})")
message(STATUS "Enabled Himbaechel-Spartan6 devices: ${HIMBAECHEL_SPARTAN6_DEVICES}")

foreach (entry ${HIMBAECHEL_SPARTAN6_DEVICES})
    if (NOT entry IN_LIST ALL_HIMBAECHEL_SPARTAN6_DEVICES)
        message(FATAL_ERROR "Device ${entry} is not a supported Spartan-6 device")
    endif()
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 device)
    list(GET parts 1 package)

    add_bba_produce_command(
        TARGET  nextpnr-himbaechel-spartan6-bba
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/s6_gen.py
            ${SILINX_DEVICES}/${device}${package}-3.json.gz
            ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba.new
            $<$<BOOL:${SILINX_UNKNOWN_PIPS}>:--unknown-pips> $<$<BOOL:${SILINX_UNKNOWN_PIPS}>:${SILINX_UNKNOWN_PIPS}/unknown-pips-${device}.json>
            $<$<BOOL:${SILINX_TIMING}>:--timing> $<$<BOOL:${SILINX_TIMING}>:${SILINX_TIMING}/timing-${device}.json>
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba
        INPUTS
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/s6_gen.py
            ${CMAKE_CURRENT_SOURCE_DIR}/constids.inc
            ${SILINX_DEVICES}/${device}${package}-3.json.gz
    )

    add_bba_compile_command(
        TARGET  nextpnr-himbaechel-spartan6-chipdb
        OUTPUT  himbaechel/spartan6/chipdb-${device}.bin
        INPUT   ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba
        MODE    binary
    )
endforeach()
