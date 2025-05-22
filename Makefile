# Makefile for building tyr
# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -DLF_RUNARA -DLF_X11
LDFLAGS = -lpodvig -Lvendor/reif/lib -lleif -lrunara -lGL -lX11 -lfontconfig -lfreetype -lharfbuzz -lm -lXrender -lglfw

# Directories and files
SRC_DIR = src
BIN_DIR = bin
TARGET = $(BIN_DIR)/tyr
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
	install -Dm755 $(TARGET) $(INSTALL_PATH)/tyr
	@echo "Installed to $(INSTALL_PATH)/tyr"

# Clean rule
clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean install

