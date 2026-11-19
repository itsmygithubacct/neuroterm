# Headless terminal core for programs that provide their own window and PTY.
NEUROTERM_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NEUROTERM_BUILD_DIR ?= build/neuroterm-terminal
NEUROTERM_VTERM := $(NEUROTERM_DIR)/third_party/libvterm
NEUROTERM_CPPFLAGS := -I$(NEUROTERM_DIR)/include -I$(NEUROTERM_VTERM)/include
NEUROTERM_VTERM_SRC := encoding keyboard mouse parser pen screen state unicode vterm
NEUROTERM_OBJECTS := $(NEUROTERM_BUILD_DIR)/terminal.o \
    $(NEUROTERM_VTERM_SRC:%=$(NEUROTERM_BUILD_DIR)/vt-%.o)

$(NEUROTERM_BUILD_DIR):
	mkdir -p $@

$(NEUROTERM_BUILD_DIR)/terminal.o: $(NEUROTERM_DIR)/terminal.c | $(NEUROTERM_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NEUROTERM_CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(NEUROTERM_BUILD_DIR)/vt-%.o: $(NEUROTERM_VTERM)/src/%.c | $(NEUROTERM_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NEUROTERM_CPPFLAGS) $(filter-out -Wall -Wextra -Wpedantic,$(CFLAGS)) -MMD -MP -c -o $@ $<

-include $(NEUROTERM_OBJECTS:.o=.d)
