cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1-controller)
# Pulls in the sample_usbd device_next bring-up helper (sample_usbd_init.c +
# its include dir) used for the USB-CDC console. Mirrors the looper.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
# M5.2: librarian.c (NVS profile store) + profile.c (validate/b64 codec — built
# into the Zephyr image now; librarian uses the struct, and the M5.x protocol
# task will use the codec).
# M6.2: config_cdc.c (CDC <-> protocol bridge + live monitor stream) +
# protocol.c (the host-tested JSON-lines config protocol). protocol.c's
# read/write paths exercise profile.c's b64 codec, pulling profile_to_b64/
# profile_from_b64 into the final image (they were gc-eliminated in M5.2).
target_sources(app PRIVATE src/main.c src/control_logic.c src/controls.c src/buttons.c src/mapping.c src/midi_out.c src/usbdev.c src/librarian.c src/profile.c src/protocol.c src/config_cdc.c src/config_cdc_fmt.c src/gesture.c)
