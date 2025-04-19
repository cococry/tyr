# Makefile for building twr
# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -DLF_RUNARA -DLF_GLFW
LDFLAGS = -lpodvig -Lvendor/reif/lib -lleif -lrunara -lGL -lX11 -lfontconfig -lfreetype -lharfbuzz -lm -lXrender -lglfw

# Directories and files
SRC_DIR = src
BIN_DIR = bin
TARGET = $(BIN_DIR)/twr
SRC = $(wildcard $(SRC_DIR)/*.c)
INSTALL_PATH = /usr/bin

# Default target
all: $(TARGET)

# Build rule
$(TARGET): $(SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Install rule
install: $(TARGET)
	install -Dm755 $(TARGET) $(INSTALL_PATH)/twr
	@echo "Installed to $(INSTALL_PATH)/twr"

# Clean rule
clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean install

