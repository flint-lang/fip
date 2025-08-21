#pragma once

// #define FIP_DEBUG_MODE

/*
 * This is the Flint Interop Protocol used for inter-process communcation
 * around interoperability. Both master and slaves will use this single header
 * file for their IPC. In the beginning all slaves are already running processes
 * and sleep, waiting for the connect message of the master. When being
 * connected to a master each slave waits for further instructions from the
 * master. Each slave has a direct connection to it's master but the master can
 * only broadcast messages to all slaves. Slaves may also broadcast to all other
 * processes.
 *
 * These are the functionalities that the FIP must provide:
 *   - Broadcast Connect (To connect the master to all slave modules)
 *   - Broadcast Listen (Slave waits for the master to send the connect message)
 *
 *   - Broadcast Symbol provide (When the master provides a symbol for it's
 * slaves)
 *   - Broadcast Symbol request (When the master requests a symbol)
 *   - Broadcast Symbol resolved (When a slave found the symbol)
 *   - Targetted Symbol resolved (The slave tells the master about the symbol)
 *
 *   - Broadcast Object request (When the master requests all compiled object
 * files)
 *   - Targetted Object resolved (The path to the slave's object)
 */

/*
 * If FIP_LIB is defined the `fip.h` library will act as a wrapper to be used
 * within C source files to create bindings which can be understood by the
 * `fip-c` Interop Module. The FIP_LIB definition is defined by default and it
 * needs to be undefined using the NO_FIP_LIB definition, but that only needs to
 * be done when creating the `fip-c` Interop Module itself, so users will just
 * need to include this file and use the provided annotations and nothing more.
 */

#ifdef FIP_DEBUG_MODE
#define NO_FIP_LIB         // For debugging purposes
#define FIP_IMPLEMENTATION // For debugging purposes
#define FIP_MASTER
#define FIP_SLAVE
#endif

#define FIP_LIB
#ifdef NO_FIP_LIB
#undef FIP_LIB
#endif

#ifdef FIP_LIB

/*
 * All the definitions to use inside binding files to bind C to FIP
 */

/// @macro `FIP_FN`
/// @brief This macro is used to signal the FIP that this function is exported
///
/// @example This macro can be used like this:
///         int FIP_FN foo() { ... }
///         void FIP_FN bar();
#define FIP_FN __attribute__((section(".fip_exports")))

#else

#include "toml/tomlc17.h"

#include <errno.h>
#include <poll.h> // poll() for multiplexing I/O
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>     // for strerror, strlen, strncpy, memset
#include <sys/select.h> // for select, FD_SET, etc.
#include <sys/socket.h> // Socket creation, binding, listening
#include <sys/un.h>     // Unix domain socket addresses
#include <unistd.h>     // close(), read(), write() system calls

const char *FIP_SOCKET_PATH = "/tmp/fip_socket";
#define FIP_MAX_SLAVES 64

/*
 * =====================
 * GENERAL FUNCTIONALITY
 * =====================
 */

/// @typedef `fip_type_enum_t`
/// @brief Enum of all possible FIP types
///
/// @note Only primitive types are supported for now
typedef enum : unsigned char {
    U8 = 0, // unsigned char
    U16,    // unsigned short
    U32,    // unsigned int
    U64,    // unsigned long
    I8,     // char
    I16,    // short
    I32,    // int
    I64,    // long
    F32,    // float
    F64,    // double
    BOOL,   // bool (byte)
    STR,    // char*
} fip_type_enum_t;

/// @typedef `fip_type_sig_t`
/// @brief Struct representing the type signature of a FIP type
typedef struct {
    bool is_mutable;
    fip_type_enum_t type;
} fip_type_sig_t;

/// @typedef `fip_fn_sig_t`
/// @brief Struct representing the signature of a FIP-defined function
typedef struct {
    char *name;
    unsigned int args_len;
    fip_type_sig_t *args;
    unsigned int rets_len;
    fip_type_sig_t *rets;
} fip_fn_sig_t;

/// @function `fip_print`
/// @brief Prints a message from the given module ID from the format string and
/// the variadic arguments which will be forwarded to printf
///
/// @param `id` The id of the process to print the message from
/// @param `format` The format string of the printed message
/// @param `...` The variadic values to put into the formatted output
void fip_print(unsigned int id, const char *format, ...);

