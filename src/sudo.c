#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <time.h>
#include <sys/select.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <sys/ioctl.h>

#define SOCKET_PATH "/dev/sudo"
#define BUFFER_SIZE 8192
#define MAX_ARGS 64
#define MAX_CONFIG_LINES 1024
#define CACHE_TTL 5

typedef struct {
    uid_t uid;
    unsigned char allow;
    unsigned char checked;
} config_entry_t;

typedef struct {
    uid_t target_uid;
    gid_t target_gid;
    char command[BUFFER_SIZE];
    char cwd[1024];
    unsigned char preserve_env;
    unsigned char interactive;
    unsigned char edit_mode;
    char edit_files[MAX_ARGS][256];
    int edit_count;
    unsigned char background;
    unsigned char list_mode;
    unsigned char non_interactive;
    unsigned char version;
    unsigned char help;
    unsigned char reset_config;
    char set_env[16][256];
    int env_count;
} sudo_request_t;

#define RESPONSE_TYPE_DATA  0
#define RESPONSE_TYPE_ERROR 2
#define RESPONSE_TYPE_DONE  3

typedef struct {
    int type;
    int data_length; 
    char data[BUFFER_SIZE];
} sudo_response_t;

typedef struct {
    config_entry_t entries[MAX_CONFIG_LINES];
    int count;
    unsigned char other_allow;
    time_t timestamp;
} config_cache_t;

static config_cache_t config_cache = {0};
static const char* config_paths[] = {
    "/system/etc/sudoers",
    "/data/local/sudoers",
    NULL
};

static void create_default_config(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    
    fprintf(fp, "# Default sudoers file\n");
    fprintf(fp, "# Format: UID ALLOW/DENY\n");
    fprintf(fp, "# Use 'other' for users not listed\n\n");
    fprintf(fp, "# Root (uid 0) - allowed\n");
    fprintf(fp, "0 ALLOW\n\n");
    fprintf(fp, "# Shell user (uid 2000) - allowed\n");
    fprintf(fp, "2000 ALLOW\n\n");
    fprintf(fp, "# All other users - denied by default\n");
    fprintf(fp, "other DENY\n");
    
    fclose(fp);
    chmod(path, 0644);
}

static int ensure_config_exists(void) {
    struct stat st;
    
    for (const char **path = config_paths; *path != NULL; path++) {
        if (stat(*path, &st) == 0) {
            return 0;
        }
    }
    
    create_default_config("/data/local/sudoers");
    return 1;
}

static inline const char* get_username(uid_t uid) {
    if (uid == 0) return "root";
    if (uid == 1000) return "system";
    if (uid == 2000) return "shell";
    return "other";
}

static inline unsigned char is_daemon_running(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = SOCKET_PATH
    };
    
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    return (result == 0);
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) _exit(EXIT_FAILURE);
    if (pid > 0) _exit(EXIT_SUCCESS);
    
    if (setsid() < 0) _exit(EXIT_FAILURE);
    
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    
    pid = fork();
    if (pid < 0) _exit(EXIT_FAILURE);
    if (pid > 0) _exit(EXIT_SUCCESS);
    
    umask(0);
    if (chdir("/") < 0) _exit(EXIT_FAILURE);
    
    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) close(x);
    
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }
}

static int create_daemon_socket(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = SOCKET_PATH
    };
    
    unlink(SOCKET_PATH);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    if (listen(sock, 5) < 0) {
        close(sock);
        return -1;
    }
    
    fchmod(sock, 0666);
    return sock;
}

