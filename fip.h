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
#define FIP_MSG_SIZE 1024

#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"
#define DEFAULT "\033[0m"
#define GREY "\033[90m"
#define FIP_TYPE_COUNT 12

/// @typedef `fip_type_enum_t`
/// @brief Enum of all possible FIP types
///
/// @note Only primitive types are supported for now
typedef enum : unsigned char {
    FIP_U8 = 0, // unsigned char
    FIP_U16,    // unsigned short
    FIP_U32,    // unsigned int
    FIP_U64,    // unsigned long
    FIP_I8,     // char
    FIP_I16,    // short
    FIP_I32,    // int
    FIP_I64,    // long
    FIP_F32,    // float
    FIP_F64,    // double
    FIP_BOOL,   // bool (byte)
    FIP_STR,    // char*
} fip_type_enum_t;

/// @typedef `fip_msg_type_t`
/// @bfief Enum of all possible messages the FIP can handle
typedef enum : unsigned char {
    FIP_MSG_CONNECT_REQUEST = 0, // Slave trying to connect to master
    FIP_MSG_SYMBOL_REQUEST,      // Master requesting symbol resolution
    FIP_MSG_SYMBOL_RESPONSE,     // Slave response of FN_REQ
    FIP_MSG_COMPILE_REQUEST,     // Master requesting all slaves to compile
    FIP_MSG_OBJECT_RESPONSE,     // Slave responding compilation with .o file
    FIP_MSG_KILL = 255,          // Kill command is special
} fip_msg_type_t;

/// @typedef `fip_msg_symbol_type_t`
/// @brief Enum of all possible symbol types
typedef enum : unsigned char {
    FIP_SYM_FUNCTION,
    FIP_SYM_DATA,
} fip_msg_symbol_type_t;

/*
 * =================
 * SYMBOL STRUCTURES
 * =================
 */

/// @typedef `fip_type_sig_t`
/// @brief Struct representing the type signature of a FIP type
typedef struct {
    bool is_mutable;
    fip_type_enum_t type;
} fip_sig_type_t;

/// @typedef `fip_fn_sig_t`
/// @brief Struct representing the signature of a FIP-defined function
typedef struct {
    char name[128];
    unsigned char args_len;
    fip_sig_type_t *args;
    unsigned char rets_len;
    fip_sig_type_t *rets;
} fip_sig_fn_t;

/*
 * ==================
 * MESSAGE STRUCTURES
 * ==================
 */

/// @typedef `fip_msg_connect_request_t`
/// @breif Struct representing all information from a connection request
typedef struct {
    struct {
        uint8_t major;
        uint8_t minor;
        uint8_t patch;
    } version;
} fip_msg_connect_request_t;

/// @typedef `fip_msg_symbol_request_t`
/// @brief Struct representing the symbol request message
typedef struct {
    fip_msg_symbol_type_t type;
    union {
        fip_sig_fn_t fn;
    } sig;
} fip_msg_symbol_request_t;

/// @typedef `fip_msg_symbol_response_t`
/// @brief Struct representing the symbol response message
typedef struct {
    bool found;
    char module_name[16];
    fip_msg_symbol_type_t type;
    union {
        fip_sig_fn_t fn;
    } sig;
} fip_msg_symbol_response_t;

/// @typedef `fip_msg_compile_request_t`
/// @brief Struct representing the compile request message
typedef struct {
    struct {
        char arch[16];
        char sub[16];
        char vendor[16];
        char sys[16];
        char abi[16];
    } target;
} fip_msg_compile_request_t;

/// @typedef `fip_msg_object_response_t`
/// @brief Struct representing the object response message
typedef struct {
    char module_name[16];
    char path[128];
} fip_msg_object_response_t;

/// @typedef `fip_msg_kill_t`
/// @brief Struct representing the kill message
typedef struct {
    enum : unsigned char {
        FIP_KILL_FINISH = 0,
        FIP_KILL_VERSION_MISMATCH,
    } reason;
} fip_msg_kill_t;