/// @function `fip_parse_type_string`
/// @brief Parses the given type string and returns the type
///
/// @param `id` The id of the process which tries to parse a type string
/// @param `type_str` The string contianing the type definition
/// @param `start_idx` The index to start the type matching at
/// @param `end_idx` The index where to end the type matching
/// @param `type_str_table` The table contianing all the type strings
/// @param `sig` The type signature list to add the type to
/// @param `sig_len` The length of the signature list
/// @return `bool` Whether the type was parsable
bool fip_parse_type_string(       //
    const unsigned int id,        //
    const char *type_str,         //
    const char *type_str_table[], //
    size_t start_idx,             //
    size_t end_idx,               //
    fip_type_sig_t **sig,         //
    unsigned int *sig_len         //
);

/// @function `fip_parse_fn_signature`
/// @brief Parses a given function signature and returns the parsed signature in
/// a struct
///
/// @param `id` The id of the process in which the signature is parsed
/// @param `signature` The function signature to parse
/// @return `fip_fn_sig_t` The parsed function signature
fip_fn_sig_t fip_parse_fn_signature(unsigned int id, const char *signature);

/// @function `fip_print_fn_sig`
/// @brief Prints a parsed function signature to the console
///
/// @param `id` The id of the process in which the signature is printed
/// @param `sig` The function signature to print
void fip_print_fn_sig(unsigned int id, fip_fn_sig_t *sig);

/*
 * ====================
 * MASTER FUNCTIONALITY
 * ====================
 */

#ifdef FIP_MASTER

typedef struct {
    unsigned char active_count;
    pid_t pids[FIP_MAX_SLAVES];
} interop_modules_t;

typedef struct {
    int server_fd;
    int client_fds[FIP_MAX_SLAVES];
    int client_count;
} fip_master_state_t;

typedef struct {
    bool fip_c_enabled;
} fip_master_config_t;

extern char **environ;

static fip_master_state_t master_state = {0};

/// @function `spawn_interop_module`
/// @brief Creates a new interop module and adds it's process ID to the list of
/// modules in the interop modules parameter
///
/// @param `modules` A pointer to the structure containing all the module PIDs
/// @param `binary` The interop module to start
/// @return `bool` Whether the interop module process creation was successful
bool spawn_interop_module(interop_modules_t *modules, const char *module);

/// @function `terminate_all_slaves`
/// @brief Terminates all currently running slaves if they have not been
/// terminated yet
///
/// @param `modules` A pointer to the structure containing all the module PIDs
void terminate_all_slaves(interop_modules_t *modules);

/// @function `fip_master_init_socket`
/// @brief Initializes the socket which is used by all slaves to send messages
/// to and to recieve messages from the master
///
/// @return `int` The file descriptor id of the created socket
int fip_master_init_socket();

/// @function `fip_master_accept_pending_connections`
/// @brief Checks for any pending connections and connects to them, this is
/// needed to check whether a new connection tries to be established
///
/// @param `socket_fd` The socket file descriptor to check the new connections
/// at
void fip_master_accept_pending_connections(int socket_fd);

/// @function `fip_master_broadcast_message`
/// @brief Broadcasts a given message to all connected slaves
///
/// @param `socked_fd` The file descriptor of the socket to send the message
/// to
/// @param `message` The message to send to the socket
void fip_master_broadcast_message(int socket_fd, const char *message);

/// @function `fip_master_cleanup_socket`
/// @brief Cleans up the socket before terminating the master
///
/// @param `socked_fd` The file descriptor of the socket to clean up
void fip_master_cleanup_socket(int socket_fd);

/// @function `fip_master_load_config`
/// @brief Loads the master config from the `.config/fip/fip.toml` file
///
/// @return `fip_master_config_t` The loaded configuration
fip_master_config_t fip_master_load_config();

#endif // End of #ifdef FIP_MASTER

#ifdef FIP_SLAVE

/*
 * ===================
 * SLAVE Functionality
 * ===================
 */

typedef enum {
    C,
} fip_module_enum_t;

const char *fip_module_enum_str[] = {"c"};