static int load_config(config_cache_t *cache) {
    if (time(NULL) - cache->timestamp < CACHE_TTL && cache->timestamp != 0) {
        return 0;
    }
    
    memset(cache, 0, sizeof(config_cache_t));
    cache->other_allow = 0;
    
    ensure_config_exists();
    
    FILE *fp = NULL;
    for (const char **path = config_paths; *path != NULL; path++) {
        fp = fopen(*path, "r");
        if (fp) {
            int fd = fileno(fp);
            int flags = fcntl(fd, F_GETFD);
            if (flags != -1) {
                fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
            }
            break;
        }
    }
    
    if (!fp) {
        cache->entries[0].uid = 0;
        cache->entries[0].allow = 1;
        cache->entries[1].uid = 2000;
        cache->entries[1].allow = 1;
        cache->count = 2;
        cache->timestamp = time(NULL);
        return 0;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp) && cache->count < MAX_CONFIG_LINES) {
        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        
        char *end = p + strlen(p) - 1;
        while (end > p && isspace(*end)) *end-- = '\0';
        
        char *uid_str = p;
        char *next = strchr(p, ' ');
        if (!next) next = strchr(p, '\t');
        if (!next) continue;
        
        *next++ = '\0';
        while (*next && isspace(*next)) next++;
        
        if (strcmp(uid_str, "other") == 0) {
            cache->other_allow = (strcasecmp(next, "ALLOW") == 0);
            continue;
        }
        
        char *endptr;
        uid_t uid = strtoul(uid_str, &endptr, 10);
        if (*endptr != '\0') continue;
        
        cache->entries[cache->count].uid = uid;
        cache->entries[cache->count].allow = (strcasecmp(next, "ALLOW") == 0);
        cache->count++;
    }
    
    fclose(fp);
    cache->timestamp = time(NULL);
    return 0;
}

static inline unsigned char check_access(uid_t uid) {
    if (uid == 0) return 1;
    
    config_cache_t *cache = &config_cache;
    load_config(cache);
    
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].uid == uid) {
            return cache->entries[i].allow;
        }
    }
    
    return cache->other_allow;
}