/// @typedef `fip_msg_t`
/// @brief Struct representing sent / recieved FIP messages
typedef struct {
    fip_msg_type_t type;
    union {
        fip_msg_connect_request_t con_req;
        fip_msg_symbol_request_t sym_req;
        fip_msg_symbol_response_t sym_res;
        fip_msg_compile_request_t com_req;
        fip_msg_object_response_t obj_res;
        fip_msg_kill_t kill;
    } u;
} fip_msg_t;

/*
 * =====================
 * GENERAL FUNCTIONALITY
 * =====================
 */

/// @function `fip_print`
/// @brief Prints a message from the given module ID from the format string and
/// the variadic arguments which will be forwarded to printf
///
/// @param `id` The id of the process to print the message from
/// @param `format` The format string of the printed message
/// @param `...` The variadic values to put into the formatted output
void fip_print(unsigned int id, const char *format, ...);

/// @function `fip_print_msg`
/// @brief Prints the given message from the given module ID
///
/// @param `id` The id of the process to print the message from
/// @param `message` The message to print
void fip_print_msg(unsigned int id, const fip_msg_t *message);

/// @function `fip_encode_msg`
/// @brief Encodes a given message into a string and stores it in the buffer
///
/// @param `buffer` The buffer in which to store the message in
/// @param `message` The message to encode into the buffer
void fip_encode_msg(char buffer[FIP_MSG_SIZE], const fip_msg_t *message);

/// @function `fip_decode_msg`
/// @brief Tries to decode a message from the given buffer and create a message
/// from it
///
/// @param `buffer` The buffer from which the message is decoded
/// @param `message` Pointer to the message where the result is stored
void fip_decode_msg(const char buffer[FIP_MSG_SIZE], fip_msg_t *message);

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
    fip_sig_type_t **sig,         //
    unsigned char *sig_len        //
);

/// @function `fip_parse_fn_signature`
/// @brief Parses a given function signature and returns the parsed signature in
/// a struct
///
/// @param `id` The id of the process in which the signature is parsed
/// @param `signature` The function signature to parse
/// @return `fip_sig_fn_t` The parsed function signature
fip_sig_fn_t fip_parse_fn_signature(unsigned int id, const char *signature);

/// @function `fip_print_sig_fn`
/// @brief Prints a parsed function signature to the console
///
/// @param `id` The id of the process in which the signature is printed
/// @param `sig` The function signature to print
void fip_print_sig_fn(unsigned int id, fip_sig_fn_t *sig);

/*
 * ====================
 * MASTER FUNCTIONALITY
 * ====================
 */

#ifdef FIP_MASTER

typedef struct {
    unsigned char active_count;
    pid_t pids[FIP_MAX_SLAVES];
} fip_interop_modules_t;

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
bool spawn_interop_module(fip_interop_modules_t *modules, const char *module);

/// @function `terminate_all_slaves`
/// @brief Terminates all currently running slaves if they have not been
/// terminated yet
///
/// @param `modules` A pointer to the structure containing all the module PIDs
void terminate_all_slaves(fip_interop_modules_t *modules);

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
/// @param `buffer` The buffer in which the message will be encoded before
/// sending it
/// @param `message` The message to send to the socket
void fip_master_broadcast_message( //
    int socket_fd,                 //
    char buffer[FIP_MSG_SIZE],     //
    const fip_msg_t *message       //
);

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