typedef struct {
    unsigned int sources_len;
    char **sources;
    unsigned int compile_flags_len;
    char **compile_flags;
} fip_module_c_config_t;

typedef struct {
    fip_module_enum_t type;
    union {
        fip_module_c_config_t c;
    } u;
} fip_slave_config_t;

/// @function `fip_slave_init_socket`
/// @brief Tries to connect to the master's socket and returns the file
/// descirptor of the master socket
///
/// @return `int` The file descriptor id of the master's created socket
int fip_slave_init_socket();

/// @function `fip_slave_recieve_message`
/// @brief Checks whether a message has been sent and stores the sent
/// message in the buffer
///
/// @param `socket_fd` The file descriptor id to recieve the message from
/// @param `buffer` The buffer where to store the recieved message at
/// @param `buffer_size` The size of the recieved message
/// @return `bool` Whether a message was recieved
bool fip_slave_receive_message(int socket_fd, char *buffer, size_t buffer_size);

/// @function `fip_slave_cleanup_socket`
/// @brief Cleans up the socket connection to the master's socket
///
/// @param `socket_fd` The socket file descriptor to clean up
void fip_slave_cleanup_socket(int socket_fd);

/// @function `fip_slave_load_config`
/// @brief Loads the master config from the `.config/fip/fip-X.toml` file where
/// `X` is dependant on the type
///
/// @param `id` The ID of the slave that tries to load the config
/// @param `type` The type of the fip module's configuration to load
/// @return `fip_master_config_t` The loaded configuration
fip_slave_config_t fip_slave_load_config( //
    const unsigned int id,                //
    const fip_module_enum_t type          //
);

#endif // End of #ifdef FIP_SLAVE

#ifdef FIP_IMPLEMENTATION

/// @var `fip_type_names`
/// @brief A small array where the value at each enum index is the name of the
/// type at that enum value
const char *fip_type_names[] = {
    "u8",
    "u16",
    "u32",
    "u64",
    "i8",
    "i16",
    "i32",
    "i64",
    "f32",
    "f64",
    "bool",
    "str",
};
#define FIP_TYPE_COUNT 12