static int get_client_info(int client_sock, pid_t *client_pid, uid_t *client_uid) {
    struct ucred {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } cred;
    socklen_t len = sizeof(cred);
    
    if (getsockopt(client_sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        *client_pid = cred.pid;
        *client_uid = cred.uid;
        return 0;
    }
    
    struct stat sock_stat;
    if (fstat(client_sock, &sock_stat) != 0) {
        return -1;
    }
    
    DIR *proc = opendir("/proc");
    if (!proc) {
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        
        int is_pid = 1;
        for (char *c = entry->d_name; *c; c++) {
            if (!isdigit(*c)) {
                is_pid = 0;
                break;
            }
        }
        if (!is_pid) continue;
        
        pid_t pid = atoi(entry->d_name);
        char fd_path[256];
        snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
        
        DIR *fd_dir = opendir(fd_path);
        if (!fd_dir) continue;
        
        struct dirent *fd_entry;
        int found = 0;
        
        while ((fd_entry = readdir(fd_dir)) != NULL) {
            if (!isdigit(fd_entry->d_name[0])) continue;
            
            char link_path[256];
            char target[256];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s", pid, fd_entry->d_name);
            
            ssize_t len = readlink(link_path, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                struct stat target_stat;
                if (stat(target, &target_stat) == 0) {
                    if (target_stat.st_ino == sock_stat.st_ino) {
                        *client_pid = pid;
                        found = 1;
                        break;
                    }
                }
            }
        }
        
        closedir(fd_dir);
        
        if (found) {
            char status_path[256];
            snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
            FILE *status_file = fopen(status_path, "r");
            if (status_file) {
                char line[256];
                while (fgets(line, sizeof(line), status_file)) {
                    if (strncmp(line, "Uid:", 4) == 0) {
                        unsigned int uid;
                        sscanf(line, "Uid:\t%u", &uid);
                        *client_uid = uid;
                        break;
                    }
                }
                fclose(status_file);
            }
            closedir(proc);
            return 0;
        }
    }
    
    closedir(proc);
    return -1;
}

static inline int open_client_fds(pid_t client_pid, int *stdin_fd, int *stdout_fd, int *stderr_fd) {
    char fd_path[48];
    
    snprintf(fd_path, sizeof(fd_path), "/proc/%u/fd/0", (unsigned int)client_pid);
    *stdin_fd = open(fd_path, O_RDWR | O_CLOEXEC);
    if (*stdin_fd < 0) return -1;
    
    snprintf(fd_path, sizeof(fd_path), "/proc/%u/fd/1", (unsigned int)client_pid);
    *stdout_fd = open(fd_path, O_WRONLY | O_CLOEXEC);
    if (*stdout_fd < 0) {
        close(*stdin_fd);
        return -1;
    }
    
    snprintf(fd_path, sizeof(fd_path), "/proc/%u/fd/2", (unsigned int)client_pid);
    *stderr_fd = open(fd_path, O_WRONLY | O_CLOEXEC);
    if (*stderr_fd < 0) {
        close(*stdin_fd);
        close(*stdout_fd);
        return -1;
    }
    
    return 0;
}

static void send_response(int client_sock, int type, const char *data, int data_len) {
    sudo_response_t resp;
    resp.type = type;
    resp.data_length = data_len;
    
    if (data && data_len > 0) {
        memcpy(resp.data, data, data_len);
    }
    
    ssize_t ret = write(client_sock, &resp, sizeof(resp.type) + sizeof(resp.data_length) + data_len);
    (void)ret;
}

static const char *allowed_env[] = {
    "ANDROID_DATA",
    "ANDROID_ART_ROOT",
    "ANDROID_TZDATA_ROOT",
    "ANDROID_ASSETS",
    "ANDROID_STORAGE",
    "ANDROID_ROOT",
    "ANDROID_I18N_ROOT",
    "ANDROID_BOOTLOGO",
    "ANDROID_SOCKET_*",
    "ANDROID_RUNTIME_ROOT",
    
    "HOME",
    "PATH",
    "SHELL",
    "TERM",
    "TERMINFO",
    "TMPDIR",
    "EXTERNAL_STORAGE",
    "DOWNLOAD_CACHE",
    "ASEC_MOUNTPOINT",
    "HOSTNAME",
    "LOGNAME",
    "USER",
    
    "BOOTCLASSPATH",
    "DEX2OATBOOTCLASSPATH",
    "SYSTEMSERVERCLASSPATH",
    "STANDALONE_SYSTEMSERVER_JARS",
    
    "DISPLAY",
    "LANG",
    "LC_*",
    "COLORTERM",
    "XAUTHORITY",
    "WAYLAND_DISPLAY",
    "XDG_*",
    
    "EDITOR",
    "PAGER",
    "VISUAL",
    "MANPAGER",
    "LESS",
    "MORE",
    
    "SSH_AUTH_SOCK",
    "SSH_AGENT_PID",
    "DBUS_SESSION_BUS_ADDRESS",
    "SESSION_MANAGER",
    
    "VTE_VERSION",
    "TERM_PROGRAM",
    "TERM_PROGRAM_VERSION",
    
    "GTK_IM_MODULE",
    "QT_IM_MODULE",
    "QT_IM_MODULES",
    "XMODIFIERS",
    
    "DESKTOP_SESSION",
    "GDMSESSION",
    "GNOME_SETUP_DISPLAY",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_TYPE",
    "XDG_SESSION_CLASS",
    "XDG_MENU_PREFIX",
    
    "JAVA_HOME",
    "GRADLE_HOME",
    "MAVEN_HOME",
    "GOPATH",
    "GOROOT",
    "RUSTUP_HOME",
    "CARGO_HOME",
    "PYTHONUSERBASE",
    "NODE_PATH",
    
    NULL
};

static void setup_environment(sudo_request_t *req, pid_t client_pid) {
    uid_t client_uid = (uid_t)-1;
    char status_path[256];
    snprintf(status_path, sizeof(status_path), "/proc/%u/status", (unsigned int)client_pid);
    
    FILE *status_file = fopen(status_path, "r");
    if (status_file) {
        char line[256];
        while (fgets(line, sizeof(line), status_file)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                unsigned int real_uid;
                if (sscanf(line, "Uid:\t%u", &real_uid) == 1) {
                    client_uid = (uid_t)real_uid;
                }
                break;
            }
        }
        fclose(status_file);
    }
    
    if (client_uid == (uid_t)-1) {
        client_uid = getuid();
    }
    
    if (req->preserve_env) {
        char env_file[48];
        int env_fd;
        char env_buf[16384];
        ssize_t env_len;
        
        snprintf(env_file, sizeof(env_file), "/proc/%u/environ", (unsigned int)client_pid);
        env_fd = open(env_file, O_RDONLY | O_CLOEXEC);
        
        if (env_fd >= 0) {
            env_len = read(env_fd, env_buf, sizeof(env_buf) - 1);
            close(env_fd);
            
            if (env_len > 0) {
                char *p = env_buf;
                while (p < env_buf + env_len) {
                    char *eq = strchr(p, '=');
                    if (eq) {
                        *eq = '\0';
                        setenv(p, eq + 1, 1);
                        *eq = '=';
                    }
                    p += strlen(p) + 1;
                }
            }
        }
    } else {
        clearenv();
        
        char env_file[48];
        int env_fd;
        char env_buf[16384];
        ssize_t env_len;
        
        snprintf(env_file, sizeof(env_file), "/proc/%u/environ", (unsigned int)client_pid);
        env_fd = open(env_file, O_RDONLY | O_CLOEXEC);
        
        if (env_fd >= 0) {
            env_len = read(env_fd, env_buf, sizeof(env_buf) - 1);
            close(env_fd);
            
            if (env_len > 0) {
                char *p = env_buf;
                while (p < env_buf + env_len) {
                    char *eq = strchr(p, '=');
                    if (eq) {
                        *eq = '\0';
                        
                        int allowed = 0;
                        
                        for (int i = 0; allowed_env[i]; i++) {
                            const char *pattern = allowed_env[i];
                            size_t pattern_len = strlen(pattern);
                            
                            if (pattern_len > 0 && pattern[pattern_len - 1] == '*') {
                                size_t prefix_len = pattern_len - 1;
                                if (strncmp(p, pattern, prefix_len) == 0) {
                                    allowed = 1;
                                    break;
                                }
                            } else {
                                if (strcmp(p, pattern) == 0) {
                                    allowed = 1;
                                    break;
                                }
                            }
                        }
                        
                        if (allowed) {
                            setenv(p, eq + 1, 1);
                        }
                        
                        *eq = '=';
                    }
                    p += strlen(p) + 1;
                }
            }
        }
        
        const char *target_username = get_username(req->target_uid);
        if (target_username) {
            setenv("USER", target_username, 1);
            setenv("LOGNAME", target_username, 1);
        }

        setenv("HOME", "/", 1);

        setenv("SHELL", "/system/bin/sh", 1);
        
        setenv("PATH", "/bin:/sbin:/system/bin:/system/xbin:/system/sbin:/odm/bin:/system/vendor/bin:/product/bin:/vendor/bin:/vendor/xbin", 1);

        if (!getenv("TERM")) {
            setenv("TERM", "xterm-256color", 1);
        }
    }
    
    for (int i = 0; i < req->env_count; i++) {
        char *eq = strchr(req->set_env[i], '=');
        if (eq) {
            char *var_name = strndup(req->set_env[i], eq - req->set_env[i]);
            if (var_name) {
                setenv(var_name, eq + 1, 1);
                free(var_name);
            }
        } else {
            setenv(req->set_env[i], "", 1);
        }
    }
}

