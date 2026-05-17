# Distributed File Sharing System

[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux-orange.svg)](https://www.linux.org/)

A peer-to-peer distributed file sharing system built with C++ that enables users to share and download files within groups. Features parallel downloading from multiple peers, piece-wise file transfer using BitTorrent-like protocols, and SHA1 hash verification for file integrity.

## Features

### Tracker

- **User Management**: Create accounts, login/logout functionality with authentication
- **Group Management**: Create groups, join/leave groups, accept/reject join requests
- **File Tracking**: Track which files are shared in each group and which users are seeding
- **Multi-Tracker Synchronization**: Two trackers stay in sync; any state change is propagated immediately
- **Logging**: All operations are logged with timestamps to `trackerlog<N>.txt`

### Client

- **User Authentication**: Login with password verification
- **Group Operations**: Create, join, leave groups; accept or reject membership requests (owner only)
- **File Sharing**: Register any local file in a group — only metadata and peer address are sent to the tracker, not the file itself
- **Parallel Downloading**: Download up to 4 pieces simultaneously from different peers per batch
- **Rarest-First Piece Selection**: Pieces are prioritised by scarcity and load-balanced across peers, guaranteeing use of multiple peers when available
- **Leeching Support**: Downloaded pieces are immediately available for upload to other peers
- **File Integrity**: SHA1 verification per piece on receipt and whole-file verification on completion
- **Concurrent Downloads**: Multiple files can be downloaded at the same time in background threads
- **Automatic Re-sharing**: All files that were shared before logout are automatically re-registered on next login

## Architecture

```text
  ┌──────────────────────────────────────────────────┐
  │                  Tracker Layer                    │
  │                                                   │
  │   ┌─────────────┐  SYNC   ┌─────────────┐        │
  │   │  Tracker 1  │◄───────►│  Tracker 2  │        │
  │   │  port 5000  │         │  port 5001  │        │
  │   └─────────────┘         └─────────────┘        │
  └────────────┬──────────────────────┬──────────────┘
               │  metadata (TCP)      │  metadata (TCP)
               │  client initiates    │  client initiates
  ┌────────────▼──────────────────────▼──────────────┐
  │                  Client / Peer Layer              │
  │                                                   │
  │   ┌──────────┐   pieces    ┌──────────┐          │
  │   │  Peer A  │◄───────────►│  Peer B  │          │
  │   │ port 6001│    (TCP)    │ port 6002│          │
  │   └──────────┘             └──────────┘          │
  │         ▲                       ▲                 │
  │         │   pieces (TCP)        │                 │
  │         ▼                       ▼                 │
  │   ┌──────────┐             ┌──────────┐          │
  │   │  Peer C  │◄───────────►│  Peer D  │          │
  │   │ port 6003│             │ port 6004│          │
  │   └──────────┘             └──────────┘          │
  └──────────────────────────────────────────────────┘
```

**Communication flows:**

- **Client → Tracker**: clients always initiate — login, upload metadata, request peer lists
- **Tracker ↔ Tracker**: bidirectional SYNC messages keep both trackers in sync after every state change
- **Peer ↔ Peer**: direct TCP connections for piece transfer; any peer can connect to any other peer — not a fixed chain

Trackers hold all metadata (users, groups, file info, seeder lists). Actual file data is transferred directly between client peers over TCP; the tracker is never in the data path.

## Prerequisites

- Linux OS
- g++ with C++17 support
- OpenSSL development libraries (`libssl-dev`)

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

`tracker_number` is the 1-based line number of this tracker's entry in `tracker_info.txt`.

```bash
./tracker tracker_info.txt 1  # binds to the address on line 1 (e.g. 127.0.0.1:5000)
./tracker tracker_info.txt 2  # binds to the address on line 2 (e.g. 127.0.0.1:5001)
```

To stop a tracker gracefully, type `quit` in its terminal.

### Start Client

```bash
cd client
./client <IP>:<PORT> tracker_info.txt
```

Each client must listen on a unique `IP:PORT`. This address is registered with the tracker on login so other peers can reach it.

```bash
./client 127.0.0.1:6001 tracker_info.txt
./client 127.0.0.1:6002 tracker_info.txt
```

## Client Commands

### User Management

| Command | Description |
| ------- | ----------- |
| `create_user <user_id> <password>` | Create a new user account |
| `login <user_id> <password>` | Login to the system |
| `logout` | Stop sharing all files and logout |

### Group Management

| Command | Description |
| ------- | ----------- |
| `create_group <group_id>` | Create a new group (creator becomes owner) |
| `join_group <group_id>` | Send a join request to a group |
| `leave_group <group_id>` | Leave a group (ownership transfers if owner leaves) |
| `list_groups` | List all groups in the network |
| `list_requests <group_id>` | List pending join requests — owner only |
| `accept_request <group_id> <user_id>` | Accept a join request — owner only |
| `reject_request <group_id> <user_id>` | Reject a join request — owner only |

### File Operations

| Command | Description |
| ------- | ----------- |
| `upload_file <file_path> <group_id>` | Register a local file for sharing in a group |
| `list_files <group_id>` | List all files shared in a group |
| `download_file <group_id> <file_name> <dest_path>` | Download a file from group peers (runs in background) |
| `show_downloads` | Show status of all downloads |
| `stop_share <group_id> <file_name>` | Stop sharing a specific file |

### Other

| Command | Description                |
|---------|----------------------------|
| `quit`  | Logout and exit the client |

## Working Procedure

1. **Start Trackers** — start both trackers for redundancy (at least one must be online)
2. **Start Clients** — each client on a different port
3. **Create Users** — `create_user <id> <password>` on each client
4. **Login** — `login <id> <password>`; previously shared files are automatically re-registered
5. **Create or Join Groups** — creator becomes owner; others send join requests that the owner accepts or rejects
6. **Upload Files** — `upload_file` registers the file's metadata and your peer address with the tracker; the file stays on your disk
7. **Download Files** — `download_file` fetches peer addresses from the tracker, then downloads pieces directly from peers; as each piece arrives it is immediately available for other peers to fetch from you
8. **Logout** — your peer address is removed from all active seeder lists until next login

## Piece Division

Files are split into logical pieces of **512 KB** each. The last piece may be smaller.

## Piece Selection Algorithm

The client uses a **Rarest-First with Load Balancing** algorithm:

1. Query every available peer for its piece availability bitmap (`GET_PIECES_INFO`)
2. For each incomplete piece, record which peers hold it
3. Sort pieces ascending by the number of peers that have them (rarest first)
4. Assign each piece to the peer with the lowest current assignment count, ensuring load is spread across peers
5. Download up to **4 pieces in parallel** per batch; repeat until the file is complete

This guarantees that when multiple peers are available, pieces are distributed across them rather than saturating a single peer.

## File Integrity

Piece hash format follows the assignment specification exactly:

- Each 512 KB piece is hashed with SHA1, producing a 40-character hex string
- The **first 20 characters** of each piece's hex hash are used
- These 20-character strings are concatenated to form the piece hash field (e.g. two pieces → 40-character string)
- The complete file SHA1 (full 40-character hex) is also stored and verified after the final piece is written
- Each piece is verified against its expected hash immediately on receipt; mismatched pieces are discarded

## Configuration

### tracker_info.txt

One `IP:PORT` per line. Both tracker and client read this file:

```text
127.0.0.1:5000
127.0.0.1:5001
```

The tracker at line N is started with `./tracker tracker_info.txt N`. Clients try each listed tracker in order and use the first one that responds.

## Assumptions

1. At least one tracker is always online
2. All clients and trackers have TCP connectivity to each other
3. File paths provided to `upload_file` are valid and the file is not modified while being shared
4. Group IDs, user IDs, and file names contain no spaces or pipe (`|`) characters
5. Each client runs on a unique IP:PORT combination

## Error Handling

- Connection timeouts (5 s to trackers, 10 s to peers) prevent indefinite blocking
- Malformed peer messages are caught and rejected without crashing the handler thread
- Piece hash mismatches cause the piece to be discarded and re-requested
- Whole-file hash mismatch after download completion is reported as a warning
- Peer disconnections during download are handled gracefully; the batch simply completes without that piece
- Ownership is automatically transferred to another member when the owner leaves a group

## Logging

- Each tracker writes to `trackerlog<N>.txt` in its working directory
- Every client command and tracker response is logged with a timestamp

## Persistence

Each tracker saves its state to `tracker_state<N>.dat` (e.g. `tracker_state1.dat`) after every durable operation. On restart the state is restored before the first client connects, so users, groups, and file metadata survive a tracker crash or reboot.

Session-only data (login state, IP/port, active seeder lists) is not persisted — it is rebuilt automatically as clients log back in and re-register their shared files.

## Limitations

- No encryption on file transfers or tracker communication
- File names with spaces are not supported (the wire protocol uses space-delimited fields)

## Project Structure

```text
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
