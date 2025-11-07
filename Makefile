# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

# Targets
TARGETS = name_server storage_server client

# Source files
COMMON_SRC = common.c
NM_SRC = name_server.c
SS_SRC = storage_server.c
CLIENT_SRC = client.c

# Object files
COMMON_OBJ = $(COMMON_SRC:.c=.o)
NM_OBJ = $(NM_SRC:.c=.o)
SS_OBJ = $(SS_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Default target
all: $(TARGETS)

# Build Name Server
name_server: $(NM_OBJ) $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Name Server built successfully"

# Build Storage Server
storage_server: $(SS_OBJ) $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Storage Server built successfully"

# Build Client
client: $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Client built successfully"

# Compile object files
%.o: %.c common.h
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(TARGETS) *.o
	rm -f nm_log.txt ss_log*.txt nm_metadata.dat
	rm -rf storage undo checkpoints
	rm -rf storage[0-9]* undo[0-9]*
	@echo "✓ Cleaned build artifacts and storage directories"

# Clean everything including logs and data
clean-all: clean
	rm -f *.log *.dat
	@echo "✓ Cleaned all files"

# Create necessary directories
dirs:
	mkdir -p storage undo
	@echo "✓ Created necessary directories"

# Run Name Server
run-nm: name_server dirs
	./name_server

# Run Storage Server (with optional port arguments)
run-ss: storage_server dirs
	./storage_server $(PORT1) $(PORT2)

# Run Client
run-client: client
	./client

# Help
help:
	@echo "Distributed File System - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build all components"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make clean-all    - Remove all generated files"
	@echo "  make run-nm       - Run Name Server"
	@echo "  make run-ss       - Run Storage Server"
	@echo "  make run-client   - Run Client"
	@echo ""
	@echo "Examples:"
	@echo "  make run-ss PORT1=9000 PORT2=9001"
	@echo ""

.PHONY: all clean clean-all dirs run-nm run-ss run-client help