static void handle_interactive_command(int client_sock, sudo_request_t *req, pid_t client_pid, uid_t client_uid) {
    int master_fd;
    pid_t pid;
    int status;
    struct termios term;
    struct winsize ws;
    
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        memset(&ws, 0, sizeof(ws));
    }
    
    pid = forkpty(&master_fd, NULL, NULL, &ws);
    
    if (pid == -1) {
        send_response(client_sock, RESPONSE_TYPE_ERROR,
                     "ERROR: Failed to create pseudo-terminal\n", 40);
        close(client_sock);
        return;
    }
    
    if (pid == 0) {
        close(client_sock);
        
        setsid();
        
        tcgetattr(STDIN_FILENO, &term);
        term.c_lflag |= ISIG;
        term.c_cc[VINTR] = 3;
        term.c_cc[VSUSP] = 26;
        term.c_cc[VQUIT] = 28;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
        
        if (setgid(req->target_gid) != 0) {
            const char *msg = "ERROR: Failed to set group ID\n";
            ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
            (void)ret;
            _exit(1);
        }
        
        if (setuid(req->target_uid) != 0) {
            const char *msg = "ERROR: Failed to set user ID\n";
            ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
            (void)ret;
            _exit(1);
        }
        
        setup_environment(req, client_pid);
        
        if (req->cwd[0] != '\0') {
            if (chdir(req->cwd) != 0) {
                const char *msg = "ERROR: Cannot change directory\n";
                ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
                (void)ret;
                _exit(1);
            }
        }
        
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        
        if (req->command[0] != '\0') {
            execl("/system/bin/sh", "sh", "-c", req->command, NULL);
        } else {
            execl("/system/bin/sh", "sh", "-i", NULL);
        }
        
        const char *msg = "ERROR: Cannot execute command\n";
        ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
        (void)ret;
        _exit(127);
    }
    
    tcgetattr(master_fd, &term);
    term.c_lflag |= ISIG;
    term.c_iflag |= ICRNL;
    term.c_oflag |= OPOST;
    term.c_cc[VINTR] = 3;
    term.c_cc[VSUSP] = 26;
    term.c_cc[VQUIT] = 28;
    tcsetattr(master_fd, TCSANOW, &term);
    
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    fd_set read_fds;
    int max_fd = (master_fd > client_sock) ? master_fd : client_sock;
    char buffer[4096];
    int running = 1;
    
    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(client_sock, &read_fds);
        FD_SET(master_fd, &read_fds);
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        
        if (FD_ISSET(client_sock, &read_fds)) {
            ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                running = 0;
                break;
            }
            
            if (write(master_fd, buffer, bytes_read) != bytes_read) {
                running = 0;
                break;
            }
        }
        
        if (FD_ISSET(master_fd, &read_fds)) {
            ssize_t bytes_read = read(master_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                running = 0;
                break;
            }
            
            if (write(client_sock, buffer, bytes_read) != bytes_read) {
                running = 0;
                break;
            }
        }
    }
    
    send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
    
    close(master_fd);
    close(client_sock);
    
    waitpid(pid, &status, 0);
}

