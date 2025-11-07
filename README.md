# Distributed File System

A multi-threaded distributed file system implementation in C featuring a Name Server, multiple Storage Servers, and a Client interface with support for concurrent file operations, hierarchical folders, checkpoints, and access control.

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [System Architecture](#system-architecture)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [How to Run](#how-to-run)
- [Usage Guide](#usage-guide)
- [Commands Reference](#commands-reference)
- [Project Structure](#project-structure)
- [Assumptions](#assumptions)
- [Error Codes](#error-codes)
- [Logging](#logging)
- [Troubleshooting](#troubleshooting)

## Overview

This distributed file system implements a client-server architecture where:
- **Name Server (NM)**: Central coordinator managing file metadata, user access control, and storage server discovery
- **Storage Servers (SS)**: Multiple servers providing actual file storage with sentence-level locking and undo functionality
- **Clients**: Interactive interface for users to perform file operations

## Features

### Core Features
- ✅ **File Operations**: CREATE, READ, WRITE, DELETE
- ✅ **Access Control**: User-based read/write permissions
- ✅ **Concurrent Access**: Sentence-level locking for fine-grained concurrency
- ✅ **File Streaming**: Real-time file content streaming
- ✅ **Undo Functionality**: Revert file changes
- ✅ **Multi-threaded**: Handles multiple clients simultaneously
- ✅ **Fault Tolerance**: Heartbeat mechanism and server health monitoring

### Bonus Features
- 📁 **Hierarchical Folders**: Organize files in folders with move operations
- 💾 **Checkpoints**: Create, view, and revert to file checkpoints with tags
- 🔐 **Access Requests**: Request and approve/deny access permissions
-  **Replication**: Automatic file replication for fault tolerance

## System Architecture

```
┌─────────────┐
│   Client    │────┐
└─────────────┘    │
                   │
┌─────────────┐    │    ┌──────────────┐
│   Client    │────┼───▶│ Name Server  │
└─────────────┘    │    │   (Port 8080)│
                   │    └──────────────┘
┌─────────────┐    │           │
│   Client    │────┘           │ Coordinates
└─────────────┘                │
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
      │Storage Server│ │Storage Server│ │Storage Server│
      │  (Port 9000) │ │  (Port 9002) │ │  (Port 9004) │
      └──────────────┘ └──────────────┘ └──────────────┘
```

## Prerequisites

- **Operating System**: Linux/Unix-based system
- **Compiler**: GCC with pthread support
- **Libraries**: 
  - pthread (POSIX threads)
  - Standard C libraries (socket, network, file I/O)

## Installation

1. **Clone the repository**:
   ```bash
   git clone <repository-url>
   cd course-project-cheetah702
   ```

2. **Build the project**:
   ```bash
   make
   ```
   
   This will compile all three components:
   - `name_server`
   - `storage_server`
   - `client`

3. **Clean build artifacts** (if needed):
   ```bash
   make clean        # Remove executables and object files
   make clean-all    # Remove all generated files including logs
   ```

## How to Run

### Step 1: Start the Name Server

Open a terminal and run:
```bash
./name_server
```

The Name Server will start on **port 8080** and wait for storage servers and clients to connect.

**Expected Output**:
```
Name Server starting on port 8080...
✓ Name Server is ready
Waiting for Storage Servers and Clients...
```

### Step 2: Start Storage Server(s)

Open **new terminal(s)** for each storage server. You need at least one storage server, but can run multiple.

**Important**: Storage servers require you to specify the Name Server IP address so devices on the same network can connect.

**First Storage Server**:
```bash
./storage_server 9000 9001 <name_server_ip>
```

**Example for local testing**:
```bash
./storage_server 9000 9001 127.0.0.1
```

**Example for network access** (devices on same network):
```bash
./storage_server 9000 9001 10.42.0.238
```

**Parameters**:
- Port 9000: Client connection port
- Port 9001: Name Server communication port
- 127.0.0.1 or 10.42.0.238: Name Server IP address

**Second Storage Server** (optional):
```bash
./storage_server 9002 9003 <name_server_ip>
```

**Third Storage Server** (optional):
```bash
./storage_server 9004 9005 <name_server_ip>
```

**Expected Output**:
```
Storage Server starting...
Client Port: 9000, NM Port: 9001
✓ Registered with Name Server
✓ Storage Server ready
```

### Step 3: Start Client(s)

Open **new terminal(s)** for each client:

**Important**: Clients require you to specify the Name Server IP address.

```bash
./client <name_server_ip>
```

**Example for local testing**:
```bash
./client 127.0.0.1
```

**Example for network access**:
```bash
./client 10.42.0.238
```

You'll be prompted to enter your **Username** (e.g., `alice`, `bob`)

**Expected Output**:
```
Connecting to Name Server at: 127.0.0.1:8080
Enter username: alice
✓ Connected to Name Server
✓ Registered as user: alice
```

### Quick Start with Makefile

Alternatively, use the provided Makefile targets:

**Note**: For network access, you'll need to manually run the commands with IP addresses as shown above. The Makefile targets are primarily for local testing.

```bash
# Terminal 1: Start Name Server
make run-nm

# Terminal 2: Start Storage Server (local testing only)
make run-ss PORT1=9000 PORT2=9001

# Terminal 3: Start Client (local testing only)
make run-client
```

## Usage Guide

### Basic Workflow

1. **Create a file**:
   ```
   CREATE myfile.txt
   ```

2. **Write content** (sentence by sentence):
   ```
   WRITE myfile.txt 0
   [Enter your sentence and press Enter]
   ```

3. **Read file**:
   ```
   READ myfile.txt
   ```

4. **View all files**:
   ```
   VIEW -al
   ```

5. **Grant access to another user**:
   ```
   ADDACCESS -R myfile.txt bob
   ```

### Working with Folders

```bash
# Create a folder
CREATEFOLDER documents

# Move file to folder
MOVE myfile.txt documents

# View folder contents
VIEWFOLDER documents
```

### Using Checkpoints

```bash
# Create a checkpoint
CHECKPOINT myfile.txt v1

# View checkpoint
VIEWCHECKPOINT myfile.txt v1

# Revert to checkpoint
REVERT myfile.txt v1

# List all checkpoints
LISTCHECKPOINTS myfile.txt
```

## Commands Reference

### File Operations
| Command | Syntax | Description |
|---------|--------|-------------|
| CREATE | `CREATE <filename>` | Create a new file |
| READ | `READ <filename>` | Read and display file content |
| WRITE | `WRITE <filename> <sentence#>` | Write/edit a specific sentence |
| DELETE | `DELETE <filename>` | Delete a file |
| INFO | `INFO <filename>` | Display file metadata |
| STREAM | `STREAM <filename>` | Stream file content in real-time |
| UNDO | `UNDO <filename>` | Undo last modification |
| EXEC | `EXEC <filename>` | Execute file as command script |

### View Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| VIEW | `VIEW` | List your files |
| VIEW | `VIEW -a` | List all users' files |
| VIEW | `VIEW -l` | List with detailed info |
| VIEW | `VIEW -al` | List all files with details |

### Access Control
| Command | Syntax | Description |
|---------|--------|-------------|
| LIST | `LIST` | List all registered users |
| ADDACCESS | `ADDACCESS -R <file> <user>` | Grant read access |
| ADDACCESS | `ADDACCESS -W <file> <user>` | Grant write access |
| REMACCESS | `REMACCESS <file> <user>` | Remove user access |
| REQUESTACCESS | `REQUESTACCESS -R <file>` | Request read access |
| REQUESTACCESS | `REQUESTACCESS -W <file>` | Request write access |
| VIEWREQUESTS | `VIEWREQUESTS` | View pending access requests (owner only) |
| APPROVE | `APPROVE <user> <file>` | Approve access request |
| DENY | `DENY <user> <file>` | Deny access request |

### Folder Operations
| Command | Syntax | Description |
|---------|--------|-------------|
| CREATEFOLDER | `CREATEFOLDER <foldername>` | Create a new folder |
| MOVE | `MOVE <filename> <foldername>` | Move file to folder |
| VIEWFOLDER | `VIEWFOLDER <foldername>` | List files in folder |

### Checkpoint Operations
| Command | Syntax | Description |
|---------|--------|-------------|
| CHECKPOINT | `CHECKPOINT <file> <tag>` | Create a checkpoint |
| VIEWCHECKPOINT | `VIEWCHECKPOINT <file> <tag>` | View checkpoint content |
| REVERT | `REVERT <file> <tag>` | Revert to checkpoint |
| LISTCHECKPOINTS | `LISTCHECKPOINTS <file>` | List all checkpoints for file |

### System Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| HELP | `HELP` | Display command menu |
| EXIT | `EXIT` | Disconnect from server |

## Project Structure

```
course-project-cheetah702/
├── name_server.c          # Name Server implementation
├── storage_server.c       # Storage Server implementation
├── client.c               # Client interface implementation
├── common.c               # Shared utility functions
├── common.h               # Common data structures and definitions
├── bonus_handlers.c       # Bonus feature implementations
├── Makefile               # Build configuration
└── README.md              # This file

Generated at runtime:
├── name_server            # Name Server executable
├── storage_server         # Storage Server executable
├── client                 # Client executable
├── nm_log.txt             # Name Server logs
├── nm_metadata.dat        # Persistent metadata
├── ss_log*.txt            # Storage Server logs
├── storage*/              # Storage directories for each SS
└── undo*/                 # Undo directories for each SS
```

## Assumptions

1. **Network Configuration**:
   - All components run on localhost (127.0.0.1) by default
   - Name Server always runs on port 8080
   - Storage Servers use configurable ports (default: 9000, 9002, 9004...)
   - No firewall restrictions on local ports

2. **Storage**:
   - Files are text-based
   - Maximum file size: 64KB (MAX_BUFFER_SIZE)
   - Maximum filename length: 256 characters
   - Sentence delimiter: newline character (`\n`)
   - Each Storage Server creates its own `storage<port>` and `undo<port>` directories

3. **Concurrency**:
   - Multiple clients can read the same file simultaneously
   - Only one client can write to a specific sentence at a time
   - Write locks are released after operation completion
   - No deadlock prevention mechanism (assumes well-behaved clients)

4. **User Management**:
   - Usernames are unique and case-sensitive
   - No authentication/password mechanism
   - File owner has full control over access permissions
   - No user registration process (first connection registers user)

5. **File System**:
   - Hierarchical folder structure with `/` as path separator
   - No circular folder references
   - Folders don't occupy storage space
   - Moving files doesn't change file ownership

6. **Fault Tolerance**:
   - Heartbeat interval: 5 seconds
   - Server timeout: 15 seconds (3 missed heartbeats)
   - Automatic failover to replica servers
   - No automatic recovery of failed servers

7. **Checkpoints**:
   - Maximum 100 checkpoints per file
   - Checkpoint tags must be unique per file
   - Checkpoints stored in Name Server memory (not persistent)

8. **Limitations**:
   - No encryption for data transmission
   - No data compression
   - Single Name Server (no HA/replication for NM)
   - Limited to MAX_CLIENTS (100) concurrent clients
   - Limited to MAX_STORAGE_SERVERS (50) storage servers

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | ERR_SUCCESS | Operation completed successfully |
| 1 | ERR_FILE_NOT_FOUND | Requested file doesn't exist |
| 2 | ERR_UNAUTHORIZED | Insufficient permissions |
| 3 | ERR_FILE_EXISTS | File already exists |
| 4 | ERR_INVALID_INDEX | Invalid sentence index |
| 5 | ERR_SENTENCE_LOCKED | Sentence is locked by another user |
| 6 | ERR_NO_STORAGE_SERVER | No available storage server |
| 7 | ERR_CONNECTION_FAILED | Cannot connect to server |
| 8 | ERR_INVALID_COMMAND | Unrecognized command |
| 9 | ERR_SERVER_ERROR | Internal server error |
| 10 | ERR_NO_UNDO_AVAILABLE | No undo history available |

## Logging

### Name Server Logs (`nm_log.txt`)
- Client connections/disconnections
- Storage Server registrations
- File operations (create, delete, access changes)
- Error conditions

### Storage Server Logs (`ss_log_<port>.txt`)
- File read/write operations
- Lock acquisitions/releases
- Replication events
- Heartbeat status

**Viewing Logs**:
```bash
# View Name Server logs
tail -f nm_log.txt

# View Storage Server logs
tail -f ss_log_9000.txt
```

## Troubleshooting

### Issue: "Cannot connect to Name Server"
**Solution**: Ensure Name Server is running first
```bash
# Check if Name Server is running
ps aux | grep name_server
# Kill if needed and restart
killall name_server
./name_server
```

### Issue: "Address already in use"
**Solution**: Port is occupied by previous instance
```bash
# Find process using port
lsof -i :8080
# Kill the process
kill -9 <PID>
```

### Issue: "No Storage Server available"
**Solution**: Start at least one Storage Server
```bash
./storage_server 9000 9001
```

### Issue: "Permission denied" errors
**Solution**: Check file access rights with INFO command
```bash
INFO <filename>
```
Owner can grant access using:
```bash
ADDACCESS -R <filename> <username>
```

### Issue: Storage directories not created
**Solution**: Create directories manually
```bash
make dirs
# Or manually:
mkdir -p storage undo
mkdir -p storage9000 undo9000
```

### Complete Reset
To completely reset the system:
```bash
make clean-all
killall name_server storage_server client
rm -rf storage* undo* checkpoints/
```

## Development

### Adding New Commands
1. Define message type in `common.h` (MSG_* constants)
2. Implement handler in appropriate server file
3. Add client command in `client.c`
4. Update this README

### Building Individual Components
```bash
make name_server      # Build only Name Server
make storage_server   # Build only Storage Server
make client          # Build only Client
```

### Debug Build
```bash
# The Makefile includes -g flag by default for debugging
gdb ./name_server
gdb ./storage_server
gdb ./client
```

---

**Author**: Cheetah702  
**Course**: CS3-OSN (Operating Systems and Networks)  
**Institution**: Monsoon 2025  

For issues or questions, please refer to the course forum or contact the development team.
