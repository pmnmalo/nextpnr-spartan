set(SOURCES
    constids.inc
    spartan3e.cc
)

add_nextpnr_himbaechel_microarchitecture(${uarch}
    CORE_SOURCES ${SOURCES}
)

# The chip databases are built from Silinx's device graphs (packages/silinx-fpga/devices/<part>.json.gz).
set(SILINX_DEVICES "" CACHE PATH "Silinx's packages/silinx-fpga/devices directory")
# the PIPs whose bits Silinx's bit database does not know, unknown-pips-<device>.json per device
# (Silinx's research/nextpnr-s3e/export-unknown-pips.mjs)
set(SILINX_UNKNOWN_PIPS "" CACHE PATH "Directory of the unknown-pips-<device>.json files")
if (NOT SILINX_DEVICES)
    message(FATAL_ERROR "Set SILINX_DEVICES to Silinx's packages/silinx-fpga/devices directory")
endif()

set(ALL_HIMBAECHEL_SPARTAN3E_DEVICES xc3s100e:cp132 xc3s250e:cp132 xc3s500e:fg320 xc3s1200e:fg320)
set(HIMBAECHEL_SPARTAN3E_DEVICES xc3s250e:cp132 CACHE STRING
    "Include support for these Spartan-3E devices (available: ${ALL_HIMBAECHEL_SPARTAN3E_DEVICES})")
message(STATUS "Enabled Himbaechel-Spartan3E devices: ${HIMBAECHEL_SPARTAN3E_DEVICES}")

foreach (entry ${HIMBAECHEL_SPARTAN3E_DEVICES})
    if (NOT entry IN_LIST ALL_HIMBAECHEL_SPARTAN3E_DEVICES)
        message(FATAL_ERROR "Device ${entry} is not a supported Spartan-3E device")
    endif()
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 device)
    list(GET parts 1 package)

    add_bba_produce_command(
        TARGET  nextpnr-himbaechel-spartan3e-bba
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/s3e_gen.py
            ${SILINX_DEVICES}/${device}${package}-4.json.gz
            ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba.new
            --sites ${CMAKE_CURRENT_BINARY_DIR}/sites-${device}.json
            $<$<BOOL:${SILINX_UNKNOWN_PIPS}>:--unknown-pips> $<$<BOOL:${SILINX_UNKNOWN_PIPS}>:${SILINX_UNKNOWN_PIPS}/unknown-pips-${device}.json>
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba
        INPUTS
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/s3e_gen.py
            ${CMAKE_CURRENT_SOURCE_DIR}/constids.inc
            ${SILINX_DEVICES}/${device}${package}-4.json.gz
    )

    add_bba_compile_command(
        TARGET  nextpnr-himbaechel-spartan3e-chipdb
        OUTPUT  himbaechel/spartan3e/chipdb-${device}.bin
        INPUT   ${CMAKE_CURRENT_BINARY_DIR}/chipdb-${device}.bba
        MODE    binary
    )
endforeach()
