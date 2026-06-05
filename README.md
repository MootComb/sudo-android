# sudo-android

A lightweight implementation of the `sudo` utility for Android 4.2.2+ (KitKat, API 17), operating on a client-server architecture via Unix socket.

## Features

- Client-server architecture
- Support for Android 4.2.2 (KitKat, API 17) and above
- Support for interactive and non-interactive sessions
- Pseudo-terminal (pty) for interactive commands
- Access configuration via `sudoers` file
- Static build (depends only on the kernel)
- Support for ARM, ARMv7, AArch64, and x86

## Requirements

- **Root access** (the daemon must run as root)
- **Android 4.2.2+** (API 17+)
- Installed cross-compilation toolchain (optional)

## Building

### Native build (for the current architecture)
```bash
./build.sh native
```

### Cross-compilation for ARM
```bash
./build.sh arm
```

### Cross-compilation for AArch64
```bash
./build.sh aarch64
```

### Interactive mode
```bash
./build.sh
```

After building, a static binary file `sudo` will be created.

## Installation

1. Build the binary for the target architecture
2. Copy `sudo` to `/system/bin/`, `/data/local/tmp/`, or `/data/local/`
3. Set execution permissions:
   ```bash
   chmod 755 /path/to/sudo
   ```
4. Start the daemon (as root):
   ```bash
   su -c "/data/local/tmp/sudo --daemon"
   ```

## Socket Location

**Unix socket is created at:** `/dev/sudo`

### Placement specifics:
- The socket is readable/writable by all users (permissions 0666)

### Socket verification:
```bash
ls -la /dev/sudo
# Output: srwxrwxrwx 1 root root 0 ... /dev/sudo
```

## Socket Protocol Format

### Request structure (`sudo_request_t`)

The client sends the daemon a structure of ~4KB in size:

```c
typedef struct {
    uid_t target_uid;           // 4 bytes - target UID
    gid_t target_gid;           // 4 bytes - target GID
    char command[BUFFER_SIZE];  // 8192 bytes - command
    char cwd[1024];             // 1024 bytes - working directory
    unsigned char preserve_env; // 1 byte - preserve environment
    unsigned char interactive;  // 1 byte - interactive mode
    unsigned char edit_mode;    // 1 byte - edit mode
    char edit_files[MAX_ARGS][256]; // 64*256 bytes - files to edit
    int edit_count;             // 4 bytes - number of files
    unsigned char background;   // 1 byte - background mode
    unsigned char list_mode;    // 1 byte - show privileges
    unsigned char non_interactive; // 1 byte - non-interactive mode
    unsigned char version;      // 1 byte - show version
    unsigned char help;         // 1 byte - show help
    unsigned char reset_config; // 1 byte - reload config
    char set_env[16][256];      // 16*256 bytes - environment variables
    int env_count;              // 4 bytes - number of variables
} sudo_request_t;
```

### Command format in the `command` field

The command is passed as a regular string that will be executed via `sh -c`:

```c
// Example: run ls
req.command = "ls -la /data"

// Example: run multiple commands
req.command = "cd /sdcard && ls && echo 'Done'"
```

### Response structure (`sudo_response_t`)

The daemon responds with the following structure:

```c
typedef struct {
    int type;           // 4 bytes - response type
    int data_length;    // 4 bytes - data length
    char data[BUFFER_SIZE]; // 8192 bytes - data
} sudo_response_t;
```

### Response types:

| Type | Value | Description |
|------|-------|-------------|
| `RESPONSE_TYPE_DATA` | 0 | Regular data (stdout) |
| `RESPONSE_TYPE_ERROR` | 2 | Error (stderr) |
| `RESPONSE_TYPE_DONE` | 3 | Command completed |

## Interaction Protocol

### 1. Access check
```
Client -> Daemon: connect()
Daemon -> Client: RESPONSE_TYPE_DATA (data_length=0) - acknowledgment
```

### 2. Sending the request
```
Client -> Daemon: sudo_request_t (full structure)
```

### 3. Receiving the result (non-interactive mode)
```
Daemon -> Client: RESPONSE_TYPE_DATA + data (may be repeated multiple times)
Daemon -> Client: RESPONSE_TYPE_ERROR + data (on error)
Daemon -> Client: RESPONSE_TYPE_DONE (completion)
```

### 4. Interactive mode
```
Client -> Daemon: sudo_request_t (interactive=1)
Daemon -> Client: [direct two-way data transfer]
                 - Client STDIN -> master-pty
                 - master-pty -> Client STDOUT/STDERR
```

## Configuration

### sudoers file

The utility searches for configuration in the following order:
- `/system/etc/sudoers` (read-only)
- `/data/local/sudoers` (read/write)

### Example sudoers
```
# Allow root
0 ALLOW

# Allow shell
2000 ALLOW

# Allow apps from isolated environment
10086 ALLOW

# Deny guest user (usually 9998)
9998 DENY

# Deny others
other DENY
```

## Usage

### Basic commands

```bash
# Run a command as root (UID 0)
sudo <command>

# Run as the shell user (UID 2000)
sudo -u 2000 <command>

# Start an interactive shell
sudo -s

# Preserve environment variables
sudo -E <command>

# Edit files
sudo -e /system/etc/hosts

# Reload configuration
sudo --reload

# Show version
sudo -V
```

### Interactive mode

When launched without a command or with the -s flag, an interactive shell is started:
- Full-featured PTY (pseudo-terminal)
- Support for Ctrl+C, Ctrl+Z
- Terminal resize handling
- Colored output

## Examples

### Mounting system as read-write
```bash
sudo mount -o rw,remount /system
```

### Editing build.prop
```bash
sudo -e /system/build.prop
```

## Troubleshooting

### "sudo daemon is not running"
```bash
su -c "/data/local/tmp/sudo --daemon"
```

### "Permission denied" when creating the socket
```bash
# Ensure the daemon is running as root
ps | grep sudo
# If not:
killall sudo
su -c "/data/local/tmp/sudo --daemon"
```

### "Address already in use"
```bash
# Remove the old socket
su -c "rm -f /dev/sudo"
# Restart the daemon
```
