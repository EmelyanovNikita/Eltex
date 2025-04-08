# Makefile for pjsua_player_many_calls.c 

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra $(shell pkg-config --cflags libpjproject)
LDFLAGS = $(shell pkg-config --libs libpjproject)
TARGET = auto_answer
SRC = pjsua_player_many_calls.c

# Default target
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Clean build artifacts
clean:
	rm -f $(TARGET)

# Rebuild from scratch
rebuild: clean all

# Phony targets (not files)
.PHONY: all clean rebuild