void fip_print(unsigned int id, const char *format, ...) {
    if (!format) {
        return;
    }
    char prefix[64];
    va_list args;

    // Create the prefix
    if (id == 0) {
        snprintf(prefix, sizeof(prefix), "[Master]:  ");
    } else {
        snprintf(prefix, sizeof(prefix), "[Slave %u]: ", id);
    }

    // Print prefix first
    printf("%s", prefix);

    // Print the formatted message
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    // Add newline
    printf("\n");
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alpha_num(char c) {
    return is_alpha(c) || is_digit(c);
}

void fip_parse_sig_type(   //
    fip_type_sig_t **sig,  //
    unsigned int *sig_len, //
    fip_type_enum_t type   //
) {
    unsigned int arg_id = *sig_len;
    (*sig_len)++;
    unsigned int new_size = sizeof(fip_type_sig_t) * *sig_len;
    *sig = realloc(*sig, new_size);
    (*sig)[arg_id].is_mutable = false;
    (*sig)[arg_id].type = type;
}

bool fip_parse_type_string(       //
    const unsigned int id,        //
    const char *type_str,         //
    const char *type_str_table[], //
    size_t start_idx,             //
    size_t end_idx,               //
    fip_type_sig_t **sig,         //
    unsigned int *sig_len         //
) {
    char arg_str[64] = {0};
    // Remove all leading and trailing spaces
    char c = *(type_str + end_idx - 1);
    while (c == ' ' || c == '\t') {
        end_idx--;
        c = *(type_str + end_idx - 1);
    }
    c = *(type_str + start_idx);
    while (c == ' ' || c == '\t') {
        start_idx++;
        c = *(type_str + start_idx);
    }
    strncpy(arg_str, type_str + start_idx, end_idx - start_idx);
    for (char i = 0; i < FIP_TYPE_COUNT; i++) {
        if (strcmp(arg_str, type_str_table[i]) == 0) {
            fip_parse_sig_type(sig, sig_len, (fip_type_enum_t)i);
            return true;
        }
    }
    fip_print(id, "Unknown Type: '%s'", arg_str);
    (*sig_len)--;
    return false;
}

bool fip_parse_sig_types(  //
    unsigned int id,       //
    fip_type_sig_t **sig,  //
    unsigned int *sig_len, //
    unsigned int *it,      //
    const char *signature  //
) {
    unsigned int start_it = ++(*it);
    char c = signature[*it];
    *sig = malloc(sizeof(fip_type_sig_t));
    *sig_len = 0;
    while (c != '\0' && c != ')') {
        if (c == ',') {
            if (!fip_parse_type_string(            //
                    id, signature, fip_type_names, //
                    start_it, *it, sig, sig_len)   //
            ) {
                fip_print(id, "message");
                return false;
            }
            c = signature[++(*it)];
            start_it = *it;
            continue;
        }
        c = signature[++(*it)];
    }
    if (start_it == *it) {
        free(*sig);
        *sig = NULL;
        *sig_len = 0;
    } else if (!fip_parse_type_string(            //
                   id, signature, fip_type_names, //
                   start_it, *it, sig, sig_len)   //
    ) {
        fip_print(id, "message");
        return false;
    }
    return true;
}

fip_fn_sig_t fip_parse_fn_signature(unsigned int id, const char *signature) {
    fip_fn_sig_t result = {0};
    if (!signature) {
        return result;
    }
    // The name is not allowed to contain alphanumeric characters
    // The name must start with an alpha character
    unsigned int it = 0;
    char c = signature[it];
    if (!is_alpha(c)) {
        fip_print(id, "ERROR: fn not allowed to start with %c", c);
        return result;
    }
    c = signature[++it];
    while (c != '\0' && c != '(') {
        if (!is_alpha_num(c)) {
            fip_print(id, "ERROR: non-alpha-num in fn name");
            return result;
        }
        c = signature[++it];
    }
    // Everything until the `(` is the name of the function
    char *name = malloc(it + 1);
    memcpy(name, signature, it);
    name[it] = '\0';
    result.name = name;

    // Now we can start parsing the argument types
    if (!fip_parse_sig_types(                                   //
            id, &result.args, &result.args_len, &it, signature) //
    ) {
        fip_print(id, "ERROR: Failed parsing arg types");
        return result;
    }
    // Now we check if an arrow follows after the `)`
    it++;
    if (signature[it++] == '-' && signature[it++] == '>') {
        // We have return type(s) in the signature
        if (signature[it] == '(') {
            // We have multiple return types
            if (!fip_parse_sig_types(                                   //
                    id, &result.rets, &result.rets_len, &it, signature) //
            ) {
                fip_print(id, "ERROR: Failed parsing ret types");
                return result;
            }
        } else {
            // We have a single return type
            it--;
            if (!fip_parse_sig_types(                                   //
                    id, &result.rets, &result.rets_len, &it, signature) //
            ) {
                fip_print(id, "ERROR: Failed parsing ret type");
                return result;
            }
        }
    }

    return result;
}

void fip_print_fn_sig(unsigned int id, fip_fn_sig_t *sig) {
    fip_print(id, "Function Signature:");
    fip_print(id, "  name: %s", sig->name);
    for (unsigned int i = 0; i < sig->args_len; i++) {
        fip_print(id, "  arg[%u]: %s", i, fip_type_names[sig->args[i].type]);
    }
    for (unsigned int i = 0; i < sig->rets_len; i++) {
        fip_print(id, "  ret[%u]: %s", i, fip_type_names[sig->rets[i].type]);
    }
}

#ifdef FIP_MASTER

bool spawn_interop_module(interop_modules_t *modules, const char *module) {
    char id[8] = {0};
    char _module[256] = {0};
    memcpy(_module, module, strlen(module));
    snprintf(id, 8, "%d", modules->active_count + 1);
    char *argv[] = {_module, id, NULL};

    fip_print(0, "Spawning slave %s...", id);

    // Create file actions to ensure stdout inheritance
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    int status = posix_spawn(&modules->pids[modules->active_count], _module,
        &actions, // Use file actions
        NULL,     // No special attributes
        argv, environ);

    posix_spawn_file_actions_destroy(&actions);

    if (status != 0) {
        fip_print(0, "Failed to spawn slave %s: %d", id, status);
        return false;
    }

    modules->active_count++;
    return true;
}

void terminate_all_slaves(interop_modules_t *modules) {
    // We terminate all slaves as their workloads must have been finished by
    // now (the master has collected the results in the form of the .o
    // files)
    for (unsigned char i = 0; i < modules->active_count; i++) {
        if (kill(modules->pids[i], 0)) {
            // Is still running and we are allowed to kill it
            kill(modules->pids[i], SIGTERM);
        }
    }
}

int fip_master_init_socket() {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        fip_print(0, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Remove existing socket file if it exists
    unlink(FIP_SOCKET_PATH);

    // Set up socket address
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Bind socket to path
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fip_print(0, "Failed to bind socket: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    // Start listening (allow up to FIP_MAX_SLAVES connections)
    if (listen(server_fd, FIP_MAX_SLAVES) == -1) {
        fip_print(0, "Failed to listen on socket: %s", strerror(errno));
        close(server_fd);
        unlink(FIP_SOCKET_PATH);
        return -1;
    }

    // Initialize client array
    for (int i = 0; i < FIP_MAX_SLAVES; i++) {
        master_state.client_fds[i] = -1;
    }
    master_state.server_fd = server_fd;
    master_state.client_count = 0;

    fip_print(0, "Socket initialized and listening on %s", FIP_SOCKET_PATH);
    return server_fd;
}

void fip_master_accept_pending_connections(int socket_fd) {
    fd_set read_fds;
    struct timeval timeout;

    while (master_state.client_count < FIP_MAX_SLAVES) {
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        timeout.tv_sec = 0;       // Non-blocking
        timeout.tv_usec = 100000; // 100ms timeout

        int activity = select(socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity <= 0) {
            break; // No more pending connections
        }

        if (FD_ISSET(socket_fd, &read_fds)) {
            int client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd != -1) {
                master_state.client_fds[master_state.client_count] = client_fd;
                master_state.client_count++;
                fip_print(0, "Accepted connection from slave %d",
                    master_state.client_count);
            }
        }
    }
}

void fip_master_broadcast_message(int socket_fd, const char *message) {
    // Accept any pending connections first
    fip_master_accept_pending_connections(socket_fd);

    fip_print(0, "Broadcasting message to %d slaves: '%s'",
        master_state.client_count, message);

    // Send to all connected clients
    for (int i = 0; i < master_state.client_count; i++) {
        if (master_state.client_fds[i] != -1) {
            ssize_t sent =
                send(master_state.client_fds[i], message, strlen(message), 0);
            if (sent == -1) {
                fip_print(0, "Failed to send to slave %d", i + 1);
                // Optionally remove this client from the array
                close(master_state.client_fds[i]);
                master_state.client_fds[i] = -1;
            }
        }
    }
}

void fip_master_cleanup_socket(int socket_fd) {
    // Close all client connections
    for (int i = 0; i < master_state.client_count; i++) {
        if (master_state.client_fds[i] != -1) {
            close(master_state.client_fds[i]);
        }
    }

    if (socket_fd != -1) {
        close(socket_fd);
    }
    unlink(FIP_SOCKET_PATH);
    fip_print(0, "Socket cleaned up");
}

fip_master_config_t fip_master_load_config() {
    const char *file_path = ".config/fip/fip.toml";
    FILE *fp = fopen(file_path, "r");
    fip_master_config_t config = {0};
    if (!fp) {
        fip_print(0, "Config file not found: %s", file_path);
        return config;
    }
    toml_result_t toml = toml_parse_file(fp);
    fclose(fp);
    // Check for parse error
    if (!toml.ok) {
        fip_print(0, "Failed to parse fip.toml file: %s", toml.errmsg);
        return config;
    }

    // Extract value(s)
    toml_datum_t fip_c_enable = toml_seek(toml.toptab, "fip-c.enable");

    if (fip_c_enable.type != TOML_BOOLEAN) {
        fip_print(0, "Missing fip-c.enable field");
        return config;
    }
    config.fip_c_enabled = fip_c_enable.u.boolean;
    toml_free(toml);
    return config;
}

#endif // End of #ifdef FIP_MASTER

#ifdef FIP_SLAVE

int fip_slave_init_socket() {
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Try to connect to master (with retries)
    for (int i = 0; i < 10; i++) { // Try 10 times
        if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return socket_fd; // Success
        }
        usleep(100000); // Wait 100ms before retry
    }

    close(socket_fd);
    return -1; // Failed to connect
}

bool fip_slave_receive_message( //
    int socket_fd,              //
    char *buffer,               //
    size_t buffer_size          //
) {
    // printf("fip_slave_recieve_message function called\n");
    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);

    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms timeout

    int activity = select(socket_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (activity > 0 && FD_ISSET(socket_fd, &read_fds)) {
        ssize_t received = recv(socket_fd, buffer, buffer_size - 1, 0);
        if (received > 0) {
            buffer[received] = '\0'; // Null-terminate
            return true;
        }
    }

    return false; // No message or error
}

void fip_slave_cleanup_socket(int socket_fd) {
    if (socket_fd != -1) {
        close(socket_fd);
    }
}

fip_slave_config_t fip_slave_load_config( //
    const unsigned int id,                //
    const fip_module_enum_t type          //
) {
    char file_path[32] = ".config/fip/fip-";
    const char *type_str = fip_module_enum_str[type];
    memcpy(file_path + 16, type_str, strlen(type_str));
    memcpy(file_path + 16 + strlen(type_str), ".toml", 5);

    FILE *fp = fopen(file_path, "r");
    fip_slave_config_t config = {0};
    if (!fp) {
        fip_print(0, "Config file not found: %s", file_path);
        return config;
    }
    toml_result_t toml = toml_parse_file(fp);
    fclose(fp);
    // Check for parse error
    if (!toml.ok) {
        fip_print(id, "Failed to parse fip-%s.toml file: %s", type_str,
            toml.errmsg);
        return config;
    }

    // Extract value(s)
    config.type = type;
    switch (type) {
        case C: {
            toml_datum_t sources_d = toml_seek(toml.toptab, "sources");
            if (sources_d.type != TOML_ARRAY) {
                fip_print(id, "Missing 'sources' field");
                return config;
            }
            int32_t arr_len = sources_d.u.arr.size;
            toml_datum_t *sources_elems_d = sources_d.u.arr.elem;
            config.u.c.sources = (char **)malloc(sizeof(char *) * arr_len);
            config.u.c.sources_len = arr_len;
            for (int32_t i = 0; i < arr_len; i++) {
                if (sources_elems_d[i].type != TOML_STRING) {
                    fip_print(id, "'sources' does contain a non-string value");
                    return config;
                }
                int32_t strlen = sources_elems_d[i].u.str.len;
                config.u.c.sources[i] = malloc(strlen + 1);
                memcpy(                           //
                    config.u.c.sources[i],        //
                    sources_elems_d[i].u.str.ptr, //
                    strlen                        //
                );
                config.u.c.sources[i][strlen] = '\0';
            }

            toml_datum_t compile_flags_d =
                toml_seek(toml.toptab, "compile_flags");
            if (compile_flags_d.type != TOML_ARRAY) {
                fip_print(id, "Missing 'compile_flags' field");
                return config;
            }
            arr_len = compile_flags_d.u.arr.size;
            toml_datum_t *compile_flags_elems_d = compile_flags_d.u.arr.elem;
            config.u.c.compile_flags =
                (char **)malloc(sizeof(char *) * arr_len);
            config.u.c.compile_flags_len = arr_len;
            for (int32_t i = 0; i < arr_len; i++) {
                if (compile_flags_elems_d[i].type != TOML_STRING) {
                    fip_print(id,
                        "'compile_flags' does contain a non-string value");
                    return config;
                }
                int32_t strlen = compile_flags_elems_d[i].u.str.len;
                config.u.c.compile_flags[i] = malloc(strlen + 1);
                memcpy(                                 //
                    config.u.c.compile_flags[i],        //
                    compile_flags_elems_d[i].u.str.ptr, //
                    strlen                              //
                );
                config.u.c.compile_flags[i][strlen] = '\0';
            }
            break;
        }
    }
    toml_free(toml);
    return config;
}

#endif // End of #ifdef FIP_SLAVE

#endif // End of #ifdef FIP_IMPLEMENTATION

#endif // End of #ifdef FIP_LIB