static void handle_noninteractive_command(int client_sock, sudo_request_t *req,
                                         pid_t client_pid, uid_t client_uid) {
    int stdin_fd = -1, stdout_fd = -1, stderr_fd = -1;
    pid_t pid;
    int status;
    
    if (open_client_fds(client_pid, &stdin_fd, &stdout_fd, &stderr_fd) < 0) {
        send_response(client_sock, RESPONSE_TYPE_ERROR,
                     "ERROR: Cannot open client file descriptors\n", 43);
        close(client_sock);
        return;
    }
    
    if (req->edit_count > 0) {
        for (int i = 0; i < req->edit_count; i++) {
            pid_t editor_pid = fork();
            
            if (editor_pid == 0) {
                close(client_sock);
                dup2(stdin_fd, STDIN_FILENO);
                dup2(stdout_fd, STDOUT_FILENO);
                dup2(stderr_fd, STDERR_FILENO);
                
                if (stdin_fd != STDIN_FILENO) close(stdin_fd);
                if (stdout_fd != STDOUT_FILENO) close(stdout_fd);
                if (stderr_fd != STDERR_FILENO) close(stderr_fd);
                
                if (setgid(req->target_gid) != 0) _exit(1);
                if (setuid(req->target_uid) != 0) _exit(1);
                
                execlp("vi", "vi", req->edit_files[i], NULL);
                _exit(1);
            } else if (editor_pid > 0) {
                waitpid(editor_pid, &status, 0);
                
                char edit_msg[512];
                int len = snprintf(edit_msg, sizeof(edit_msg), "Edited: %s\n", req->edit_files[i]);
                send_response(client_sock, RESPONSE_TYPE_DATA, edit_msg, len);
            }
        }
        
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        close(client_sock);
        return;
    }
    
    if (req->cwd[0] != '\0') {
        if (chdir(req->cwd) != 0) {
            char error_msg[512];
            int len = snprintf(error_msg, sizeof(error_msg), 
                              "ERROR: Cannot chdir to %s\n", req->cwd);
            send_response(client_sock, RESPONSE_TYPE_ERROR, error_msg, len);
            
            close(stdin_fd);
            close(stdout_fd);
            close(stderr_fd);
            close(client_sock);
            return;
        }
    }
    
    pid = fork();
    if (pid < 0) {
        send_response(client_sock, RESPONSE_TYPE_ERROR,
                     "ERROR: Fork failed\n", 20);
        
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        close(client_sock);
        return;
    }
    
    if (pid == 0) {
        close(client_sock);
        
        dup2(stdin_fd, STDIN_FILENO);
        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        
        if (stdin_fd != STDIN_FILENO) close(stdin_fd);
        if (stdout_fd != STDOUT_FILENO) close(stdout_fd);
        if (stderr_fd != STDERR_FILENO) close(stderr_fd);
        
        if (setgid(req->target_gid) != 0) _exit(1);
        if (setuid(req->target_uid) != 0) _exit(1);
        
        setup_environment(req, client_pid);
        
        if (req->command[0] != '\0') {
            execl("/system/bin/sh", "sh", "-c", req->command, NULL);
        } else {
            execl("/system/bin/sh", "sh", NULL);
        }
        
        const char *msg = "ERROR: Cannot execute command\n";
        ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
        (void)ret;
        _exit(127);
    } else {
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        
        if (req->background) {
            char pid_msg[64];
            int len = snprintf(pid_msg, sizeof(pid_msg), "[1] %u\n", (unsigned int)pid);
            send_response(client_sock, RESPONSE_TYPE_DATA, pid_msg, len);
            send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
            close(client_sock);
            return;
        }
        
        waitpid(pid, &status, 0);
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        close(client_sock);
    }
}