void fip_print(unsigned int id, const char *format, ...) {
    if (!format) {
        return;
    }
    char prefix[64];
    va_list args;

    // Create the prefix
    if (id == 0) {
        snprintf(prefix, sizeof(prefix), "[%sMaster%s]:  ", CYAN, DEFAULT);
    } else {
        snprintf(prefix, sizeof(prefix), "[%sSlave %u%s]: ", YELLOW, id,
            DEFAULT);
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

void fip_print_msg(unsigned int id, const fip_msg_t *message) {
    fip_print(id, "TODO: fip_print_msg");
}

void fip_encode_sig_fn(        //
    char buffer[FIP_MSG_SIZE], //
    int *idx,                  //
    const fip_sig_fn_t *sig    //
) {
    // Because the name is a known size of 128 bytes we can store it directly in
    // the buffer
    memcpy(buffer + *idx, sig->name, 128);
    *idx += 128;
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are unsigned char's annyway
    // because which function has more than 256 parameters or return types?
    buffer[(*idx)++] = sig->args_len;
    for (uint8_t i = 0; i < sig->args_len; i++) {
        buffer[(*idx)++] = sig->args[i].is_mutable;
        buffer[(*idx)++] = sig->args[i].type;
    }
    buffer[(*idx)++] = sig->rets_len;
    for (uint8_t i = 0; i < sig->rets_len; i++) {
        buffer[(*idx)++] = sig->rets[i].is_mutable;
        buffer[(*idx)++] = sig->rets[i].type;
    }
}

void fip_encode_msg(char buffer[FIP_MSG_SIZE], const fip_msg_t *message) {
    // Clear the buffer
    memset(buffer, 0, 1024);
    // The first character in the buffer is the message type
    int idx = 0;
    buffer[idx++] = message->type;
    switch (message->type) {
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the versions into the buffer one by
            // one and is done
            buffer[idx++] = message->u.con_req.version.major;
            buffer[idx++] = message->u.con_req.version.minor;
            buffer[idx++] = message->u.con_req.version.patch;
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            buffer[idx++] = message->u.sym_req.type;
            switch (message->u.sym_req.type) {
                case FIP_SYM_FUNCTION:
                    fip_encode_sig_fn(buffer, &idx, &message->u.sym_req.sig.fn);
                    break;
                case FIP_SYM_DATA:
                    break;
            }
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            // We place all elements into the buffer one by one until we come to
            // the union
            buffer[idx++] = message->u.sym_res.found;
            memcpy(buffer + idx, message->u.sym_res.module_name, 16);
            idx += 16;
            buffer[idx++] = message->u.sym_res.type;
            switch (message->u.sym_res.type) {
                case FIP_SYM_FUNCTION:
                    fip_encode_sig_fn(buffer, &idx, &message->u.sym_res.sig.fn);
                    break;
                case FIP_SYM_DATA:
                    break;
            }
            break;
        case FIP_MSG_COMPILE_REQUEST:
            // The compile request places each 16 byte piece of it in the buffer
            // directly
            memcpy(buffer + idx, message->u.com_req.target.arch, 16);
            idx += 16;
            memcpy(buffer + idx, message->u.com_req.target.sub, 16);
            idx += 16;
            memcpy(buffer + idx, message->u.com_req.target.vendor, 16);
            idx += 16;
            memcpy(buffer + idx, message->u.com_req.target.sys, 16);
            idx += 16;
            memcpy(buffer + idx, message->u.com_req.target.abi, 16);
            break;
        case FIP_MSG_OBJECT_RESPONSE:
            // The sizes of the buffers are known so we can put them into the
            // buffer directly
            memcpy(buffer + idx, message->u.obj_res.module_name, 16);
            idx += 16;
            memcpy(buffer + idx, message->u.obj_res.path, 128);
            break;
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            buffer[idx++] = message->u.kill.reason;
            break;
    }
}

void fip_decode_sig_fn(              //
    const char buffer[FIP_MSG_SIZE], //
    int *idx,                        //
    fip_sig_fn_t *sig                //
) {
    // Because the name is a known size of 128 bytes we can store it directly in
    // the signature
    memcpy(sig->name, buffer + *idx, 128);
    *idx += 128;
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are unsigned char's annyway
    // because which function has more than 256 parameters or return types?
    sig->args_len = buffer[(*idx)++];
    if (sig->args_len > 0) {
        sig->args = malloc(sizeof(fip_sig_type_t) * sig->args_len);
        for (uint8_t i = 0; i < sig->args_len; i++) {
            sig->args[i].is_mutable = buffer[(*idx)++];
            sig->args[i].type = buffer[(*idx)++];
        }
    } else {
        sig->args = NULL;
    }
    sig->rets_len = buffer[(*idx)++];
    if (sig->rets_len > 0) {
        sig->rets = malloc(sizeof(fip_sig_type_t) * sig->rets_len);
        for (uint8_t i = 0; i < sig->rets_len; i++) {
            sig->rets[i].is_mutable = buffer[(*idx)++];
            sig->rets[i].type = buffer[(*idx)++];
        }
    } else {
        sig->rets = NULL;
    }
}

void fip_decode_msg(const char buffer[FIP_MSG_SIZE], fip_msg_t *message) {
    // The first character in the buffer is the message type
    int idx = 0;
    message->type = buffer[idx++];
    switch (message->type) {
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the versions into the buffer one by
            // one and is done
            message->u.con_req.version.major = buffer[idx++];
            message->u.con_req.version.minor = buffer[idx++];
            message->u.con_req.version.patch = buffer[idx++];
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            message->u.sym_req.type = buffer[idx++];
            switch (message->u.sym_req.type) {
                case FIP_SYM_FUNCTION:
                    fip_decode_sig_fn(buffer, &idx, &message->u.sym_req.sig.fn);
                    break;
                case FIP_SYM_DATA:
                    break;
            }
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            // We place all elements into the buffer one by one until we come to
            // the union
            message->u.sym_res.found = buffer[idx++];
            memcpy(message->u.sym_res.module_name, buffer + idx, 16);
            idx += 16;
            message->u.sym_res.type = buffer[idx++];
            switch (message->u.sym_res.type) {
                case FIP_SYM_FUNCTION:
                    fip_decode_sig_fn(buffer, &idx, &message->u.sym_res.sig.fn);
                    break;
                case FIP_SYM_DATA:
                    break;
            }
            break;
        case FIP_MSG_COMPILE_REQUEST:
            // The compile request places each 16 byte piece of it in the buffer
            // directly
            memcpy(message->u.com_req.target.arch, buffer + idx, 16);
            idx += 16;
            memcpy(message->u.com_req.target.sub, buffer + idx, 16);
            idx += 16;
            memcpy(message->u.com_req.target.vendor, buffer + idx, 16);
            idx += 16;
            memcpy(message->u.com_req.target.sys, buffer + idx, 16);
            idx += 16;
            memcpy(message->u.com_req.target.abi, buffer + idx, 16);
            break;
        case FIP_MSG_OBJECT_RESPONSE:
            // The sizes of the buffers are known so we can put them into the
            // buffer directly
            memcpy(message->u.obj_res.module_name, buffer + idx, 16);
            idx += 16;
            memcpy(message->u.obj_res.path, buffer + idx, 128);
            break;
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            message->u.kill.reason = buffer[idx++];
            break;
    }
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

void fip_parse_sig_type(    //
    fip_sig_type_t **sig,   //
    unsigned char *sig_len, //
    fip_type_enum_t type    //
) {
    unsigned int arg_id = *sig_len;
    (*sig_len)++;
    unsigned int new_size = sizeof(fip_sig_type_t) * *sig_len;
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
    fip_sig_type_t **sig,         //
    unsigned char *sig_len        //
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

void fip_print_sig_fn(unsigned int id, fip_sig_fn_t *sig) {
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

bool spawn_interop_module(fip_interop_modules_t *modules, const char *module) {
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

void terminate_all_slaves(fip_interop_modules_t *modules) {
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

void fip_master_broadcast_message( //
    int socket_fd,                 //
    char buffer[FIP_MSG_SIZE],     //
    const fip_msg_t *message       //
) {
    // Accept any pending connections first
    fip_master_accept_pending_connections(socket_fd);

    fip_print(0, "Broadcasting message to %d slaves",
        master_state.client_count);

    fip_encode_msg(buffer, message);

    // Send to all connected clients
    for (int i = 0; i < master_state.client_count; i++) {
        if (master_state.client_fds[i] != -1) {
            ssize_t sent =
                send(master_state.client_fds[i], buffer, FIP_MSG_SIZE, 0);
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
