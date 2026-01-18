# Shared Memory Chat Application

A multi-process chat application developed in C using **POSIX Shared Memory**, **Semaphores**, and **Pthreads**. This project was created as an assignment for the **Operating Systems** course for my uni.

## 📋 Overview

This application allows multiple users (processes) to communicate in real-time through distinct chat rooms. It demonstrates Inter-Process Communication (IPC) concepts by utilizing a shared memory segment accessible by all running instances of the program.

### Key Features

- **Multi-Room Support**: Supports up to 10 independent chat rooms.
- **Multithreading**: Uses separate threads for sending (Main Thread) and receiving (Reader Thread) messages to ensure non-blocking I/O.
- **Synchronization**: Implements **POSIX Semaphores** (mutexes) to handle race conditions and ensure thread-safe access to the shared buffer.
- **Message Persistence**: Uses a circular buffer mechanism where messages remain until read by all active participants.
- **Clean Shutdown**: Automatically unlinks shared memory when the last user exits the system.

## ⚙️ Architecture & Design

The project relies on a central `SharedSegment` structure stored in shared memory, defined in `common.h`.

### Data Structures

1.  **SharedSegment**: Contains an array of `ChatRoom` structures.
2.  **ChatRoom**:
    - `mutex`: A generic semaphore for mutual exclusion within the room.
    - `messages`: A circular buffer (size 100) storing `Message` structs.
    - `active_participants`: Tracks the number of users in the room to manage message read receipts.
3.  **Message**:
    - `payload`: The actual text (max 128 chars).
    - `readers_left`: A counter ensuring every message is displayed to all participants before being overwritten.

### Workflow

- **Initialization**: The first process creates the shared memory segment (`shm_open` with `O_CREAT`). Subsequent processes attach to it.
- **Writing**: The main thread waits for user input and writes to the circular buffer's head, ensuring it doesn't overwrite unread messages.
- **Reading**: A background `reader` thread polls the shared memory. If `global_seq_count` increases, it retrieves and prints the new message, then decrements the `readers_left` counter.

## 🚀 Getting Started

### Prerequisites

- GCC Compiler
- Linux environment (required for POSIX library support: `-lrt`, `-pthread`)

### Compilation

To build the project, simply run:

```bash
make
```

This will compile the source code and place the executable in the `build/` directory.

### Usage

Run the application:

```bash
./build/chat

```

1. **Select a Room**: Upon start, enter a Room ID (0-9) to join.
2. **Chat**: Type your message and press Enter.
3. **Exit**: Type `TERMINATE` to leave the chat room.

> **Note**: If you are the last user in the entire system, the application will automatically remove the shared memory segment (`/chat_app`).

## 👤 Author

**Mr.Phoenix300**

---

_This software is provided for educational purposes._