static void handle_daemon_client(int client_sock) {
    sudo_request_t req;
    ssize_t n;
    pid_t client_pid;
    uid_t client_uid;
    
    if (get_client_info(client_sock, &client_pid, &client_uid) < 0) {
        send_response(client_sock, RESPONSE_TYPE_ERROR, 
                     "ERROR: Cannot determine client credentials\n", 42);
        close(client_sock);
        return;
    }
    
    if (!check_access(client_uid)) {
        send_response(client_sock, RESPONSE_TYPE_ERROR, 
                     "Access denied\n", 15);
        close(client_sock);
        return;
    }
    
    send_response(client_sock, RESPONSE_TYPE_DATA, NULL, 0);
    
    n = read(client_sock, &req, sizeof(req));
    if (n <= 0) {
        close(client_sock);
        return;
    }

    if (req.reset_config) {
        memset(&config_cache, 0, sizeof(config_cache_t));
        send_response(client_sock, RESPONSE_TYPE_DATA, 
                     "Configuration reloaded\n", 23);
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        close(client_sock);
        return;
    }
    
    char proc_path[48];
    snprintf(proc_path, sizeof(proc_path), "/proc/%u", (unsigned int)client_pid);
    if (access(proc_path, F_OK) != 0) {
        send_response(client_sock, RESPONSE_TYPE_ERROR,
                     "Client process no longer exists\n", 39);
        close(client_sock);
        return;
    }
    
    if (req.version) {
        send_response(client_sock, RESPONSE_TYPE_DATA, "Sudo version 1.0\n", 17);
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        close(client_sock);
        return;
    }
    
    if (req.help) {
        const char *help_text = 
            "Usage: sudo [options] [command]\n"
            "Options:\n"
            "  -u, --user USER         Run as user\n"
            "  -g, --group GROUP       Run as group\n"
            "  -s, --shell             Run shell\n"
            "  -i, --login             Run login shell\n"
            "  -E, --preserve-env      Preserve environment\n"
            "  -e, --edit              Edit files\n"
            "  -l, --list              List privileges\n"
            "  -b, --background        Run in background\n"
            "  -D, --chdir DIR         Change directory\n"
            "  -n, --non-interactive   Non-interactive mode\n"
            "  -r, --reload            Reload configuration\n"
            "  -V, --version           Show version\n"
            "  -h, --help              Show help\n"
            "  -d, --daemon            Start daemon mode\n";
        
        send_response(client_sock, RESPONSE_TYPE_DATA, help_text, strlen(help_text));
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        close(client_sock);
        return;
    }
    
    if (req.list_mode) {
        char list_text[512];
        int len = snprintf(list_text, sizeof(list_text),
                          "User %s may run the following commands on this host:\n"
                          "    (ALL : ALL) ALL\n",
                          get_username(client_uid));
        send_response(client_sock, RESPONSE_TYPE_DATA, list_text, len);
        send_response(client_sock, RESPONSE_TYPE_DONE, NULL, 0);
        close(client_sock);
        return;
    }
    
    int need_pty = (req.command[0] == '\0' || req.interactive) && 
                   !req.edit_mode && !req.list_mode;
    
    if (need_pty) {
        handle_interactive_command(client_sock, &req, client_pid, client_uid);
    } else {
        handle_noninteractive_command(client_sock, &req, client_pid, client_uid);
    }
}

static void run_daemon(void) {
    int sock;
    
    if (geteuid() != 0) {
        fprintf(stderr, "Daemon must be run as root\n");
        _exit(EXIT_FAILURE);
    }
    
    ensure_config_exists();
    daemonize();
    
    sock = create_daemon_socket();
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        _exit(EXIT_FAILURE);
    }
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    
    while (1) {
        int client = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        
        pid_t pid = fork();
        if (pid < 0) {
            close(client);
            continue;
        }
        
        if (pid == 0) {
            close(sock);
            handle_daemon_client(client);
            _exit(EXIT_SUCCESS);
        } else {
            close(client);
        }
    }
    
    close(sock);
    unlink(SOCKET_PATH);
}

