SRC=../app/src
CFLAGS=-Wall -Wextra -I$(SRC)
TESTS=test_control_logic test_mapping test_profile test_protocol test_buttons test_config_cdc test_gesture test_midi1_codec test_transport test_mode test_lib_header test_lib_bank test_dial test_led_dial test_side_led test_layer_bank test_kbd_hid test_chord_engine test_chord6 test_led_override test_nvs_erase test_btn_toggle test_cc_value
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
test_control_logic: test_control_logic.c $(SRC)/control_logic.c $(SRC)/control_logic.h
	$(CC) $(CFLAGS) -o $@ test_control_logic.c $(SRC)/control_logic.c
test_mapping: test_mapping.c $(SRC)/mapping.c $(SRC)/mapping.h $(SRC)/profile.h $(SRC)/chord6.c $(SRC)/chord6.h $(SRC)/cc_value.c $(SRC)/cc_value.h
	$(CC) $(CFLAGS) -o $@ test_mapping.c $(SRC)/mapping.c $(SRC)/chord6.c $(SRC)/cc_value.c
test_profile: test_profile.c $(SRC)/profile.c $(SRC)/profile.h $(SRC)/chord6.c $(SRC)/chord6.h
	$(CC) $(CFLAGS) -o $@ test_profile.c $(SRC)/profile.c $(SRC)/chord6.c
test_profile_h_dep: $(SRC)/profile.h
test_protocol: test_protocol.c $(SRC)/protocol.c $(SRC)/protocol.h $(SRC)/profile.h $(SRC)/led_override.c $(SRC)/led_override.h
	$(CC) $(CFLAGS) -o $@ test_protocol.c $(SRC)/protocol.c $(SRC)/profile.c $(SRC)/led_override.c
test_buttons: test_buttons.c $(SRC)/buttons.c $(SRC)/buttons.h
	$(CC) $(CFLAGS) -DBUTTONS_HOST_TEST -o $@ test_buttons.c $(SRC)/buttons.c
test_config_cdc: test_config_cdc.c $(SRC)/config_cdc_fmt.c $(SRC)/config_cdc.h $(SRC)/lib_header.h
	$(CC) $(CFLAGS) -o $@ test_config_cdc.c $(SRC)/config_cdc_fmt.c
test_gesture: test_gesture.c $(SRC)/gesture.c $(SRC)/gesture.h
	$(CC) $(CFLAGS) -o $@ test_gesture.c $(SRC)/gesture.c
test_dial: test_dial.c $(SRC)/dial.c $(SRC)/dial.h
	$(CC) $(CFLAGS) -o $@ test_dial.c $(SRC)/dial.c
test_led_dial: test_led_dial.c $(SRC)/dial.c $(SRC)/dial.h
	$(CC) $(CFLAGS) -o $@ test_led_dial.c $(SRC)/dial.c
test_mode: test_mode.c $(SRC)/mode.c $(SRC)/mode.h
	$(CC) $(CFLAGS) -o $@ test_mode.c $(SRC)/mode.c
test_side_led: test_side_led.c $(SRC)/side_led.c $(SRC)/side_led.h $(SRC)/mode.c $(SRC)/mode.h
	$(CC) $(CFLAGS) -o $@ test_side_led.c $(SRC)/side_led.c $(SRC)/mode.c
test_lib_header: test_lib_header.c $(SRC)/lib_header.h $(SRC)/lib_bank.h $(SRC)/seed_cadence.h $(SRC)/mode.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_lib_header.c
test_lib_bank: test_lib_bank.c $(SRC)/lib_bank.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_lib_bank.c
test_midi1_codec: test_midi1_codec.c $(SRC)/midi1_codec.c $(SRC)/midi1_codec.h
	$(CC) $(CFLAGS) -o $@ test_midi1_codec.c $(SRC)/midi1_codec.c
test_transport: test_transport.c $(SRC)/transport.c $(SRC)/transport.h
	$(CC) $(CFLAGS) -o $@ test_transport.c $(SRC)/transport.c
test_layer_bank: test_layer_bank.c $(SRC)/layer_takeover.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_layer_bank.c
test_kbd_hid: test_kbd_hid.c $(SRC)/kbd_hid.c $(SRC)/kbd_hid.h
	$(CC) $(CFLAGS) -o $@ test_kbd_hid.c $(SRC)/kbd_hid.c
test_chord_engine: test_chord_engine.c $(SRC)/chord_engine.c $(SRC)/chord_engine.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_chord_engine.c $(SRC)/chord_engine.c
test_chord6: test_chord6.c $(SRC)/chord6.c $(SRC)/chord6.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_chord6.c $(SRC)/chord6.c
test_led_override: test_led_override.c $(SRC)/led_override.c $(SRC)/led_override.h
	$(CC) $(CFLAGS) -o $@ test_led_override.c $(SRC)/led_override.c
test_btn_toggle: test_btn_toggle.c $(SRC)/btn_toggle.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_btn_toggle.c
test_cc_value: test_cc_value.c $(SRC)/cc_value.c $(SRC)/cc_value.h $(SRC)/profile.h $(SRC)/btn_toggle.h
	$(CC) $(CFLAGS) -o $@ test_cc_value.c $(SRC)/cc_value.c
test_nvs_erase: test_nvs_erase.c $(SRC)/nvs_erase.h
	$(CC) $(CFLAGS) -o $@ test_nvs_erase.c
.PHONY: test clean
clean: ; rm -f $(TESTS)
