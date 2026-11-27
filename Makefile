# === Project layout =========================================================
SRC_DIR    := srcs
OBJ_DIR    := asms
BIN_DIR    := bins
INC_DIR    := include

CONCORD_DIR := concord

TARGET := $(BIN_DIR)/cfbot

# === Sources / objects =======================================================
# Recursive: picks up main.c at the top level and every *.c under srcs/<category>/
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Every immediate subfolder of include/ (core, useful, mod, roleutil, customcom,
# misc, utils, ...) gets its own -I so quoted #includes resolve regardless of
# which category a header lives under.
INC_SUBDIRS := $(wildcard $(INC_DIR)/*/)

# === Toolchain ================================================================
CC      := cc
CFLAGS  := -g -Wall -Wextra -std=gnu11 \
           -I$(INC_DIR) $(addprefix -I,$(INC_SUBDIRS)) -I$(CONCORD_DIR)/include
CPPFLAGS:= -MMD -MP

# Concord itself needs pthread + curl to link.
LDFLAGS := -L$(CONCORD_DIR)/lib
LDLIBS  := -ldiscord -lsqlite3 -lpthread -lcurl -lm

# === Rules ====================================================================
.PHONY: all clean fclean re test

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Uses real Concord headers, an in-memory SQLite DB, and stubbed Discord I/O.
# No bot token or Discord connection is required.
test: $(BIN_DIR)/test_msglimit
	./$(BIN_DIR)/test_msglimit

$(BIN_DIR)/test_msglimit: tests/test_msglimit.c srcs/mod/msglimit.c \
                        include/mod/msglimit.h include/utils/utils.h \
                        include/customcom/customcom.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_msglimit.c srcs/mod/msglimit.c \
		-o $@ -lsqlite3

clean:
	rm -rf $(OBJ_DIR)

fclean: clean
	rm -rf $(BIN_DIR)

release-static: CFLAGS := -O2 -DNDEBUG -static -I$(INC_DIR) -I$(CONCORD_DIR)/include
release-static: LDFLAGS := -L$(CONCORD_DIR)/lib -static
release-static: LDLIBS := -ldiscord -lcurl -lsqlite3 -lssl -lcrypto -lpthread -lz -lm
release-static: $(TARGET)
	strip $(TARGET)

re: fclean all

-include $(DEPS)