static inline int connect_to_daemon(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = SOCKET_PATH
    };
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    sudo_response_t resp;
    ssize_t n = read(sock, &resp, sizeof(resp.type) + sizeof(resp.data_length));
    
    if (n <= 0) {
        close(sock);
        return -1;
    }
    
    if (resp.type == RESPONSE_TYPE_ERROR) {
        if (resp.data_length > 0 && resp.data_length < BUFFER_SIZE) {
            char error_msg[BUFFER_SIZE];
            n = read(sock, error_msg, resp.data_length);
            if (n == resp.data_length) {
                ssize_t ret = write(STDERR_FILENO, error_msg, n);
                (void)ret;
            }
        }
        close(sock);
        return -1;
    }
    
    if (resp.type != RESPONSE_TYPE_DATA || resp.data_length != 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

static void parse_arguments(int argc, char *argv[], sudo_request_t *req) {
    memset(req, 0, sizeof(sudo_request_t));
    
    req->target_uid = 0;
    req->target_gid = 0;
    req->interactive = 1;
    
    if (getcwd(req->cwd, sizeof(req->cwd)) == NULL) {
        strcpy(req->cwd, "/");
    }
    
    int command_started = 0;
    uid_t parsed_target_uid = 0;
    gid_t parsed_target_gid = 0;
    unsigned char user_specified = 0;
    unsigned char group_specified = 0;
    
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        
        if (command_started) {
            size_t current_len = strlen(req->command);
            if (current_len == 0) {
                strncpy(req->command, arg, sizeof(req->command) - 1);
                req->command[sizeof(req->command) - 1] = '\0';
            } else {
                strncat(req->command, " ", sizeof(req->command) - current_len - 1);
                strncat(req->command, arg, sizeof(req->command) - strlen(req->command) - 1);
            }
            continue;
        }
        
        if (strcmp(arg, "-u") == 0 || strcmp(arg, "--user") == 0) {
            if (i + 1 < argc) {
                char *user_str = argv[++i];
                char *endptr;
                long val = strtol(user_str, &endptr, 10);
                if (*endptr == '\0') {
                    parsed_target_uid = (uid_t)val;
                    user_specified = 1;
                }
            }
        }
        else if (strcmp(arg, "-g") == 0 || strcmp(arg, "--group") == 0) {
            if (i + 1 < argc) {
                char *group_str = argv[++i];
                char *endptr;
                long val = strtol(group_str, &endptr, 10);
                if (*endptr == '\0') {
                    parsed_target_gid = (gid_t)val;
                    group_specified = 1;
                }
            }
        }
        else if (strcmp(arg, "-D") == 0 || strcmp(arg, "--chdir") == 0) {
            if (i + 1 < argc) {
                strncpy(req->cwd, argv[++i], sizeof(req->cwd) - 1);
                req->cwd[sizeof(req->cwd) - 1] = '\0';
            }
        }
        else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--shell") == 0) {
            req->interactive = 1;
        }
        else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--login") == 0) {
            req->interactive = 1;
            req->preserve_env = 0;
        }
        else if (strcmp(arg, "-E") == 0 || strcmp(arg, "--preserve-env") == 0) {
            req->preserve_env = 1;
        }
        else if (strcmp(arg, "-b") == 0 || strcmp(arg, "--background") == 0) {
            req->background = 1;
        }
        else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--non-interactive") == 0) {
            req->non_interactive = 1;
        }
        else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            req->version = 1;
            command_started = 1;
        }
        else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            req->help = 1;
            command_started = 1;
        }
        else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--list") == 0) {
            req->list_mode = 1;
            command_started = 1;
        }
        else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--reload") == 0) {
            req->reset_config = 1;
            command_started = 1;
        }
        else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--edit") == 0) {
            req->edit_mode = 1;
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(req->edit_files[req->edit_count], argv[++i], 255);
                req->edit_files[req->edit_count][255] = '\0';
                req->edit_count++;
                if (req->edit_count >= MAX_ARGS) break;
            }
        }
        else if (strchr(arg, '=') && !command_started && req->env_count < 16) {
            strncpy(req->set_env[req->env_count], arg, 255);
            req->set_env[req->env_count][255] = '\0';
            req->env_count++;
        }
        else {
            if (arg[0] == '-' && arg[1] != '\0') {
                fprintf(stderr, "sudo: invalid option - '%c'\n", arg[1]);
                fprintf(stderr, "Try 'sudo -h' for more information.\n");
                exit(1);
            }
            
            command_started = 1;
            strncpy(req->command, arg, sizeof(req->command) - 1);
            req->command[sizeof(req->command) - 1] = '\0';
        }
    }
    
    if (user_specified) {
        req->target_uid = parsed_target_uid;
        if (!group_specified) {
            req->target_gid = parsed_target_uid;
        }
    }
    
    if (group_specified) {
        req->target_gid = parsed_target_gid;
    }
}

