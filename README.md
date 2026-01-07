# Distributed File Sharing System

[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux-orange.svg)](https://www.linux.org/)

A peer-to-peer distributed file sharing system built with C++ that enables users to share and download files within groups. Features parallel downloading from multiple peers, piece-wise file transfer using BitTorrent-like protocols, and SHA1 hash verification for file integrity.

## Features

### Tracker
- **User Management**: Create accounts, login/logout functionality with authentication
- **Group Management**: Create groups, join/leave groups, manage join requests
- **File Tracking**: Track which files are shared in each group and which users are seeding
- **Multi-Tracker Synchronization**: Two trackers stay synchronized with each other
- **Logging**: All operations are logged to a tracker log file

### Client
- **User Authentication**: Secure login with password verification
- **Group Operations**: Create, join, leave groups; manage group membership
- **File Sharing**: Share files within groups you're a member of
- **Parallel Downloading**: Download pieces from multiple peers simultaneously
- **Piece Selection Algorithm**: Rarest-first piece selection for efficient distribution
- **Leeching Support**: Share pieces as soon as they're downloaded
- **File Integrity**: SHA1 hash verification for complete file and individual pieces
- **Concurrent Downloads**: Download multiple files simultaneously
- **Automatic Re-sharing**: Previously shared files are automatically shared on login

## Architecture

```
┌─────────────┐     ┌─────────────┐
│  Tracker 1  │◄───►│  Tracker 2  │
└──────┬──────┘     └──────┬──────┘
       │                   │
       ▼                   ▼
┌─────────────────────────────────┐
│           Clients               │
│  ┌─────┐  ┌─────┐  ┌─────┐     │
│  │Peer1│◄►│Peer2│◄►│Peer3│     │
│  └─────┘  └─────┘  └─────┘     │
└─────────────────────────────────┘
```

## Prerequisites

- Linux OS
- g++ with C++17 support
- OpenSSL development libraries (`libssl-dev`)
- pthread library

### Install Dependencies (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install g++ libssl-dev
```

## Compilation

### Build Tracker

```bash
cd tracker
make clean && make
```

### Build Client

```bash
cd client
make clean && make
```

## Execution

### Start Tracker

```bash
cd tracker
./tracker tracker_info.txt <tracker_number>
```

Where `tracker_number` is 1 or 2 (corresponding to the line number in tracker_info.txt).

Example:
```bash
./tracker tracker_info.txt 1  # Start tracker 1 on port 5000
./tracker tracker_info.txt 2  # Start tracker 2 on port 5001
```

To stop the tracker, type `quit` in the tracker terminal.

### Start Client

```bash
cd client
./client <IP>:<PORT> tracker_info.txt
```

Example:
```bash
./client 127.0.0.1:6001 tracker_info.txt
./client 127.0.0.1:6002 tracker_info.txt
```

## Client Commands

### User Management

| Command | Description |
|---------|-------------|
| `create_user <user_id> <password>` | Create a new user account |
| `login <user_id> <password>` | Login to the system |
| `logout` | Logout from the system |

### Group Management

| Command | Description |
|---------|-------------|
| `create_group <group_id>` | Create a new group (you become the owner) |
| `join_group <group_id>` | Send a request to join a group |
| `leave_group <group_id>` | Leave a group |
| `list_groups` | List all available groups |
| `list_requests <group_id>` | List pending join requests (owner only) |
| `accept_request <group_id> <user_id>` | Accept a join request (owner only) |

### File Operations

| Command | Description |
|---------|-------------|
| `upload_file <file_path> <group_id>` | Share a file in a group |
| `list_files <group_id>` | List all files shared in a group |
| `download_file <group_id> <file_name> <dest_path>` | Download a file from the group |
| `show_downloads` | Show status of all downloads |
| `stop_share <group_id> <file_name>` | Stop sharing a specific file |

### Other

| Command | Description |
|---------|-------------|
| `quit` | Exit the client |

## Expected Output Examples

### Starting the Tracker

```bash
$ cd tracker
$ ./tracker tracker_info.txt 1
Tracker 1 started on 127.0.0.1:5000
```

### Starting the Client

```bash
$ cd client
$ ./client 127.0.0.1:6001 tracker_info.txt
Client started on 127.0.0.1:6001
Connected to 2 tracker(s)
> 
```

### User Management

```bash
> create_user alice pass123
User created successfully

> login alice pass123
Login successful

> logout
Logout successful
```

### Group Management

```bash
> create_group mygroup
Group created successfully

> list_groups
Groups: mygroup

> join_group mygroup
Join request sent

> list_requests mygroup
Pending requests: bob

> accept_request mygroup bob
Request accepted

> leave_group mygroup
Left group successfully
```

### File Operations

```bash
> upload_file /home/user/document.pdf mygroup
Computing file hashes...
File uploaded successfully

> list_files mygroup
Files: document.pdf (1048576 bytes)

> download_file mygroup document.pdf /home/user/downloads/document.pdf
Download started in background
Starting download: document.pdf (1048576 bytes, 2 pieces)
Download complete: document.pdf

> show_downloads
[C] [mygroup] document.pdf

> stop_share mygroup document.pdf
Stopped sharing: document.pdf
```

### Download Status Format

```bash
> show_downloads
[D] [mygroup] largefile.zip (15/20 pieces)   # D = Downloading
[C] [mygroup] document.pdf                    # C = Complete
```

### Error Messages

```bash
> login alice wrongpassword
Invalid password

> create_user alice pass123
User already exists

> create_group mygroup
Group already exists

> join_group mygroup
Already a member of this group

> list_files mygroup
Not a member of this group

> download_file mygroup file.txt /tmp/file.txt
No active peers for this file
```

### Complete Session Example

```bash
$ ./client 127.0.0.1:6001 tracker_info.txt
Client started on 127.0.0.1:6001
Connected to 2 tracker(s)
> create_user alice password123
User created successfully
> login alice password123
Login successful
> create_group developers
Group created successfully
> upload_file /tmp/project.zip developers
Computing file hashes...
File uploaded successfully
> list_files developers
Files: project.zip (2097152 bytes)
> logout
Logout successful
> quit
Client shutting down...
```

## Configuration

### tracker_info.txt

Contains the IP and port of each tracker, one per line:

```
127.0.0.1:5000
127.0.0.1:5001
```

## Working Procedure

1. **Start Trackers**: Start at least one tracker (preferably both for redundancy)
2. **Start Clients**: Start multiple clients, each on a different port
3. **Create Users**: Each client creates a user account
4. **Login**: Users login with their credentials
5. **Create/Join Groups**: Users create or join groups to share files
6. **Upload Files**: Users share files in their groups
7. **Download Files**: Users download files from peers in the same group

## Piece Division

- Files are divided into pieces of **512 KB** each
- Each piece has its own SHA1 hash for integrity verification
- The complete file hash is computed using the combined hashes of all pieces

## Piece Selection Algorithm

The system implements a **Rarest-First** piece selection algorithm:

1. Query all available peers for their piece availability
2. Count how many peers have each piece
3. Prioritize downloading the rarest pieces first
4. Balance the load across peers to maximize parallel downloads
5. Download up to 4 pieces in parallel

## File Integrity

- SHA1 hash is computed for each 512KB piece
- The first 20 characters of each piece hash are used (40 hex characters)
- Complete file hash is verified after download completion
- Pieces are verified immediately upon receipt

## Assumptions

1. At least one tracker is always online
2. All clients and trackers are on the same network (or have network connectivity)
3. File paths provided are valid and accessible
4. The system uses TCP for reliable communication
5. Group IDs and user IDs contain no spaces
6. Files are not modified while being shared

## Error Handling

- Connection timeouts for unresponsive peers/trackers
- Hash verification failure detection
- Graceful handling of peer disconnections
- Automatic retry for failed piece downloads
- Proper cleanup on client shutdown

## Logging

- Tracker logs all operations to `trackerlog<N>.txt`
- Logs include timestamps, client operations, and responses

## Limitations

- Files must fit in available disk space
- Large files may take time for hash computation
- Network bandwidth affects download speed
- No encryption of file transfers (could be added)

## Project Structure

```
.
├── README.md
├── tracker/
│   ├── Makefile
│   ├── tracker.cpp
│   └── tracker_info.txt
└── client/
    ├── Makefile
    ├── client.cpp
    └── tracker_info.txt
```

## Author

AOS Assignment 4 - Distributed File Sharing System
