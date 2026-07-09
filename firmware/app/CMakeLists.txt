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
# Phase 2b: mode.c is the pure device-mode decision unit (mode_decide / the LED
# pattern table). main.c's button-scan now calls mode_decide() to drive the
# on-device •• + FWD/RWD mode toggle (and the config_cdc unsolicited mode push),
# so it must be in the image — it was host-test-only until this wiring landed.
# Indicator-language rework: dial.c is the pure count-dial FSM + the count->track-
# row LED mapping (dial_track_pattern). main.c's •• handler now feeds dial_step()
# and renders dial_track_pattern() every tick, so it must be in the image — it was
# host-test-only (firmware/test) until this wiring landed.
# 0.9.1 LED redesign: side_led.c is the pure SIDE-row render (side_led_pattern =
# layer rest + temporary mode-flash). main.c's per-tick side-row block calls it
# every loop, so it must be in the image — it was host-test-only until this wiring.
# 0.9.3-beta stateful buttons: transport.c is the pure BTN_TRANSPORT decision
# (transport_rt -> MIDI system real-time Start/Stop/Continue). main.c's
# route_midi_button() now calls it on a transport-button press, so it must be in
# the image — it was host-test-only (firmware/test) until this wiring landed.
# Keyboard auto-repeat: kbd_hid.c is the pure Keyboard-mode HELD-key state +
# boot-keyboard report builder. main.c now latches press/release edges into it and
# sends usb_hid_send_report() so a held button holds its key (host OS auto-repeats),
# so it must be in the image — host-test-only (firmware/test) until this wiring.
target_sources(app PRIVATE src/main.c src/control_logic.c src/controls.c src/buttons.c src/mapping.c src/cc_value.c src/midi_out.c src/usbdev.c src/usb_midi1.c src/usb_hid.c src/kbd_hid.c src/midi1_codec.c src/transport.c src/librarian.c src/profile.c src/protocol.c src/config_cdc.c src/config_cdc_fmt.c src/gesture.c src/mode.c src/dial.c src/side_led.c src/chord_engine.c src/chord6.c src/led_override.c src/led.c)