static void interactive_mode(int sock) {
    fd_set read_fds;
    int max_fd;
    char buffer[4096];
    struct termios orig_termios, raw_termios;
    int interactive = isatty(STDIN_FILENO);
    
    if (interactive) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        
        raw_termios = orig_termios;
        raw_termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        raw_termios.c_oflag &= ~OPOST;
        raw_termios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw_termios.c_cflag &= ~(CSIZE | PARENB);
        raw_termios.c_cflag |= CS8;
        raw_termios.c_cc[VMIN] = 1;
        raw_termios.c_cc[VTIME] = 0;
        
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios);
    }
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sock, &read_fds);
        max_fd = (STDIN_FILENO > sock) ? STDIN_FILENO : sock;
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) break;
            
            if (write(sock, buffer, n) != n) break;
        }
        
        if (FD_ISSET(sock, &read_fds)) {
            ssize_t n = read(sock, buffer, sizeof(buffer));
            if (n <= 0) break;
            if (write(STDOUT_FILENO, buffer, n) != n) break;
        }
    }
    
    if (interactive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1 && (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--daemon") == 0)) {
        run_daemon();
        return 0;
    }
    
    if (!is_daemon_running()) {
        fprintf(stderr, "Error: sudo daemon is not running.\n");
        fprintf(stderr, "Start it with: %s --daemon (as root)\n", argv[0]);
        return 1;
    }
    
    int sock = connect_to_daemon();
    if (sock < 0) {
        return 1;
    }
    
    sudo_request_t req;
    parse_arguments(argc, argv, &req);
    
    if (write(sock, &req, sizeof(req)) != sizeof(req)) {
        close(sock);
        return 1;
    }
    
    int interactive_mode_needed = (req.command[0] == '\0' || req.interactive) && 
                                   !req.edit_mode && !req.list_mode &&
                                   !req.version && !req.help && !req.reset_config;
    
    if (interactive_mode_needed) {
        interactive_mode(sock);
        close(sock);
        return 0;
    } else {
        int exit_code = 0;
        int done_received = 0;
        
        while (!done_received) {
            int response_type;
            ssize_t bytes_read = read(sock, &response_type, sizeof(response_type));
            
            if (bytes_read <= 0) {
                break;
            }
            
            if (bytes_read != sizeof(response_type)) {
                break;
            }
            
            int data_length;
            bytes_read = read(sock, &data_length, sizeof(data_length));
            
            if (bytes_read != sizeof(data_length)) {
                break;
            }
            
            if (response_type == RESPONSE_TYPE_DONE) {
                done_received = 1;
                break;
            } 
            else if (response_type == RESPONSE_TYPE_DATA || response_type == RESPONSE_TYPE_ERROR) {
                if (data_length > 0 && data_length < BUFFER_SIZE) {
                    char buffer[BUFFER_SIZE];
                    bytes_read = read(sock, buffer, data_length);
                    
                    if (bytes_read == data_length) {
                        if (response_type == RESPONSE_TYPE_DATA) {
                            if (write(STDOUT_FILENO, buffer, data_length) != data_length) {
                                exit_code = 1;
                            }
                        } else {
                            if (write(STDERR_FILENO, buffer, data_length) != data_length) {
                                exit_code = 1;
                            }
                            exit_code = 1;
                        }
                    } else {
                        break;
                    }
                }
            } 
            else {
                break;
            }
        }
        
        close(sock);
        return exit_code;
    }
}
