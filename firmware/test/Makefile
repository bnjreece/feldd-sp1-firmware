SRC=../app/src
CFLAGS=-Wall -Wextra -I$(SRC)
TESTS=test_control_logic test_mapping test_profile test_protocol test_buttons test_config_cdc test_midi_out test_gesture
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
test_control_logic: test_control_logic.c $(SRC)/control_logic.c $(SRC)/control_logic.h
	$(CC) $(CFLAGS) -o $@ test_control_logic.c $(SRC)/control_logic.c
test_mapping: test_mapping.c $(SRC)/mapping.c $(SRC)/mapping.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_mapping.c $(SRC)/mapping.c
test_profile: test_profile.c $(SRC)/profile.c $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_profile.c $(SRC)/profile.c
test_profile_h_dep: $(SRC)/profile.h
test_protocol: test_protocol.c $(SRC)/protocol.c $(SRC)/protocol.h $(SRC)/profile.h
	$(CC) $(CFLAGS) -o $@ test_protocol.c $(SRC)/protocol.c $(SRC)/profile.c
test_buttons: test_buttons.c $(SRC)/buttons.c $(SRC)/buttons.h
	$(CC) $(CFLAGS) -DBUTTONS_HOST_TEST -o $@ test_buttons.c $(SRC)/buttons.c
test_config_cdc: test_config_cdc.c $(SRC)/config_cdc_fmt.c $(SRC)/config_cdc.h
	$(CC) $(CFLAGS) -o $@ test_config_cdc.c $(SRC)/config_cdc_fmt.c
test_midi_out: test_midi_out.c $(SRC)/midi_out.c $(SRC)/midi_out.h $(SRC)/mapping.h
	$(CC) $(CFLAGS) -DMIDI_OUT_HOST_TEST -o $@ test_midi_out.c $(SRC)/midi_out.c
test_gesture: test_gesture.c $(SRC)/gesture.c $(SRC)/gesture.h
	$(CC) $(CFLAGS) -o $@ test_gesture.c $(SRC)/gesture.c
.PHONY: test clean
clean: ; rm -f $(TESTS)
