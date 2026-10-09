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

#ifdef FIP_DEBUG_MODE
#define FIP_IMPLEMENTATION // For debugging purposes
#define FIP_MASTER
#define FIP_SLAVE
#endif

#include <tomlc17.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>

#else // #ifdef _WIN32 end

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef __USE_POSIX
#define __USE_POSIX
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// #define _GNU_SOURCE
// #define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <signal.h> // IWYU pragma: keep
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Explicit function declarations for POSIX functions
extern FILE *fdopen(int fd, const char *mode);
extern int fileno(FILE *stream);
extern int kill(pid_t pid, int sig);
extern int nanosleep(const struct timespec *req, struct timespec *rem);
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);
extern int clock_gettime(clockid_t clk_id, struct timespec *tp);

#endif // #else

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
__attribute__((unused)) static void msleep(unsigned int ms) {
    Sleep(ms);
}
#else
__attribute__((unused)) static void msleep(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* nanosleep may be interrupted; ignore remaining time for simplicity */
    nanosleep(&ts, NULL);
}
#endif

// The version of the FIP
#define FIP_MAJOR 0
#define FIP_MINOR 4
#define FIP_PATCH 1

#define FIP_MSG_SIZE 4096
#define FIP_SLAVE_DELAY_MS 1

#define FIP_MAX_MODULE_NAME_LEN 16
#define FIP_PATH_SIZE 8
#define FIP_PATHS_SIZE FIP_MSG_SIZE - 32

/// @typedef `fip_log_level_e`
/// @brief Enum of all possible log levels of FIP
typedef enum fip_log_level_e : uint8_t {
    FIP_NONE = 0,
    FIP_ERROR,
    FIP_WARN,
    FIP_INFO,
    FIP_DEBUG,
    FIP_TRACE,
} fip_log_level_e;

extern fip_log_level_e LOG_LEVEL;

/*
 * ===============
 * TYPE STRUCTURES
 * ===============
 */

/// @brief Forward-Declaration of the `fip_type_t` struct type
struct fip_type_t;

/// @typedef `fip_type_prim_e`
/// @brief Enum of all possible primitive types supported by FIP
typedef enum fip_type_prim_e : uint8_t {
    FIP_VOID = 0, // void
    FIP_U8,       // unsigned char
    FIP_U16,      // unsigned short
    FIP_U32,      // unsigned int
    FIP_U64,      // unsigned long
    FIP_I8,       // char
    FIP_I16,      // short
    FIP_I32,      // int
    FIP_I64,      // long
    FIP_F32,      // float
    FIP_F64,      // double
    FIP_BOOL,     // bool (byte)
    FIP_STR,      // char*
} fip_type_prim_e;

/// @typedef `fip_type_ptr_t`
/// @brief The struct representing a pointer type
typedef struct {
    struct fip_type_t *base_type;
} fip_type_ptr_t;

/// @typedef `fip_type_struct_t`
/// @brief The struct representing a struct type
typedef struct {
    char name[128];
    size_t field_count;
    struct fip_type_t *fields;
} fip_type_struct_t;

/// @typedef `fip_type_recursive_t`
/// @brief The struct representing recursive / repeating types
typedef struct {
    uint8_t levels_back;
} fip_type_recursive_t;

/// @typedef `fip_type_enum_t`
/// @brief The struct representing enum types
typedef struct {
    char name[128];
    uint8_t bit_width;
    bool is_signed;
    size_t value_count;
    // The value is a size_t because it can be anything from i1 up to an u64 /
    // i64. The underlying enum type can differ
    size_t *values;
} fip_type_enum_t;

/// @typedef `fip_type_array_t`
/// @brief The struct representing fixed-size arrays
typedef struct {
    size_t size;
    struct fip_type_t *base_type;
} fip_type_array_t;

/// @typedef `fip_type_opaque_t`
/// @brief The struct representing a named opaque type
typedef struct {
    char name[128];
} fip_type_opaque_t;

/// @typedef `fip_type_tag_e`
/// @brief The enum containing all possible FIP types there are
typedef enum fip_type_tag_e : uint8_t {
    FIP_TYPE_PRIMITIVE,
    FIP_TYPE_PTR,
    FIP_TYPE_STRUCT,
    FIP_TYPE_RECURSIVE,
    FIP_TYPE_ENUM,
    FIP_TYPE_ARRAY,
    FIP_TYPE_OPAQUE,
} fip_type_tag_e;

/// @typedef `fip_type_t`
/// @brief The struct representing a type in FIP
typedef struct fip_type_t {
    bool is_mutable;
    fip_type_tag_e tag;
    union {
        fip_type_prim_e prim;
        fip_type_ptr_t ptr;
        fip_type_struct_t struct_t;
        fip_type_recursive_t recursive;
        fip_type_enum_t enum_t;
        fip_type_array_t array;
        fip_type_opaque_t opaque;
    } u;
} fip_type_t;

/*
 * ====================
 * SIGNATURE STRUCTURES
 * ====================
 */

/// @typedef `fip_sig_fn_arg_t`
/// @brief Struct representing a single arugment of a FIP-defined function
typedef struct {
    char name[128];
    fip_type_t type;
} fip_sig_fn_arg_t;

/// @typedef `fip_sig_fn_t`
/// @brief Struct representing the signature of a FIP-defined function
typedef struct {
    char name[128];
    size_t args_len;
    fip_sig_fn_arg_t *args;
    size_t rets_len;
    fip_type_t *rets;
} fip_sig_fn_t;

/// @typedef `fip_sig_data_field_t`
/// @brief Struct representing a single field of a data structure
typedef struct {
    char name[128];
    fip_type_t type;
} fip_sig_data_field_t;

/// @typedef `fip_sig_data_t`
/// @brief Struct representing the signature of FIP-defined data
typedef struct {
    char name[128];
    size_t field_count;
    fip_sig_data_field_t *fields;
} fip_sig_data_t;

/// @typedef `fip_sig_enum_value_t`
/// @brief Struct representing a single enum value
typedef struct {
    char tag[128];
    // The value is a size_t because it can be anything from i1 up to an u64 /
    // i64. The underlying enum type can differ
    size_t value;
} fip_sig_enum_value_t;

/// @typedef `fip_sig_enum_t`
/// @brief Struct representing the signature of a FIP-defined enum
typedef struct {
    char name[128];
    fip_type_prim_e type;
    size_t value_count;
    fip_sig_enum_value_t *values;
} fip_sig_enum_t;

/// @typedef `fip_sig_opaque_t`
/// @brief Struct representing the signature of a FIP-defined named opaque type
typedef struct {
    char name[128];
} fip_sig_opaque_t;

/// @typedef `fip_sig_tag__e`
/// @brief Enum of all possible signature types
typedef enum fip_sig_tag_e : uint8_t {
    FIP_SIG_UNKNOWN = 0,
    FIP_SIG_FUNCTION,
    FIP_SIG_DATA,
    FIP_SIG_ENUM,
    FIP_SIG_OPAQUE,
} fip_sig_tag_e;

/// @typedef `fip_sig_t`
/// @brief Struct representing a signature defined in FIP
typedef struct {
    fip_sig_tag_e tag;
    union {
        fip_sig_fn_t fn;
        fip_sig_data_t data;
        fip_sig_enum_t enum_t;
        fip_sig_opaque_t opaque;
    } u;
} fip_sig_t;

/// @typedef `fip_sig_list_t`
/// @brief Struct representing a list of signatures
typedef struct {
    size_t count;
    fip_sig_t sigs[];
} fip_sig_list_t;

/*
 * ==================
 * MESSAGE STRUCTURES
 * ==================
 */

/// @typedef `fip_msg_connect_request_t`
/// @brief Struct representing all information from a connection request
typedef struct {
    bool setup_ok;
    struct {
        uint8_t major;
        uint8_t minor;
        uint8_t patch;
    } version;
    char module_name[FIP_MAX_MODULE_NAME_LEN];
} fip_msg_connect_request_t;

/// @typedef `fip_msg_symbol_request_t`
/// @brief Struct representing the symbol request message
typedef struct {
    fip_sig_t sig;
} fip_msg_symbol_request_t;

/// @typedef `fip_msg_symbol_response_t`
/// @brief Struct representing the symbol response message
typedef struct {
    bool found;
    char module_name[FIP_MAX_MODULE_NAME_LEN];
    fip_sig_t sig;
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
    bool has_obj;
    bool compilation_failed;
    char module_name[FIP_MAX_MODULE_NAME_LEN];
    size_t path_count;
    char paths[FIP_PATHS_SIZE];
} fip_msg_object_response_t;

/// @typedef `fip_msg_tag_request_t`
/// @brief Struct representing the tag request message
typedef struct {
    char tag[128];
} fip_msg_tag_request_t;

/// @typedef `fip_msg_tag_present_response_t`
/// @brief Struct representing the tag present response message
typedef struct {
    bool is_present;
} fip_msg_tag_present_response_t;

/// @typedef `fip_msg_tag_symbol_response_t`
/// @brief Struct representing the tag symbol response message
typedef struct {
    bool is_empty;
    fip_sig_t sig;
} fip_msg_tag_symbol_response_t;

/// @typedef `fip_msg_kill_reason_e`
/// @brief The reason enum for the kill command
typedef enum fip_msg_kill_reason_e : uint8_t {
    FIP_KILL_FINISH = 0,
    FIP_KILL_VERSION_MISMATCH,
} fip_msg_kill_reason_e;

/// @typedef `fip_msg_kill_t`
/// @brief Struct representing the kill message
typedef struct {
    fip_msg_kill_reason_e reason;
} fip_msg_kill_t;

/// @typedef `fip_msg_tag_e`
/// @bfief Enum of all possible messages the FIP can handle
typedef enum fip_msg_tag_e : uint8_t {
    // Unknown message
    FIP_MSG_UNKNOWN = 0,
    // Slave trying to connect to master
    FIP_MSG_CONNECT_REQUEST,
    // Master requesting symbol resolution
    FIP_MSG_SYMBOL_REQUEST,
    // Slave response of FN_REQ
    FIP_MSG_SYMBOL_RESPONSE,
    // Master requesting all slaves to compile
    FIP_MSG_COMPILE_REQUEST,
    // Slave responding compilation with .o file
    FIP_MSG_OBJECT_RESPONSE,
    // The master requests that each IM searches for a certain tag and the
    // symbols from that tag
    FIP_MSG_TAG_REQUEST,
    // The IM's response to the request. It sends whether it contains that
    // searched-for tag. If it contains this tag then it will send all the tag
    // symbol responses it contains. The master then reads every single sent tag
    // symbol response of the IM
    FIP_MSG_TAG_PRESENT_RESPONSE,
    // The master request a single IM to give it the next symbol in it's symbol
    // list of the previously requested module tag. The slave will respond with
    // symbol responses until it reaches the end of the list, responding with an
    // empty symbol response
    FIP_MSG_TAG_NEXT_SYMBOL_REQUEST,
    // The IM's response to the tag request. It sends one symbol at a time and
    // whether that was the last symbol it provides
    FIP_MSG_TAG_SYMBOL_RESPONSE,
    // Kill command comes last
    FIP_MSG_KILL,
} fip_msg_tag_e;

/// @typedef `fip_msg_t`
/// @brief Struct representing sent / recieved FIP messages
typedef struct {
    fip_msg_tag_e tag;
    union {
        fip_msg_connect_request_t con_req;
        fip_msg_symbol_request_t sym_req;
        fip_msg_symbol_response_t sym_res;
        fip_msg_compile_request_t com_req;
        fip_msg_object_response_t obj_res;
        fip_msg_tag_request_t tag_req;
        fip_msg_tag_present_response_t tag_pres_res;
        fip_msg_tag_symbol_response_t tag_sym_res;
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
/// @param `log_level` The log level of the current message to print
/// @param `format` The format string of the printed message
/// @param `...` The variadic values to put into the formatted output
void fip_print(                      //
    const uint32_t id,               //
    const fip_log_level_e log_level, //
    const char *format,              //
    ...                              //
);

/// @function `fip_print_msg`
/// @brief Prints the given message from the given module ID
///
/// @param `message` The message to print
/// @param `id` The id of the process to print the message from
void fip_print_msg(const fip_msg_t *message, const uint32_t id);

/// @function `fip_encode_msg`
/// @brief Encodes a given message into a string and stores it in the internal
/// buffer
///
/// @param `message` The message to encode into the internal buffer
void fip_encode_msg(const fip_msg_t *message);

/// @function `fip_decode_msg`
/// @brief Tries to decode a message from the internal buffer and create a
/// message from it
///
/// @param `message` Pointer to the message where the result is stored
void fip_decode_msg(fip_msg_t *const message);

/// @function `fip_free_type`
/// @brief Frees the given type
///
/// @param `type` The type to free
void fip_free_type(fip_type_t *const type);

/// @function `fip_free_msg`
/// @brief Frees a given message
///
/// @param `message` The message to free
void fip_free_msg(fip_msg_t *const message);

/// @function `fip_free_sig_list`
/// @brief Frees a given signature list
///
/// @param `list` The list to free
void fip_free_sig_list(fip_sig_list_t *const list);

/// @function `fip_create_hash`
/// @brief Creates a 8 Byte character hash from the given file path to make
/// differentiating between different files predictable in size. Each character
/// in the 8 character hash is one of 63 possible characters (A-Z, a-z, 0-9, _)
/// which means that the 8 Byte hash can have roughly as many unique hashes as a
/// equivalent 48 bit number.
///
/// @param `hash` The buffer in which to write the hash
/// @param `file_path` The file path to turn into a 8 Byte hash
void fip_create_hash(char hash[FIP_PATH_SIZE], const char *file_path);

/// @function `fip_print_type`
/// @brief "Prints" a given type into the internal buffer which then can be used
/// to print the whole type in one fip_print call
///
/// @param `type` The type to print into the internal buffer
void fip_print_type(const fip_type_t *const type);

/// @function `fip_print_sig_fn`
/// @brief Prints a parsed function signature to the console
///
/// @param `sig` The function signature to print
/// @param `id` The id of the process in which the signature is printed
void fip_print_sig_fn(const fip_sig_fn_t *const sig, const uint32_t id);

/// @function `fip_print_sig_data`
/// @brief Prints a parsed data definition signature to the console
///
/// @param `sig` The data signature to print
/// @param `id` The id of the process in which the signature is printed
void fip_print_sig_data(const fip_sig_data_t *const sig, const uint32_t id);

/// @function `fip_print_sig_enum`
/// @brief Prints a parsed enum definition signature to the console
///
/// @param `sig` The enum signature to print
/// @param `id` The id of the process in which the signature is printed
void fip_print_sig_enum(const fip_sig_enum_t *const sig, const uint32_t id);

/// @function `fip_print_sig_opaque`
/// @brief Prints a parsed opaque type signature to the console
///
/// @param `sig` The opaque signature to print
/// @param `id` The id of the process in which the signature is printed
void fip_print_sig_opaque(const fip_sig_opaque_t *const sig, const uint32_t id);

/// @function `fip_clone_sig_fn`
/// @brief Clones a given function signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
void fip_clone_sig_fn(const fip_sig_fn_t *const src, fip_sig_fn_t *const dest);

/// @function `fip_clone_sig_data`
/// @brief Clones a given data signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
void fip_clone_sig_data(             //
    const fip_sig_data_t *const src, //
    fip_sig_data_t *const dest       //
);

/// @function `fip_clone_sig_enum`
/// @brief Clones a given enum signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
void fip_clone_sig_enum(             //
    const fip_sig_enum_t *const src, //
    fip_sig_enum_t *const dest       //
);

/// @function `fip_clone_sig_opaque`
/// @brief Clones a given opaque signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
void fip_clone_sig_opaque(             //
    const fip_sig_opaque_t *const src, //
    fip_sig_opaque_t *const dest       //
);

/// @function `fip_clone_type`
/// @brief Clones a given type from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The type to fill
void fip_clone_type(const fip_type_t *const src, fip_type_t *const dest);

/// @function `fip_execute_and_capture`
/// @brief Executes the given command and captures both stdout and stderr in the
/// output string. Returns the exit code of the executed command
///
/// @param `output` The output parameter where the output of the command gets
/// written to
/// @param `command` The command to execute
/// @return `int` The exit code of the executed command
int fip_execute_and_caputre(char **output, const char *command);

/*
 * ====================
 * MASTER FUNCTIONALITY
 * ====================
 */

#ifdef FIP_MASTER

#define FIP_MAX_ENABLED_MODULES 16
#define FIP_MAX_SLAVES 64

/// @typedef `fip_interop_modules_t`
/// @brief A list of all active interop modules spawned by the master
typedef struct {
    uint8_t active_count;
    pid_t pids[FIP_MAX_SLAVES];
} fip_interop_modules_t;

/// @typedef `fip_master_state_t`
/// @brief The structure containing the whole state of the entire master
typedef struct {
    FILE *slave_stdin[FIP_MAX_SLAVES];
    FILE *slave_stdout[FIP_MAX_SLAVES];
    FILE *slave_stderr[FIP_MAX_SLAVES];
    uint32_t slave_count;
    fip_msg_t responses[FIP_MAX_SLAVES];
    uint32_t response_count;
} fip_master_state_t;

/// @typedef `fip_tag_request_status_e`
/// @brief A simple enum desciring the exit code of the `fip_master_tag_request`
/// function
typedef enum fip_tag_request_status_e : uint8_t {
    FIP_TAG_REQUEST_STATUS_OK,
    FIP_TAG_REQUEST_STATUS_ERR_FAULTY,
    FIP_TAG_REQUEST_STATUS_ERR_UNKNOWN_TAG,
    FIP_TAG_REQUEST_STATUS_ERR_AMBIGUOUS_TAG,
    FIP_TAG_REQUEST_STATUS_ERR_WRITE,
} fip_tag_request_status_e;

/// @typedef `fip_tag_request_result_t`
/// @brief A structure containing the result type of the
/// `fip_master_tag_request` function being an exit status and a list of symbols
typedef struct {
    fip_tag_request_status_e status;
    fip_sig_list_t *list;
} fip_tag_request_result_t;

/// @typedef `fip_master_config`
/// @brief The structure containing the results of the parsed toml file
typedef struct {
    bool ok;
    char enabled_modules[FIP_MAX_ENABLED_MODULES][FIP_MAX_MODULE_NAME_LEN];
    uint8_t enabled_count;
} fip_master_config_t;

extern fip_master_state_t master_state;

/// @function `fip_copy_stream_lines`
/// @brief Copies all lines from the `src` stream into the `dest` stream. Only
/// copies full lines, and copies the lines as one unit. So, all leftover
/// characters are, well, left, in the `src` and only full lines are written to
/// `dest`
///
/// @param `src` The source stream from which lines are read
/// @param `dest` The destination stream to which lines are copied to
void fip_copy_stream_lines(FILE *const src, FILE *const dest);

/// @function `fip_print_slave_streams`
/// @brief Prints all the `stderr` streams from all slaves into the `stderr`
/// stream of the master, to gather all the debug output from all slaves
void fip_print_slave_streams();

/// @function `fip_spawn_interop_module`
/// @brief Creates a new interop module and adds it's process ID to the list of
/// modules in the interop modules parameter
///
/// @param `modules` A pointer to the structure containing all the module PIDs
/// @param `root_path` The root path of the project (the directory where the
/// .fip directory is contained). The `module` program will be started in this
/// directory
/// @param `module` The interop module to start
/// @return `bool` Whether the interop module process creation was successful
bool fip_spawn_interop_module(      //
    fip_interop_modules_t *modules, //
    const char *root_path,          //
    const char *module              //
);

/// @function `fip_terminate_all_slaves`
/// @brief Terminates all currently running slaves if they have not been
/// terminated yet
///
/// @param `modules` A pointer to the structure containing all the module PIDs
void fip_terminate_all_slaves(fip_interop_modules_t *modules);

/// @function `fip_master_init`
/// @brief Initializes the master for stdio-based communication
///
/// @param `modules` The interop modules structure containing slave PIDs
/// @return `bool` Whether initialization was successful
bool fip_master_init(fip_interop_modules_t *modules);

/// @function `fip_master_broadcast_message`
/// @brief Broadcasts a given message to stdout
///
/// @param `message` The message to send
void fip_master_broadcast_message(const fip_msg_t *message);

/// @function `fip_master_await_responses`
/// @brief Waits for all slaves to respond with a message from stdin
///
/// @param `responses` The responses of all slaves where the ID of the response
/// in the array corresponds to the ID of the slave itself
/// @param `response_count` How many responses we got
/// @param `expected_msg_tag` The tag of the expected message
/// @return `uint8_t` How many responses were faulty (unable to be read) or had
/// the wrong type
uint8_t fip_master_await_responses(      //
    fip_msg_t responses[FIP_MAX_SLAVES], //
    uint32_t *const response_count,      //
    const fip_msg_tag_e expected_msg_tag //
);

/// @function `fip_master_symbol_request`
/// @brief Broadcasts a symbol request message and then awaits all
/// symbol response messages and returns whether the requested
/// symbol was found
///
/// @param `message` The symbol request message to send
/// @return `bool` Whether the requested symbol was found
///
/// @note This function asserts the message type to be FIP_MSG_SYMBOL_REQUEST
bool fip_master_symbol_request(const fip_msg_t *message);

/// @function `fip_master_compile_request`
/// @brief Broadcasts a compile request message and then awaits
/// all object response messages and returns whether all modules
/// were able to compile their sources
///
/// @param `message` The compile request message to send
/// @return `bool` Whether all interop modules were able to compile their
/// source files
///
/// @note This function asserts the message type to be FIP_MSG_COMPILE_REQUEST
bool fip_master_compile_request(const fip_msg_t *message);

/// @function `fip_master_tag_request`
/// @brief Broadcasts a tag request message and then collects all the symbols of
/// all interop modules
///
/// @param `message` The tag request message to send
/// @return `fip_sig_list_t *` A list of all collected signatures from the tag
///
/// @note This function asserts the message type to be FIP_MSG_TAG_REQUEST
fip_tag_request_result_t fip_master_tag_request(const fip_msg_t *message);

/// @function `fip_master_receive_message_from`
/// @brief Reads a message from stdin from a given IM id and stores it in the
/// internal global buffer
///
/// @param `id` The id of the slave to get the message from
/// @return `bool` Whether a message was recieved
bool fip_master_receive_message_from(const uint32_t id);

/// @function `fip_master_send_message_to`
/// @brief Sends a message to the stdout of a given interop module
///
/// @param `id` The id of the slave to send the message to
/// @param `message` The message which will be sent
void fip_master_send_message_to(const uint32_t id, const fip_msg_t *message);

/// @function `fip_master_cleanup`
/// @brief Cleans up the master
void fip_master_cleanup();

/// @function `fip_master_load_config`
/// @brief Loads the master config from the file at the `config_path`
///
/// @param `config_path` The path to the `fip.toml` config file located in
/// `<ProjectPath>/.fip/config/.toml`
/// @return `fip_master_config_t` The loaded configuration
fip_master_config_t fip_master_load_config(const char *config_path);

#endif // End of #ifdef FIP_MASTER

#ifdef FIP_SLAVE

/*
 * ===================
 * SLAVE Functionality
 * ===================
 */

/// @function `fip_slave_init`
/// @brief Initializes the slave for stdio-based communication with named pipes
///
/// @param `slave_id` The ID of this slave process
/// @return `bool` Whether initialization was successful
bool fip_slave_init(const uint32_t slave_id);

/// @function `fip_slave_receive_message`
/// @brief Reads a message from stdin and stores it in the internal buffer
///
/// @return `bool` Whether a message was recieved
bool fip_slave_receive_message();

/// @function `fip_slave_send_message`
/// @brief Sends a message to stdout
///
/// @param `id` The id of the slave who tries to send the message
/// @param `message` The message which will be sent
void fip_slave_send_message(const uint32_t id, const fip_msg_t *message);

/// @function `fip_slave_cleanup`
/// @brief Cleans up the slave
void fip_slave_cleanup();

/// @function `fip_slave_load_config`
/// @brief Loads the slave config from the `.fip/config/X.toml` where `X` is
/// the name of the module (for example `fip-c`)
///
/// @param `id` The ID of the slave that tries to load the config
/// @param `module_name` The name of the module to load
/// @return `toml_result_t` The loaded configuration toml file. Interpreting the
/// content of this file is the responsibility of each interop module itself,
/// the FIP protocol itself stays purely language-independant
toml_result_t fip_slave_load_config( //
    const uint32_t id,               //
    const char *module_name          //
);

#endif // End of #ifdef FIP_SLAVE

#ifdef FIP_IMPLEMENTATION

#define FIP_LINE_BUF_SIZE 4096

/// @brief The global internal buffer for encoding, decoding and all that fun
/// stuff
static char buffer[FIP_MSG_SIZE] = {0};

/// @brief The global buffer write index
static size_t b_idx = 0;

static const char *fip_msg_type_str[] = {
    "FIP_MSG_UNKNOWN",
    "FIP_MSG_CONNECT_REQUEST",
    "FIP_MSG_SYMBOL_REQUEST",
    "FIP_MSG_SYMBOL_RESPONSE",
    "FIP_MSG_COMPILE_REQUEST",
    "FIP_MSG_OBJECT_RESPONSE",
    "FIP_MSG_TAG_REQUEST",
    "FIP_MSG_TAG_PRESENT_RESPONSE",
    "FIP_MSG_TAG_NEXT_SYMBOL_REQUEST",
    "FIP_MSG_TAG_SYMBOL_RESPONSE",
    "FIP_MSG_KILL",
};

/// @var `fip_type_names`
/// @brief A small array where the value at each enum index is the name of the
/// type at that enum value
const char *fip_type_names[] = {
    "void",
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

void fip_print(                      //
    const uint32_t id,               //
    const fip_log_level_e log_level, //
    const char *format,              //
    ...                              //
) {
    if (log_level > LOG_LEVEL) {
        return;
    }
    if (!format) {
        return;
    }

    // Variables for timestamp
    int year, month, day, hour, minute, second;
    long milliseconds, microseconds;

#ifdef _WIN32
    // Windows-specific time handling
    SYSTEMTIME st;
    FILETIME ft;

    // Get system time with precision
    GetSystemTimeAsFileTime(&ft);
    // Convert to SYSTEMTIME for easy access to components
    FileTimeToSystemTime(&ft, &st);

    // Extract date/time components
    year = st.wYear;
    month = st.wMonth;
    day = st.wDay;
    hour = st.wHour;
    minute = st.wMinute;
    second = st.wSecond;
    milliseconds = st.wMilliseconds;

    // Calculate microseconds from FILETIME (100-nanosecond intervals)
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert to microseconds, get remainder
    microseconds = (uli.QuadPart / 10) % 1000;
#else
    // Linux/POSIX-specific time handling
    struct timespec ts;
    clock_gettime(0, &ts);
    struct tm *tm_info = localtime(&ts.tv_sec);

    // Extract date/time components
    year = tm_info->tm_year + 1900;
    month = tm_info->tm_mon + 1;
    day = tm_info->tm_mday;
    hour = tm_info->tm_hour;
    minute = tm_info->tm_min;
    second = tm_info->tm_sec;

    // Calculate microseconds and milliseconds
    microseconds = ts.tv_nsec / 1000;
    milliseconds = microseconds / 1000;
    microseconds = microseconds % 1000;
#endif

    // Buffer for the whole message to fit into
    static char message[4096];
    memset(message, 0, sizeof(message));
    static char prefix[256];
    memset(prefix, 0, sizeof(prefix));
    static char timestamp[32];
    memset(timestamp, 0, sizeof(timestamp));

    // ANSI color constants
    static const char *const colors[] = {
        "",         // FIP_NONE = nothing
        "\033[31m", // FIP_ERROR = RED
        "\033[33m", // FIP_WARN = YELLOW
        "\033[0m",  // FIP_INFO = DEFAULT
        "\033[37m", // FIP_DEBUG = WHITE
        "\033[90m"  // FIP_TRACE = GREY
    };

    // Log level strings
    static const char *const level_names[] = {
        "NONE ", // FIP_NONE
        "ERROR", // FIP_ERROR
        "WARN ", // FIP_WARN
        "INFO ", // FIP_INFO
        "DEBUG", // FIP_DEBUG
        "TRACE"  // FIP_TRACE
    };

    // Format timestamp: YYYY-MM-DD_HH:MI:SS.MS.US (fixed width: 26 chars)
    snprintf(                                        //
        timestamp, sizeof(timestamp),                //
        "%04d-%02d-%02d_%02d:%02d:%02d.%03ld.%03ld", //
        year,                                        //
        month,                                       //
        day,                                         //
        hour,                                        //
        minute,                                      //
        second,                                      //
        milliseconds,                                //
        microseconds                                 //
    );

    // Build prefix with color, ID, timestamp, and log level
    if (id == 0) {
        snprintf(                                      //
            prefix, sizeof(prefix),                    //
            "[%sMaster\033[0m]  [%s] [%s%s\033[0m]: ", //
            colors[log_level],                         //
            timestamp,                                 //
            colors[log_level],                         //
            level_names[log_level]                     //
        );
    } else {
        snprintf(                                       //
            prefix, sizeof(prefix),                     //
            "[%sSlave %u\033[0m] [%s] [%s%s\033[0m]: ", //
            colors[log_level], id,                      //
            timestamp,                                  //
            colors[log_level],                          //
            level_names[log_level]                      //
        );
    }

    // Print the formatted message into the message buffer
    va_list args;
    va_start(args, format);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat=2"
    vsnprintf(message, sizeof(message), format, args);
#pragma GCC diagnostic pop
    va_end(args);

    // Print the prefix and the message to stderr
    fprintf(stderr, "%s%s\n", prefix, message);
    fflush(stderr);
}

void fip_print_msg(const fip_msg_t *message, const uint32_t id) {
    switch (message->tag) {
        case FIP_MSG_UNKNOWN:
            fip_print(id, FIP_DEBUG, "FIP_MSG_UNKNOWN: {}");
            break;
        case FIP_MSG_CONNECT_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_CONNECT_REQUEST: {");
            fip_print(id, FIP_DEBUG, "  .setup_ok: %d", //
                message->u.con_req.setup_ok             //
            );
            fip_print(id, FIP_DEBUG, "  .version: %d.%d:%d", //
                message->u.con_req.version.major,            //
                message->u.con_req.version.minor,            //
                message->u.con_req.version.patch             //
            );
            fip_print(id, FIP_DEBUG, "  .module_name: %s", //
                message->u.con_req.module_name             //
            );
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_SYMBOL_REQUEST: {");
            switch (message->u.sym_req.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    fip_print(id, FIP_DEBUG, "  .type: UNKNOWN");
                    break;
                case FIP_SIG_FUNCTION:
                    fip_print(id, FIP_DEBUG, "  .type: FUNCTION");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_fn(&message->u.sym_req.sig.u.fn, id);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SIG_DATA:
                    fip_print(id, FIP_DEBUG, "  .type: DATA");
                    fip_print(id, FIP_DEBUG, "  .signature: TODO");
                    break;
                case FIP_SIG_ENUM:
                    fip_print(id, FIP_DEBUG, "  .type: ENUM");
                    fip_print(id, FIP_DEBUG, "  .signature: TODO");
                    break;
                case FIP_SIG_OPAQUE:
                    fip_print(id, FIP_DEBUG, "  .type: OPAQUE");
                    fip_print(id, FIP_DEBUG, "  .signature: TODO");
                    break;
            }
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            fip_print(id, FIP_DEBUG, "FIP_MSG_SYMBOL_RESPONSE: {");
            fip_print(id, FIP_DEBUG, "  .found: %d", message->u.sym_res.found);
            fip_print(id, FIP_DEBUG, "  .module_name: %s", //
                message->u.sym_res.module_name             //
            );
            switch (message->u.sym_res.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    fip_print(id, FIP_DEBUG, "  .type: UNKNOWN");
                    break;
                case FIP_SIG_FUNCTION:
                    fip_print(id, FIP_DEBUG, "  .type: FUNCTION");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_fn(&message->u.sym_res.sig.u.fn, id);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SIG_DATA:
                    fip_print(id, FIP_DEBUG, "  .type: DATA");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_data(&message->u.sym_res.sig.u.data, id);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SIG_ENUM:
                    fip_print(id, FIP_DEBUG, "  .type: ENUM");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_enum(&message->u.sym_res.sig.u.enum_t, id);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SIG_OPAQUE:
                    fip_print(id, FIP_DEBUG, "  .type: OPAQUE");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_opaque(&message->u.sym_res.sig.u.opaque, id);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
            }
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_COMPILE_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_COMPILE_REQUEST: {");
            fip_print(id, FIP_DEBUG, "  .target: {");
            fip_print(id, FIP_DEBUG, "    .arch: %s", //
                message->u.com_req.target.arch        //
            );
            fip_print(id, FIP_DEBUG, "    .sub: %s", //
                message->u.com_req.target.sub        //
            );
            fip_print(id, FIP_DEBUG, "    .vendor: %s", //
                message->u.com_req.target.vendor        //
            );
            fip_print(id, FIP_DEBUG, "    .sys: %s", //
                message->u.com_req.target.sys        //
            );
            fip_print(id, FIP_DEBUG, "    .abi: %s", //
                message->u.com_req.target.abi        //
            );
            fip_print(id, FIP_DEBUG, "  }");
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_OBJECT_RESPONSE:
            fip_print(id, FIP_DEBUG, "FIP_MSG_OBJECT_RESPONSE: {");
            fip_print(id, FIP_DEBUG, "  .has_obj: %d", //
                message->u.obj_res.has_obj             //
            );
            fip_print(id, FIP_DEBUG, "  .compilation_failed: %d", //
                message->u.obj_res.compilation_failed             //
            );
            fip_print(id, FIP_DEBUG, "  .module_name: %s", //
                message->u.obj_res.module_name             //
            );
            fip_print(id, FIP_DEBUG, "  .path_count: %d", //
                message->u.obj_res.path_count             //
            );
            fip_print(id, FIP_DEBUG, "  .paths: %s", //
                message->u.obj_res.paths             //
            );
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_TAG_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_TAG_REQUEST: {");
            fip_print(id, FIP_DEBUG, "  .tag: %s", message->u.tag_req.tag);
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_TAG_PRESENT_RESPONSE:
            fip_print(id, FIP_DEBUG, "FIP_MSG_TAG_PRESENT_RESPONSE: {");
            fip_print(id, FIP_DEBUG, "  .is_present: %d", //
                message->u.tag_pres_res.is_present        //
            );
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_TAG_NEXT_SYMBOL_REQUEST: {}");
            break;
        case FIP_MSG_TAG_SYMBOL_RESPONSE:
            fip_print(id, FIP_DEBUG, "FIP_MSG_TAG_SYMBOL_RESPONSE: {");
            fip_print(id, FIP_DEBUG, "  .is_empty: %d", //
                message->u.tag_sym_res.is_empty         //
            );
            if (!message->u.tag_sym_res.is_empty) {
                switch (message->u.tag_sym_res.sig.tag) {
                    case FIP_SIG_UNKNOWN:
                        fip_print(id, FIP_DEBUG, "  .type: UNKNOWN");
                        break;
                    case FIP_SIG_FUNCTION:
                        fip_print(id, FIP_DEBUG, "  .type: FUNCTION");
                        fip_print_sig_fn(&message->u.tag_sym_res.sig.u.fn, id);
                        break;
                    case FIP_SIG_DATA:
                        fip_print(id, FIP_DEBUG, "  .type: DATA");
                        fip_print_sig_data(                        //
                            &message->u.tag_sym_res.sig.u.data, id //
                        );
                        break;
                    case FIP_SIG_ENUM:
                        fip_print(id, FIP_DEBUG, "  .type: ENUM");
                        fip_print_sig_enum(                          //
                            &message->u.tag_sym_res.sig.u.enum_t, id //
                        );
                        break;
                    case FIP_SIG_OPAQUE:
                        fip_print(id, FIP_DEBUG, "  .type: OPAQUE");
                        fip_print_sig_opaque(                        //
                            &message->u.tag_sym_res.sig.u.opaque, id //
                        );
                        break;
                }
            }
            fip_print(id, FIP_DEBUG, "}");
            break;
        case FIP_MSG_KILL:
            fip_print(id, FIP_DEBUG, "FIP_MSG_KILL: {");
            switch (message->u.kill.reason) {
                case FIP_KILL_FINISH:
                    fip_print(id, FIP_DEBUG, "  .reason: FINISH");
                    break;
                case FIP_KILL_VERSION_MISMATCH:
                    fip_print(id, FIP_DEBUG, "  .reason: VERSION_MISMATCH");
                    break;
            }
            fip_print(id, FIP_DEBUG, "}");
            break;
    }
}

void fip_encode_type(const fip_type_t *type) {
    buffer[b_idx++] = (char)type->is_mutable;
    buffer[b_idx++] = (char)type->tag;
    switch (type->tag) {
        case FIP_TYPE_PRIMITIVE:
            buffer[b_idx++] = (char)type->u.prim;
            break;
        case FIP_TYPE_PTR:
            fip_encode_type(type->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT: {
            const uint8_t type_name_len = strlen(type->u.struct_t.name);
            buffer[b_idx++] = type_name_len;
            if (type_name_len > 0) {
                memcpy(&buffer[b_idx], type->u.struct_t.name, type_name_len);
                b_idx += type_name_len;
            }
            buffer[b_idx++] = (char)type->u.struct_t.field_count;
            for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                fip_encode_type(&type->u.struct_t.fields[i]);
            }
            break;
        }
        case FIP_TYPE_RECURSIVE:
            buffer[b_idx++] = (char)type->u.recursive.levels_back;
            break;
        case FIP_TYPE_ENUM: {
            const uint8_t type_name_len = strlen(type->u.enum_t.name);
            buffer[b_idx++] = type_name_len;
            if (type_name_len > 0) {
                memcpy(&buffer[b_idx], type->u.enum_t.name, type_name_len);
                b_idx += type_name_len;
            }
            buffer[b_idx++] = (char)type->u.enum_t.bit_width;
            buffer[b_idx++] = (char)type->u.enum_t.is_signed;
            memcpy(&buffer[b_idx], &type->u.enum_t.value_count, sizeof(size_t));
            b_idx += sizeof(size_t);
            for (size_t i = 0; i < type->u.enum_t.value_count; i++) {
                memcpy(                                                       //
                    &buffer[b_idx], &type->u.enum_t.values[i], sizeof(size_t) //
                );
                b_idx += sizeof(size_t);
            }
            break;
        }
        case FIP_TYPE_ARRAY:
            memcpy(&buffer[b_idx], &type->u.array.size, sizeof(size_t));
            b_idx += sizeof(size_t);
            fip_encode_type(type->u.array.base_type);
            break;
        case FIP_TYPE_OPAQUE: {
            const uint8_t type_name_len = strlen(type->u.opaque.name);
            buffer[b_idx++] = type_name_len;
            if (type_name_len > 0) {
                memcpy(&buffer[b_idx], type->u.opaque.name, type_name_len);
                b_idx += type_name_len;
            }
            break;
        }
    }
}

void fip_encode_sig_fn(const fip_sig_fn_t *sig) {
    const uint8_t name_len = strlen(sig->name);
    buffer[b_idx++] = name_len;
    if (name_len > 0) {
        memcpy(&buffer[b_idx], sig->name, name_len);
        b_idx += name_len;
    }
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are uint8_t's annyway
    // because which function has more than 256 parameters or return types?
    memcpy(&buffer[b_idx], &sig->args_len, sizeof(size_t));
    b_idx += sizeof(size_t);
    for (size_t i = 0; i < sig->args_len; i++) {
        const uint8_t arg_name_len = strlen(sig->args[i].name);
        buffer[b_idx++] = arg_name_len;
        if (arg_name_len > 0) {
            memcpy(&buffer[b_idx], sig->args[i].name, arg_name_len);
            b_idx += arg_name_len;
        }
        buffer[b_idx++] = sig->args[i].type.is_mutable;
        fip_encode_type(&sig->args[i].type);
    }
    memcpy(&buffer[b_idx], &sig->rets_len, sizeof(size_t));
    b_idx += sizeof(size_t);
    for (size_t i = 0; i < sig->rets_len; i++) {
        buffer[b_idx++] = sig->rets[i].is_mutable;
        fip_encode_type(&sig->rets[i]);
    }
}

void fip_encode_sig_data(const fip_sig_data_t *sig) {
    const uint8_t name_len = strlen(sig->name);
    buffer[b_idx++] = name_len;
    if (name_len > 0) {
        memcpy(&buffer[b_idx], sig->name, name_len);
        b_idx += name_len;
    }
    memcpy(&buffer[b_idx], &sig->field_count, sizeof(size_t));
    b_idx += sizeof(size_t);
    // We store all value names first, then all value types
    for (size_t i = 0; i < sig->field_count; i++) {
        const uint8_t value_name_len = strlen(sig->fields[i].name);
        buffer[b_idx++] = value_name_len;
        if (value_name_len > 0) {
            memcpy(&buffer[b_idx], sig->fields[i].name, value_name_len);
            b_idx += value_name_len;
        }
    }
    for (size_t i = 0; i < sig->field_count; i++) {
        fip_encode_type(&sig->fields[i].type);
    }
}

void fip_encode_sig_enum(const fip_sig_enum_t *sig) {
    const size_t name_len = strlen(sig->name);
    buffer[b_idx++] = (char)name_len;
    if (name_len > 0) {
        memcpy(&buffer[b_idx], sig->name, name_len);
        b_idx += name_len;
    }
    buffer[b_idx++] = sig->type;
    memcpy(&buffer[b_idx], &sig->value_count, sizeof(size_t));
    b_idx += sizeof(size_t);
    // For enums we first store all tags to reduce padding needs
    for (size_t i = 0; i < sig->value_count; i++) {
        const uint8_t tag_len = strlen(sig->values[i].tag);
        buffer[b_idx++] = tag_len;
        if (tag_len > 0) {
            memcpy(&buffer[b_idx], sig->values[i].tag, tag_len);
            b_idx += tag_len;
        }
    }
    // And then we store all the values one after another
    for (size_t i = 0; i < sig->value_count; i++) {
        memcpy(&buffer[b_idx], &sig->values[i].value, sizeof(size_t));
        b_idx += sizeof(size_t);
    }
}

void fip_encode_sig_opaque(const fip_sig_opaque_t *sig) {
    const uint8_t name_len = strlen(sig->name);
    buffer[b_idx++] = name_len;
    if (name_len > 0) {
        memcpy(&buffer[b_idx], sig->name, name_len);
        b_idx += name_len;
    }
}

void fip_encode_msg(const fip_msg_t *message) {
    // Clear the buffer
    memset(buffer, 0, FIP_MSG_SIZE);
    // The message always starts with the length of the message as a 4 byte
    // uint32_teger and then the actual message follows. This is why the
    // 'idx' starts at 4, since the first 4 bytes need to be filled with the
    // size of the message itself
    // The first character in the buffer is the message type
    b_idx = 0;
    buffer[b_idx++] = message->tag;
    switch (message->tag) {
        case FIP_MSG_UNKNOWN:
            // Sending unknown or faulty message
            break;
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the version info followed by the
            // module name into the buffer and is done
            buffer[b_idx++] = (bool)message->u.con_req.setup_ok;
            buffer[b_idx++] = message->u.con_req.version.major;
            buffer[b_idx++] = message->u.con_req.version.minor;
            buffer[b_idx++] = message->u.con_req.version.patch;
            memcpy(&buffer[b_idx], message->u.con_req.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            b_idx += FIP_MAX_MODULE_NAME_LEN;
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            buffer[b_idx++] = message->u.sym_req.sig.tag;
            switch (message->u.sym_req.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    break;
                case FIP_SIG_FUNCTION:
                    fip_encode_sig_fn(&message->u.sym_req.sig.u.fn);
                    break;
                case FIP_SIG_DATA:
                    break;
                case FIP_SIG_ENUM:
                    break;
                case FIP_SIG_OPAQUE:
                    fip_encode_sig_opaque(&message->u.sym_req.sig.u.opaque);
                    break;
            }
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            // We place all elements into the buffer one by one until we come to
            // the union
            buffer[b_idx++] = message->u.sym_res.found;
            memcpy(&buffer[b_idx], message->u.sym_res.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            b_idx += FIP_MAX_MODULE_NAME_LEN;
            buffer[b_idx++] = message->u.sym_res.sig.tag;
            switch (message->u.sym_res.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    break;
                case FIP_SIG_FUNCTION:
                    fip_encode_sig_fn(&message->u.sym_res.sig.u.fn);
                    break;
                case FIP_SIG_DATA:
                    fip_encode_sig_data(                   //
                        &message->u.tag_sym_res.sig.u.data //
                    );
                    break;
                case FIP_SIG_ENUM:
                    fip_encode_sig_enum(                 //
                        &message->u.sym_res.sig.u.enum_t //
                    );
                    break;
                case FIP_SIG_OPAQUE:
                    fip_encode_sig_opaque(               //
                        &message->u.sym_res.sig.u.opaque //
                    );
                    break;
            }
            break;
        case FIP_MSG_COMPILE_REQUEST:
            // The compile request places each 16 byte piece of it in the buffer
            // directly
            memcpy(&buffer[b_idx], message->u.com_req.target.arch, 16);
            b_idx += 16;
            memcpy(&buffer[b_idx], message->u.com_req.target.sub, 16);
            b_idx += 16;
            memcpy(&buffer[b_idx], message->u.com_req.target.vendor, 16);
            b_idx += 16;
            memcpy(&buffer[b_idx], message->u.com_req.target.sys, 16);
            b_idx += 16;
            memcpy(&buffer[b_idx], message->u.com_req.target.abi, 16);
            b_idx += 16;
            break;
        case FIP_MSG_OBJECT_RESPONSE: {
            // The sizes of the buffers are known so we can put them into the
            // buffer directly
            buffer[b_idx++] = message->u.obj_res.has_obj;
            buffer[b_idx++] = message->u.obj_res.compilation_failed;
            memcpy(&buffer[b_idx], message->u.obj_res.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            b_idx += FIP_MAX_MODULE_NAME_LEN;
            const size_t path_count = message->u.obj_res.path_count;
            memcpy(&buffer[b_idx], &path_count, sizeof(size_t));
            b_idx += sizeof(size_t);
            const size_t paths_size = FIP_PATH_SIZE * path_count;
            memcpy(&buffer[b_idx], message->u.obj_res.paths, paths_size);
            b_idx += paths_size;
            break;
        }
        case FIP_MSG_TAG_REQUEST: {
            const uint8_t tag_len = strlen(message->u.tag_req.tag);
            buffer[b_idx++] = tag_len;
            memcpy(&buffer[b_idx], message->u.tag_req.tag, tag_len);
            b_idx += tag_len;
            break;
        }
        case FIP_MSG_TAG_PRESENT_RESPONSE:
            buffer[b_idx++] = message->u.tag_pres_res.is_present;
            break;
        case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
            break;
        case FIP_MSG_TAG_SYMBOL_RESPONSE:
            buffer[b_idx++] = message->u.tag_sym_res.is_empty;
            if (!message->u.tag_sym_res.is_empty) {
                // Only encode the content if it's not empty, to make the
                // message to send smaller in the empty case
                buffer[b_idx++] = message->u.tag_sym_res.sig.tag;
                switch (message->u.tag_sym_res.sig.tag) {
                    case FIP_SIG_UNKNOWN:
                        break;
                    case FIP_SIG_FUNCTION:
                        fip_encode_sig_fn(&message->u.tag_sym_res.sig.u.fn);
                        break;
                    case FIP_SIG_DATA:
                        fip_encode_sig_data(&message->u.tag_sym_res.sig.u.data);
                        break;
                    case FIP_SIG_ENUM:
                        fip_encode_sig_enum(                     //
                            &message->u.tag_sym_res.sig.u.enum_t //
                        );
                        break;
                    case FIP_SIG_OPAQUE:
                        fip_encode_sig_opaque(                   //
                            &message->u.tag_sym_res.sig.u.opaque //
                        );
                        break;
                }
            }
            break;
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            buffer[b_idx++] = message->u.kill.reason;
            break;
    }
    uint32_t msg_len = b_idx - sizeof(uint32_t);
    memcpy(&buffer[0], &msg_len, sizeof(uint32_t));
}

void fip_decode_type(fip_type_t *const type) {
    type->is_mutable = (bool)buffer[b_idx++];
    type->tag = (fip_type_tag_e)buffer[b_idx++];
    switch (type->tag) {
        case FIP_TYPE_PRIMITIVE:
            type->u.prim = (fip_type_prim_e)buffer[b_idx++];
            break;
        case FIP_TYPE_PTR:
            type->u.ptr.base_type = (fip_type_t *)malloc(sizeof(fip_type_t));
            fip_decode_type(type->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT: {
            const uint8_t type_name_len = buffer[b_idx++];
            memset(type->u.struct_t.name, 0, sizeof(type->u.struct_t.name));
            if (type_name_len > 0) {
                memcpy(type->u.struct_t.name, &buffer[b_idx], type_name_len);
                b_idx += type_name_len;
            }
            memcpy(                                                           //
                &type->u.struct_t.field_count, &buffer[b_idx], sizeof(size_t) //
            );
            b_idx += sizeof(size_t);
            if (type->u.struct_t.field_count > 0) {
                type->u.struct_t.fields = (fip_type_t *)malloc(       //
                    sizeof(fip_type_t) * type->u.struct_t.field_count //
                );
                for (size_t i = 0; i < type->u.struct_t.field_count; i++) {
                    fip_decode_type(&type->u.struct_t.fields[i]);
                }
            }
            break;
        }
        case FIP_TYPE_RECURSIVE:
            type->u.recursive.levels_back = (uint8_t)buffer[b_idx++];
            break;
        case FIP_TYPE_ENUM: {
            const uint8_t type_name_len = buffer[b_idx++];
            memset(type->u.enum_t.name, 0, sizeof(type->u.enum_t.name));
            if (type_name_len > 0) {
                memcpy(type->u.enum_t.name, &buffer[b_idx], type_name_len);
                b_idx += type_name_len;
            }
            type->u.enum_t.bit_width = buffer[b_idx++];
            type->u.enum_t.is_signed = buffer[b_idx++];
            memcpy(&type->u.enum_t.value_count, &buffer[b_idx], sizeof(size_t));
            b_idx += sizeof(size_t);
            type->u.enum_t.values = (size_t *)malloc(       //
                sizeof(size_t) * type->u.enum_t.value_count //
            );
            for (size_t i = 0; i < type->u.enum_t.value_count; i++) {
                memcpy(                                                       //
                    &type->u.enum_t.values[i], &buffer[b_idx], sizeof(size_t) //
                );
                b_idx += sizeof(size_t);
            }
            break;
        }
        case FIP_TYPE_ARRAY:
            memcpy(&type->u.array.size, &buffer[b_idx], sizeof(size_t));
            b_idx += sizeof(size_t);
            type->u.array.base_type = (fip_type_t *)malloc(sizeof(fip_type_t));
            fip_decode_type(type->u.array.base_type);
            break;
        case FIP_TYPE_OPAQUE: {
            const uint8_t type_name_len = buffer[b_idx++];
            memset(type->u.opaque.name, 0, sizeof(type->u.opaque.name));
            if (type_name_len > 0) {
                memcpy(type->u.opaque.name, &buffer[b_idx], type_name_len);
                b_idx += type_name_len;
            }
            break;
        }
    }
}

void fip_decode_sig_fn(fip_sig_fn_t *const sig) {
    const uint8_t name_len = buffer[b_idx++];
    memset(sig->name, 0, sizeof(sig->name));
    if (name_len > 0) {
        memcpy(sig->name, &buffer[b_idx], name_len);
        b_idx += name_len;
    }
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are uint8_t's annyway
    // because which function has more than 256 parameters or return types?
    memcpy(&sig->args_len, &buffer[b_idx], sizeof(size_t));
    b_idx += sizeof(size_t);
    if (sig->args_len > 0) {
        sig->args = (fip_sig_fn_arg_t *)malloc(      //
            sizeof(fip_sig_fn_arg_t) * sig->args_len //
        );
        for (size_t i = 0; i < sig->args_len; i++) {
            const uint8_t arg_name_len = buffer[b_idx++];
            memset(sig->args[i].name, 0, sizeof(sig->args[i].name));
            if (arg_name_len > 0) {
                memcpy(sig->args[i].name, &buffer[b_idx], arg_name_len);
                b_idx += arg_name_len;
            }
            sig->args[i].type.is_mutable = buffer[b_idx++];
            fip_decode_type(&sig->args[i].type);
        }
    } else {
        sig->args = NULL;
    }
    memcpy(&sig->rets_len, &buffer[b_idx], sizeof(size_t));
    b_idx += sizeof(size_t);
    if (sig->rets_len > 0) {
        sig->rets = (fip_type_t *)malloc(sizeof(fip_type_t) * sig->rets_len);
        for (size_t i = 0; i < sig->rets_len; i++) {
            sig->rets[i].is_mutable = buffer[b_idx++];
            fip_decode_type(&sig->rets[i]);
        }
    } else {
        sig->rets = NULL;
    }
}

void fip_decode_sig_data(fip_sig_data_t *const sig) {
    const uint8_t name_len = buffer[b_idx++];
    memset(sig->name, 0, sizeof(sig->name));
    if (name_len > 0) {
        memcpy(sig->name, &buffer[b_idx], name_len);
        b_idx += name_len;
    }
    memcpy(&sig->field_count, &buffer[b_idx], sizeof(size_t));
    b_idx += sizeof(size_t);
    sig->fields = (fip_sig_data_field_t *)malloc(       //
        sizeof(fip_sig_data_field_t) * sig->field_count //
    );
    for (size_t i = 0; i < sig->field_count; i++) {
        const uint8_t field_name_len = buffer[b_idx++];
        memset(sig->fields[i].name, 0, sizeof(sig->name));
        if (field_name_len > 0) {
            memcpy(sig->fields[i].name, &buffer[b_idx], field_name_len);
            b_idx += field_name_len;
        }
    }
    for (size_t i = 0; i < sig->field_count; i++) {
        fip_decode_type(&sig->fields[i].type);
    }
}

void fip_decode_sig_enum(fip_sig_enum_t *const sig) {
    const uint8_t name_len = buffer[b_idx++];
    memset(sig->name, 0, sizeof(sig->name));
    if (name_len > 0) {
        memcpy(sig->name, &buffer[b_idx], name_len);
        b_idx += name_len;
    }
    sig->type = (fip_type_prim_e)buffer[b_idx++];
    memcpy(&sig->value_count, &buffer[b_idx], sizeof(size_t));
    b_idx += sizeof(size_t);
    // For enums we first stored all tags to reduce padding needs
    sig->values = (fip_sig_enum_value_t *)malloc(         //
        sizeof(fip_sig_enum_value_t *) * sig->value_count //
    );
    for (size_t i = 0; i < sig->value_count; i++) {
        const uint8_t tag_len = buffer[b_idx++];
        if (tag_len > 0) {
            memcpy(sig->values[i].tag, &buffer[b_idx], tag_len);
            b_idx += tag_len;
        }
    }
    // And then we store all the values one after another
    for (size_t i = 0; i < sig->value_count; i++) {
        memcpy(&sig->values[i].value, &buffer[b_idx], sizeof(size_t));
        b_idx += sizeof(size_t);
    }
}

void fip_decode_sig_opaque(fip_sig_opaque_t *const sig) {
    const uint8_t name_len = buffer[b_idx++];
    memset(sig->name, 0, sizeof(sig->name));
    if (name_len > 0) {
        memcpy(sig->name, &buffer[b_idx], name_len);
        b_idx += name_len;
    }
}

void fip_decode_msg(fip_msg_t *const message) {
    memset(message, 0, sizeof(fip_msg_t));
    b_idx = 0;
    message->tag = (fip_msg_tag_e)buffer[b_idx++];
    switch (message->tag) {
        case FIP_MSG_UNKNOWN:
            // Received unknown or faulty message
            break;
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the versions into the buffer one by
            // one and is done
            message->u.con_req.setup_ok = (bool)buffer[b_idx++];
            message->u.con_req.version.major = buffer[b_idx++];
            message->u.con_req.version.minor = buffer[b_idx++];
            message->u.con_req.version.patch = buffer[b_idx++];
            memcpy(message->u.con_req.module_name, &buffer[b_idx],
                FIP_MAX_MODULE_NAME_LEN);
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            message->u.sym_req.sig.tag = (fip_sig_tag_e)buffer[b_idx++];
            switch (message->u.sym_req.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    break;
                case FIP_SIG_FUNCTION:
                    fip_decode_sig_fn(&message->u.sym_req.sig.u.fn);
                    break;
                case FIP_SIG_DATA:
                    fip_decode_sig_data(&message->u.sym_req.sig.u.data);
                    break;
                case FIP_SIG_ENUM:
                    fip_decode_sig_enum(&message->u.sym_req.sig.u.enum_t);
                    break;
                case FIP_SIG_OPAQUE:
                    fip_decode_sig_opaque(&message->u.sym_req.sig.u.opaque);
                    break;
            }
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            // We place all elements into the buffer one by one until we come to
            // the union
            message->u.sym_res.found = (bool)buffer[b_idx++];
            memcpy(message->u.sym_res.module_name, &buffer[b_idx],
                FIP_MAX_MODULE_NAME_LEN);
            b_idx += FIP_MAX_MODULE_NAME_LEN;
            message->u.sym_res.sig.tag = (fip_sig_tag_e)buffer[b_idx++];
            switch (message->u.sym_res.sig.tag) {
                case FIP_SIG_UNKNOWN:
                    break;
                case FIP_SIG_FUNCTION:
                    fip_decode_sig_fn(&message->u.sym_res.sig.u.fn);
                    break;
                case FIP_SIG_DATA:
                    fip_decode_sig_data(&message->u.sym_res.sig.u.data);
                    break;
                case FIP_SIG_ENUM:
                    fip_decode_sig_enum(&message->u.sym_res.sig.u.enum_t);
                    break;
                case FIP_SIG_OPAQUE:
                    fip_decode_sig_opaque(&message->u.sym_res.sig.u.opaque);
                    break;
            }
            break;
        case FIP_MSG_COMPILE_REQUEST:
            // The compile request places each 16 byte piece of it in the buffer
            // directly
            memcpy(message->u.com_req.target.arch, &buffer[b_idx], 16);
            b_idx += 16;
            memcpy(message->u.com_req.target.sub, &buffer[b_idx], 16);
            b_idx += 16;
            memcpy(message->u.com_req.target.vendor, &buffer[b_idx], 16);
            b_idx += 16;
            memcpy(message->u.com_req.target.sys, &buffer[b_idx], 16);
            b_idx += 16;
            memcpy(message->u.com_req.target.abi, &buffer[b_idx], 16);
            break;
        case FIP_MSG_OBJECT_RESPONSE: {
            // The sizes of the buffers are known so we can read them from the
            // buffer directly
            message->u.obj_res.has_obj = (bool)buffer[b_idx++];
            message->u.obj_res.compilation_failed = (bool)buffer[b_idx++];
            memcpy(message->u.obj_res.module_name, &buffer[b_idx],
                FIP_MAX_MODULE_NAME_LEN);
            b_idx += FIP_MAX_MODULE_NAME_LEN;
            size_t path_count = 0;
            memcpy(&path_count, &buffer[b_idx], sizeof(size_t));
            b_idx += sizeof(size_t);
            message->u.obj_res.path_count = path_count;
            const size_t paths_size = FIP_PATH_SIZE * path_count;
            memcpy(message->u.obj_res.paths, &buffer[b_idx], paths_size);
            break;
        }
        case FIP_MSG_TAG_REQUEST: {
            const uint8_t tag_len = buffer[b_idx++];
            memcpy(message->u.tag_req.tag, &buffer[b_idx], tag_len);
            b_idx += tag_len;
            break;
        }
        case FIP_MSG_TAG_PRESENT_RESPONSE:
            message->u.tag_pres_res.is_present = buffer[b_idx++];
            break;
        case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
            break;
        case FIP_MSG_TAG_SYMBOL_RESPONSE:
            message->u.tag_sym_res.is_empty = buffer[b_idx++];
            if (!message->u.tag_sym_res.is_empty) {
                // Only decode the content if it's not empty, to make the
                // message to send smaller in the empty case
                message->u.tag_sym_res.sig.tag = (fip_sig_tag_e)buffer[b_idx++];
                switch (message->u.tag_sym_res.sig.tag) {
                    case FIP_SIG_UNKNOWN:
                        break;
                    case FIP_SIG_FUNCTION:
                        fip_decode_sig_fn(&message->u.tag_sym_res.sig.u.fn);
                        break;
                    case FIP_SIG_DATA:
                        fip_decode_sig_data(&message->u.tag_sym_res.sig.u.data);
                        break;
                    case FIP_SIG_ENUM:
                        fip_decode_sig_enum(                     //
                            &message->u.tag_sym_res.sig.u.enum_t //
                        );
                        break;
                    case FIP_SIG_OPAQUE:
                        fip_decode_sig_opaque(                   //
                            &message->u.tag_sym_res.sig.u.opaque //
                        );
                        break;
                }
            }
            break;
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            message->u.kill.reason = (fip_msg_kill_reason_e)buffer[b_idx++];
            break;
    }
}

void fip_free_type(fip_type_t *const type) {
    switch (type->tag) {
        case FIP_TYPE_PRIMITIVE:
            break;
        case FIP_TYPE_PTR:
            fip_free_type(type->u.ptr.base_type);
            free(type->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT:
            if (type->u.struct_t.field_count > 0) {
                for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                    fip_free_type(&type->u.struct_t.fields[i]);
                }
                free(type->u.struct_t.fields);
            }
            break;
        case FIP_TYPE_RECURSIVE:
            break;
        case FIP_TYPE_ENUM:
            free(type->u.enum_t.values);
            break;
        case FIP_TYPE_ARRAY:
            fip_free_type(type->u.array.base_type);
            free(type->u.array.base_type);
            break;
        case FIP_TYPE_OPAQUE:
            break;
    }
}

void fip_free_msg(fip_msg_t *const message) {
    const fip_msg_tag_e msg_tag = message->tag;
    message->tag = FIP_MSG_UNKNOWN;
    switch (msg_tag) {
        case FIP_MSG_UNKNOWN:
            return;
        case FIP_MSG_CONNECT_REQUEST:
            message->u.con_req.version.major = 0;
            message->u.con_req.version.minor = 0;
            message->u.con_req.version.patch = 0;
            break;
        case FIP_MSG_SYMBOL_REQUEST: {
            fip_msg_symbol_request_t *const sym_req = &message->u.sym_req;
            switch (sym_req->sig.tag) {
                case FIP_SIG_UNKNOWN:
                    // Do nothing on already freed / unknwon symbol
                    break;
                case FIP_SIG_FUNCTION: {
                    sym_req->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_fn_t *const fn = &sym_req->sig.u.fn;
                    memset(fn->name, 0, sizeof(fn->name));
                    if (fn->args_len > 0) {
                        for (size_t i = 0; i < fn->args_len; i++) {
                            fip_free_type(&fn->args[i].type);
                        }
                        free(fn->args);
                    }
                    fn->args_len = 0;
                    fn->args = NULL;
                    if (fn->rets_len > 0) {
                        for (size_t i = 0; i < fn->rets_len; i++) {
                            fip_free_type(&fn->rets[i]);
                        }
                        free(fn->rets);
                    }
                    fn->rets_len = 0;
                    fn->rets = NULL;
                    break;
                }
                case FIP_SIG_DATA: {
                    sym_req->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_data_t *const data = &sym_req->sig.u.data;
                    memset(data->name, 0, sizeof(data->name));
                    if (data->field_count > 0) {
                        for (size_t i = 0; i < data->field_count; i++) {
                            fip_free_type(&data->fields[i].type);
                        }
                        free(data->fields);
                    }
                    data->field_count = 0;
                    data->fields = NULL;
                    break;
                }
                case FIP_SIG_ENUM: {
                    sym_req->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_enum_t *const enum_t = &sym_req->sig.u.enum_t;
                    memset(enum_t->name, 0, sizeof(enum_t->name));
                    enum_t->type = FIP_VOID;
                    if (enum_t->value_count > 0) {
                        free(enum_t->values);
                    }
                    enum_t->value_count = 0;
                    enum_t->values = NULL;
                    break;
                }
                case FIP_SIG_OPAQUE: {
                    sym_req->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_opaque_t *const opaque = &sym_req->sig.u.opaque;
                    memset(opaque->name, 0, sizeof(opaque->name));
                    break;
                }
            }
            break;
        }
        case FIP_MSG_SYMBOL_RESPONSE: {
            fip_msg_symbol_response_t *const sym_res = &message->u.sym_res;
            sym_res->found = false;
            memset(sym_res->module_name, 0, FIP_MAX_MODULE_NAME_LEN);
            switch (sym_res->sig.tag) {
                case FIP_SIG_UNKNOWN:
                    // Do nothing on already freed / unknwon symbol
                    break;
                case FIP_SIG_FUNCTION: {
                    sym_res->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_fn_t *const fn = &sym_res->sig.u.fn;
                    memset(fn->name, 0, sizeof(fn->name));
                    if (fn->args_len > 0) {
                        for (size_t i = 0; i < fn->args_len; i++) {
                            fip_free_type(&fn->args[i].type);
                        }
                        free(fn->args);
                    }
                    fn->args_len = 0;
                    fn->args = NULL;
                    if (fn->rets_len > 0) {
                        for (size_t i = 0; i < fn->rets_len; i++) {
                            fip_free_type(&fn->rets[i]);
                        }
                        free(fn->rets);
                    }
                    fn->rets_len = 0;
                    fn->rets = NULL;
                    break;
                }
                case FIP_SIG_DATA: {
                    sym_res->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_data_t *const data = &sym_res->sig.u.data;
                    memset(data->name, 0, sizeof(data->name));
                    if (data->field_count > 0) {
                        for (size_t i = 0; i < data->field_count; i++) {
                            fip_free_type(&data->fields[i].type);
                        }
                        free(data->fields);
                    }
                    data->field_count = 0;
                    data->fields = NULL;
                    break;
                }
                case FIP_SIG_ENUM: {
                    sym_res->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_enum_t *const enum_t = &sym_res->sig.u.enum_t;
                    memset(enum_t->name, 0, sizeof(enum_t->name));
                    enum_t->type = FIP_VOID;
                    if (enum_t->value_count > 0) {
                        free(message->u.sym_res.sig.u.enum_t.values);
                    }
                    enum_t->value_count = 0;
                    enum_t->values = NULL;
                    break;
                }
                case FIP_SIG_OPAQUE: {
                    sym_res->sig.tag = FIP_SIG_UNKNOWN;
                    fip_sig_opaque_t *const opaque = &sym_res->sig.u.opaque;
                    memset(opaque->name, 0, sizeof(opaque->name));
                    break;
                }
            }
            break;
        }
        case FIP_MSG_COMPILE_REQUEST:
            memset(message->u.com_req.target.arch, 0, 16);
            memset(message->u.com_req.target.sub, 0, 16);
            memset(message->u.com_req.target.vendor, 0, 16);
            memset(message->u.com_req.target.sys, 0, 16);
            memset(message->u.com_req.target.abi, 0, 16);
            break;
        case FIP_MSG_OBJECT_RESPONSE:
            memset(message->u.obj_res.module_name, 0, FIP_MAX_MODULE_NAME_LEN);
            memset(message->u.obj_res.paths, 0, FIP_PATHS_SIZE);
            break;
        case FIP_MSG_TAG_REQUEST:
            memset(message->u.tag_req.tag, 0, sizeof(message->u.tag_req.tag));
            break;
        case FIP_MSG_TAG_PRESENT_RESPONSE:
            break;
        case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
            break;
        case FIP_MSG_TAG_SYMBOL_RESPONSE: {
            fip_msg_tag_symbol_response_t *const tag_sym_res =
                &message->u.tag_sym_res;
            tag_sym_res->is_empty = false;
            switch (tag_sym_res->sig.tag) {
                case FIP_SIG_UNKNOWN:
                    break;
                case FIP_SIG_FUNCTION: {
                    fip_sig_fn_t *const fn = &tag_sym_res->sig.u.fn;
                    memset(fn->name, 0, sizeof(fn->name));
                    if (fn->args_len > 0) {
                        for (size_t i = 0; i < fn->args_len; i++) {
                            fip_free_type(&fn->args[i].type);
                        }
                        free(fn->args);
                    }
                    fn->args_len = 0;
                    if (fn->rets_len > 0) {
                        for (size_t i = 0; i < fn->rets_len; i++) {
                            fip_free_type(&fn->rets[i]);
                        }
                        free(fn->rets);
                    }
                    fn->rets_len = 0;
                    break;
                }
                case FIP_SIG_DATA: {
                    fip_sig_data_t *const data = &tag_sym_res->sig.u.data;
                    memset(data->name, 0, sizeof(data->name));
                    if (data->field_count > 0) {
                        for (size_t i = 0; i < data->field_count; i++) {
                            fip_free_type(&data->fields[i].type);
                        }
                        free(data->fields);
                    }
                    data->field_count = 0;
                    data->fields = NULL;
                    break;
                }
                case FIP_SIG_ENUM: {
                    fip_sig_enum_t *const enum_t = &tag_sym_res->sig.u.enum_t;
                    memset(enum_t->name, 0, sizeof(enum_t->name));
                    enum_t->type = FIP_VOID;
                    if (enum_t->value_count > 0) {
                        free(enum_t->values);
                    }
                    enum_t->value_count = 0;
                    enum_t->values = NULL;
                    break;
                }
                case FIP_SIG_OPAQUE: {
                    fip_sig_opaque_t *const opaque = &tag_sym_res->sig.u.opaque;
                    memset(opaque->name, 0, sizeof(opaque->name));
                    break;
                }
            }
            tag_sym_res->sig.tag = FIP_SIG_UNKNOWN;
            break;
        }
        case FIP_MSG_KILL:
            // The enum does not need to be changed at all
            break;
    }
}

void fip_free_sig_list(fip_sig_list_t *const list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        fip_sig_t *const sig = &list->sigs[i];
        switch (sig->tag) {
            case FIP_SIG_UNKNOWN:
                break;
            case FIP_SIG_FUNCTION: {
                fip_sig_fn_t *const fn = &sig->u.fn;
                memset(fn->name, 0, sizeof(fn->name));
                if (fn->args_len > 0) {
                    for (size_t j = 0; j < fn->args_len; j++) {
                        fip_free_type(&fn->args[j].type);
                    }
                    free(fn->args);
                }
                fn->args = NULL;
                if (fn->rets_len > 0) {
                    for (size_t j = 0; j < fn->rets_len; j++) {
                        fip_free_type(&fn->rets[j]);
                    }
                    free(fn->rets);
                }
                fn->rets = NULL;
                break;
            }
            case FIP_SIG_DATA: {
                fip_sig_data_t *const data = &sig->u.data;
                memset(data->name, 0, sizeof(data->name));
                if (data->field_count > 0) {
                    for (size_t j = 0; j < data->field_count; j++) {
                        fip_free_type(&data->fields[j].type);
                    }
                    free(data->fields);
                }
                data->field_count = 0;
                data->fields = NULL;
                break;
            }
            case FIP_SIG_ENUM: {
                fip_sig_enum_t *const enum_t = &sig->u.enum_t;
                memset(enum_t->name, 0, sizeof(enum_t->name));
                enum_t->type = FIP_VOID;
                if (enum_t->value_count > 0) {
                    free(enum_t->values);
                }
                enum_t->value_count = 0;
                enum_t->values = NULL;
                break;
            }
            case FIP_SIG_OPAQUE: {
                fip_sig_opaque_t *const opaque = &list->sigs[i].u.opaque;
                memset(opaque->name, 0, sizeof(opaque->name));
                break;
            }
        }
    }
}

void fip_create_hash(char hash[FIP_PATH_SIZE], const char *file_path) {
    // Valid characters: 1-9, A-Z, a-z (61 characters total)
    // I choose to remove the '0' char as a possible character to have 61 total
    // characters. This reduces the number of unique hashes only slighlty but it
    // makes the char count a prime number, which hopefully increases the hash
    // distribution, making overlaps much less likely to happen
    static const char charset[] = "123456789ABCDEFGHIJKLMNOPQRSTU"
                                  "VWXYZabcdefghijklmnopqrstuvwxyz";
    const int charset_size = 61;

    // Initialize hash buffer
    // The value '0' means default and because the hash will not contain a 0 as
    // it's not in it's char set we only need to check the first character to
    // tell whether the hashing function succeeded or failed. Hashes do not need
    // to end with a zero terminator, as we know the size of the hash at
    // compile-time (8 Bytes)
    hash[0] = '0';
    size_t path_len = strlen(file_path);
    if (path_len == 0) {
        return;
    }
    uint32_t seed = 2166136261U;
    for (const char *p = file_path; *p; p++) {
        seed ^= (unsigned char)*p;
        seed *= 16777619U;
    }
    for (int i = 0; i < 8; i++) {
        uint32_t pos_hash = seed ^ (i * 0x9e3779b9);
        pos_hash *= 0x85ebca6b;
        pos_hash ^= pos_hash >> 13;
        pos_hash *= 0xc2b2ae35;
        pos_hash ^= pos_hash >> 16;

        hash[i] = charset[pos_hash % charset_size];
        seed = pos_hash;
    }
}

void fip_print_type(const fip_type_t *const type) {
    if (b_idx == 0) {
        memset(buffer, 0, FIP_MSG_SIZE);
    }
    switch (type->tag) {
        case FIP_TYPE_PRIMITIVE: {
            const char *type_name = fip_type_names[type->u.prim];
            uint8_t type_len = (uint8_t)strlen(type_name);
            memcpy(&buffer[b_idx], type_name, type_len);
            b_idx += type_len;
            break;
        }
        case FIP_TYPE_PTR:
            fip_print_type(type->u.ptr.base_type);
            buffer[b_idx++] = '*';
            break;
        case FIP_TYPE_STRUCT:
            buffer[b_idx++] = '{';
            buffer[b_idx++] = ' ';
            for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                fip_print_type(&type->u.struct_t.fields[i]);
                if (i + 1 != type->u.struct_t.field_count) {
                    buffer[b_idx++] = ',';
                }
                buffer[b_idx++] = ' ';
            }
            buffer[b_idx++] = '}';
            break;
        case FIP_TYPE_RECURSIVE: {
            buffer[b_idx++] = '{';
            buffer[b_idx++] = 'R';
            buffer[b_idx++] = 'E';
            buffer[b_idx++] = 'C';
            buffer[b_idx++] = ':';
            uint8_t level = type->u.recursive.levels_back;
            uint8_t part_100 = level / 100;
            uint8_t part_10 = (level - part_100 * 100) / 10;
            uint8_t part_1 = level - part_100 * 100 - part_10 * 10;
            if (level >= 100) {
                buffer[b_idx++] = (char)('0' + part_100);
            }
            if (level >= 10) {
                buffer[b_idx++] = (char)('0' + part_10);
            }
            buffer[b_idx++] = (char)('0' + part_1);
            buffer[b_idx++] = '}';
            break;
        }
        case FIP_TYPE_ENUM: {
            buffer[b_idx++] = 'e';
            buffer[b_idx++] = 'n';
            buffer[b_idx++] = 'u';
            buffer[b_idx++] = 'm';
            buffer[b_idx++] = '(';
            if (type->u.enum_t.is_signed) {
                buffer[b_idx++] = 'i';
            } else {
                buffer[b_idx++] = 'u';
            }
            if (type->u.enum_t.bit_width > 10) {
                assert(type->u.enum_t.bit_width < 100);
                const uint8_t bw10 = type->u.enum_t.bit_width / 10;
                buffer[b_idx++] = '0' + bw10;
                const uint8_t bw1 = (type->u.enum_t.bit_width - (bw10 * 10));
                buffer[b_idx++] = '0' + bw1;
            } else {
                buffer[b_idx++] = '0' + type->u.enum_t.bit_width;
            }
            buffer[b_idx++] = ')';
            buffer[b_idx++] = '{';
            for (uint8_t i = 0; i < type->u.enum_t.value_count; i++) {
                uint64_t raw = (uint64_t)type->u.enum_t.values[i];
                uint8_t bw = type->u.enum_t.bit_width;

                // Compute mask for bw bits
                uint64_t mask = (bw == 64) //
                    ? UINT64_MAX           //
                    : ((1ull << bw) - 1ull);
                uint64_t v = raw & mask;

                if (type->u.enum_t.is_signed) {
                    // sign-extend v from bw bits to int64_t
                    int64_t sval;
                    if (bw == 64) {
                        // full 64-bit two's complement
                        sval = (int64_t)v;
                    } else {
                        uint64_t sign_bit = 1ull << (bw - 1);
                        if (v & sign_bit) {
                            // extend ones above bw
                            uint64_t extended = v | (~mask);
                            sval = (int64_t)extended;
                        } else {
                            sval = (int64_t)v;
                        }
                    }
                    // append signed decimal
                    int wrote =
                        snprintf(&buffer[b_idx], 20, "%lld", (long long)sval);
                    if (wrote < 0)
                        wrote = 0;
                    b_idx += wrote;
                } else {
                    // unsigned
                    unsigned long long uval = (unsigned long long)v;
                    int wrote = snprintf(&buffer[b_idx], 20, "%llu", uval);
                    if (wrote < 0)
                        wrote = 0;
                    b_idx += wrote;
                }

                // separator between values
                if (i + 1 < type->u.enum_t.value_count) {
                    buffer[b_idx++] = ',';
                    buffer[b_idx++] = ' ';
                }
            }
            buffer[b_idx++] = '}';
            break;
        }
        case FIP_TYPE_ARRAY: {
            fip_print_type(type->u.array.base_type);
            buffer[b_idx++] = '[';
            unsigned long long arr_len = (unsigned long long)type->u.array.size;
            int wrote = snprintf(&buffer[b_idx], 20, "%llu", arr_len);
            if (wrote < 0) {
                wrote = 0;
            }
            b_idx += wrote;
            buffer[b_idx++] = ']';
            break;
        }
        case FIP_TYPE_OPAQUE: {
            const size_t name_len = strlen(type->u.opaque.name);
            memcpy(&buffer[b_idx], type->u.opaque.name, name_len);
            b_idx += name_len;
            break;
        }
    }
}

void fip_print_sig_fn(const fip_sig_fn_t *const sig, const uint32_t id) {
    fip_print(id, FIP_DEBUG, "  Function Signature:");
    fip_print(id, FIP_DEBUG, "    name: %s", sig->name);
    memset(buffer, 0, FIP_MSG_SIZE);
    b_idx = 0;
    for (size_t i = 0; i < sig->args_len; i++) {
        b_idx = 0;
        fip_print_type(&sig->args[i].type);
        if (sig->args[i].type.is_mutable) {
            fip_print(                                   //
                id, FIP_DEBUG, "    arg[%u]: mut %s %s", //
                i, buffer, sig->args[i].name             //
            );
        } else {
            fip_print(                                     //
                id, FIP_DEBUG, "    arg[%u]: const %s %s", //
                i, buffer, sig->args[i].name               //
            );
        }
    }
    for (size_t i = 0; i < sig->rets_len; i++) {
        memset(buffer, 0, FIP_MSG_SIZE);
        b_idx = 0;
        fip_print_type(&sig->rets[i]);
        if (sig->rets[i].is_mutable) {
            fip_print(id, FIP_DEBUG, "    ret[%u]: mut %s", i, buffer);
        } else {
            fip_print(id, FIP_DEBUG, "    ret[%u]: const %s", i, buffer);
        }
    }
}

void fip_print_sig_data(const fip_sig_data_t *const sig, const uint32_t id) {
    fip_print(id, FIP_DEBUG, "  Data Signature:");
    fip_print(id, FIP_DEBUG, "    name: %s", sig->name);
    b_idx = 0;
    for (size_t i = 0; i < sig->field_count; i++) {
        fip_sig_data_field_t *const field = &sig->fields[i];
        memset(buffer, 0, FIP_MSG_SIZE);
        b_idx = 0;
        fip_print_type(&field->type);
        if (field->type.is_mutable) {
            fip_print(id, FIP_DEBUG, "    %s: mut %s", field->name, buffer);
        } else {
            fip_print(id, FIP_DEBUG, "    %s: const %s", field->name, buffer);
        }
    }
}

void fip_print_sig_enum(const fip_sig_enum_t *const sig, const uint32_t id) {
    fip_print(id, FIP_DEBUG, "  Enum Signature:");
    fip_print(id, FIP_DEBUG, "    name: %s", sig->name);
    for (size_t i = 0; i < sig->value_count; i++) {
        fip_sig_enum_value_t *const value = &sig->values[i];
        fip_print(id, FIP_DEBUG, "    %s: %u", value->tag, value->value);
    }
}

void fip_print_sig_opaque(             //
    const fip_sig_opaque_t *const sig, //
    const uint32_t id                  //
) {
    fip_print(id, FIP_DEBUG, "  Opaque Signature:");
    fip_print(id, FIP_DEBUG, "    name: %s", sig->name);
}

void fip_clone_sig_fn(const fip_sig_fn_t *const src, fip_sig_fn_t *const dest) {
    memcpy(dest->name, src->name, sizeof(src->name));
    dest->args_len = src->args_len;
    dest->args = NULL;
    if (src->args_len > 0) {
        dest->args = (fip_sig_fn_arg_t *)malloc(     //
            sizeof(fip_sig_fn_arg_t) * src->args_len //
        );
        for (size_t i = 0; i < src->args_len; i++) {
            strcpy(dest->args[i].name, src->args[i].name);
            fip_clone_type(&src->args[i].type, &dest->args[i].type);
        }
    }
    dest->rets_len = src->rets_len;
    dest->rets = NULL;
    if (src->rets_len > 0) {
        dest->rets = (fip_type_t *)malloc(sizeof(fip_type_t) * src->rets_len);
        for (size_t i = 0; i < src->rets_len; i++) {
            fip_clone_type(&src->rets[i], &dest->rets[i]);
        }
    }
}

void fip_clone_sig_data(             //
    const fip_sig_data_t *const src, //
    fip_sig_data_t *const dest       //
) {
    memcpy(dest->name, src->name, sizeof(src->name));
    dest->field_count = src->field_count;
    dest->fields = NULL;
    if (src->field_count > 0) {
        dest->fields = (fip_sig_data_field_t *)malloc(      //
            sizeof(fip_sig_data_field_t) * src->field_count //
        );
        for (size_t i = 0; i < src->field_count; i++) {
            memset(dest->fields[i].name, 0, sizeof(dest->fields[i].name));
            const size_t name_len = strlen(src->fields[i].name);
            if (name_len > 0) {
                memcpy(dest->fields[i].name, src->fields[i].name, name_len);
            }
            fip_clone_type(&src->fields[i].type, &dest->fields[i].type);
        }
    }
}

void fip_clone_sig_enum(             //
    const fip_sig_enum_t *const src, //
    fip_sig_enum_t *const dest       //
) {
    memcpy(dest->name, src->name, sizeof(src->name));
    dest->type = src->type;
    dest->value_count = src->value_count;
    dest->values = NULL;
    if (src->value_count > 0) {
        dest->values = (fip_sig_enum_value_t *)malloc(      //
            sizeof(fip_sig_enum_value_t) * src->value_count //
        );
        for (size_t i = 0; i < src->value_count; i++) {
            memcpy(                                //
                &dest->values[i], &src->values[i], //
                sizeof(fip_sig_enum_value_t)       //
            );
        }
    }
}

void fip_clone_sig_opaque(             //
    const fip_sig_opaque_t *const src, //
    fip_sig_opaque_t *const dest       //
) {
    memcpy(dest->name, src->name, sizeof(src->name));
}

void fip_clone_type(const fip_type_t *const src, fip_type_t *const dest) {
    dest->is_mutable = src->is_mutable;
    dest->tag = src->tag;
    switch (src->tag) {
        case FIP_TYPE_PRIMITIVE:
            dest->u.prim = src->u.prim;
            break;
        case FIP_TYPE_PTR:
            dest->u.ptr.base_type = (fip_type_t *)malloc(sizeof(fip_type_t));
            fip_clone_type(src->u.ptr.base_type, dest->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT: {
            const uint8_t type_name_len = strlen(src->u.struct_t.name);
            memset(dest->u.struct_t.name, 0, sizeof(dest->u.struct_t.name));
            if (type_name_len > 0) {
                memcpy(                    //
                    dest->u.struct_t.name, //
                    src->u.struct_t.name,  //
                    type_name_len          //
                );
            }
            dest->u.struct_t.field_count = src->u.struct_t.field_count;
            if (src->u.struct_t.field_count > 0) {
                dest->u.struct_t.fields = (fip_type_t *)malloc(      //
                    sizeof(fip_type_t) * src->u.struct_t.field_count //
                );
                for (uint8_t i = 0; i < src->u.struct_t.field_count; i++) {
                    fip_clone_type(                 //
                        &src->u.struct_t.fields[i], //
                        &dest->u.struct_t.fields[i] //
                    );
                }
            }
            break;
        }
        case FIP_TYPE_RECURSIVE:
            dest->u.recursive.levels_back = src->u.recursive.levels_back;
            break;
        case FIP_TYPE_ENUM: {
            const uint8_t type_name_len = strlen(src->u.enum_t.name);
            memset(dest->u.enum_t.name, 0, sizeof(dest->u.enum_t.name));
            if (type_name_len > 0) {
                memcpy(dest->u.enum_t.name, src->u.enum_t.name, type_name_len);
            }
            dest->u.enum_t.bit_width = src->u.enum_t.bit_width;
            dest->u.enum_t.is_signed = src->u.enum_t.bit_width;
            dest->u.enum_t.value_count = src->u.enum_t.value_count;
            if (src->u.enum_t.value_count > 0) {
                const size_t val_size =
                    sizeof(size_t) * src->u.enum_t.value_count;
                dest->u.enum_t.values = (size_t *)malloc(val_size);
                memcpy(dest->u.enum_t.values, src->u.enum_t.values, val_size);
            }
            break;
        }
        case FIP_TYPE_ARRAY:
            dest->u.array.size = src->u.array.size;
            dest->u.array.base_type = (fip_type_t *)malloc(sizeof(fip_type_t));
            fip_clone_type(src->u.array.base_type, dest->u.array.base_type);
            break;
        case FIP_TYPE_OPAQUE: {
            const uint8_t type_name_len = strlen(src->u.opaque.name);
            memset(dest->u.opaque.name, 0, sizeof(dest->u.opaque.name));
            if (type_name_len > 0) {
                memcpy(dest->u.opaque.name, src->u.opaque.name, type_name_len);
            }
            break;
        }
    }
}

int fip_execute_and_capture(char **output, const char *command) {
    if (!output || !command) {
        return -1;
    }

    *output = NULL;
    int exit_code = 0;

#ifdef _WIN32
    // Windows implementation
    HANDLE stdout_read, stdout_write;
    HANDLE stderr_read, stderr_write;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

    // Create pipes for capturing output
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        return -1;
    }

    // Make parent handles non-inheritable
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Create a modifiable copy of the command
    char *cmd_copy = (char *)malloc(strlen(command) + 1);
    strcpy(cmd_copy, command);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    if (CreateProcessA(NULL, cmd_copy, NULL, NULL, TRUE, 0, NULL, NULL, &si,
            &pi)) {
        // Close child ends
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
        CloseHandle(pi.hThread);

        // Read all output
        char buf[FIP_LINE_BUF_SIZE];
        DWORD bytes_read;
        size_t total_size = 0;
        size_t capacity = FIP_LINE_BUF_SIZE;
        *output = (char *)malloc(capacity);
        (*output)[0] = '\0';

        // Read from both stdout and stderr
        HANDLE handles[2] = {stdout_read, stderr_read};
        for (int i = 0; i < 2; i++) {
            while (ReadFile(handles[i], buf, sizeof(buf), &bytes_read, NULL) &&
                bytes_read > 0) {
                if (total_size + bytes_read >= capacity) {
                    capacity *= 2;
                    *output = (char *)realloc(*output, capacity);
                }
                memcpy(*output + total_size, buf, bytes_read);
                total_size += bytes_read;
                (*output)[total_size] = '\0';
            }
        }

        // Wait for process and get exit code
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code_dword;
        GetExitCodeProcess(pi.hProcess, &exit_code_dword);
        exit_code = (int)exit_code_dword;

        CloseHandle(pi.hProcess);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
    } else {
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        exit_code = -1;
    }

    free(cmd_copy);
#else
    // Linux implementation using popen with combined stdout/stderr
    char *popen_cmd = (char *)malloc(strlen(command) + 10);
    sprintf(popen_cmd, "(%s) 2>&1", command);

    FILE *fp = popen(popen_cmd, "r");
    if (!fp) {
        free(popen_cmd);
        return -1;
    }

    // Read all output
    char buf[FIP_LINE_BUF_SIZE];
    size_t total_size = 0;
    size_t capacity = FIP_LINE_BUF_SIZE;
    *output = (char *)malloc(capacity);
    (*output)[0] = '\0';

    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (total_size + bytes_read >= capacity) {
            capacity *= 2;
            *output = (char *)realloc(*output, capacity);
        }
        memcpy(*output + total_size, buf, bytes_read);
        total_size += bytes_read;
        (*output)[total_size] = '\0';
    }

    exit_code = pclose(fp);
    if (exit_code != -1) {
        exit_code = WEXITSTATUS(exit_code);
    }

    free(popen_cmd);
#endif

    return exit_code;
}

/*
 * ======================================
 * PLATFORM-AGNOSTIC STDIO FUNCTIONS
 * ======================================
 */

#ifdef FIP_MASTER

void fip_copy_stream_lines(FILE *const src, FILE *const dest) {
    if (!src || !dest) {
        fprintf(stderr, "fip_copy_stream_lines: NULL argument\n");
        abort();
    }

    char buf[FIP_LINE_BUF_SIZE];

#ifdef _WIN32
    // Windows approach - check if data is available
    HANDLE handle = (HANDLE)_get_osfhandle(fileno(src));
    if (handle == INVALID_HANDLE_VALUE) {
        return; // Can't get handle, just return
    }

    DWORD bytes_available = 0;
    DWORD bytes_read = 0;

    // For pipes, check if data is available
    if (PeekNamedPipe(handle, NULL, 0, NULL, &bytes_available, NULL)) {
        if (bytes_available > 0) {
            // Data is available, read it
            size_t to_read = (bytes_available < sizeof(buf) - 1)
                ? bytes_available
                : sizeof(buf) - 1;
            if (ReadFile(handle, buf, to_read, &bytes_read, NULL)) {
                buf[bytes_read] = '\0';
                fputs(buf, dest);
                fflush(dest);
            }
        }
    }
#else
    // Linux approach - use fcntl
    int fd = fileno(src);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        ssize_t bytes_read;
        while ((bytes_read = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[bytes_read] = '\0';
            fputs(buf, dest);
            fflush(dest);
        }

        // Restore original flags
        fcntl(fd, F_SETFL, flags);
    }
#endif

    // Clear any error flags
    clearerr(src);
}

void fip_print_slave_streams() {
    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        fip_copy_stream_lines(master_state.slave_stderr[i], stderr);
    }
}

void fip_master_broadcast_message(const fip_msg_t *message) {
    fip_print(0, FIP_INFO, "Broadcasting message to %d slaves",
        master_state.slave_count);
    fip_encode_msg(message);
    uint32_t msg_len;
    memcpy(&msg_len, buffer, sizeof(uint32_t));

    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (master_state.slave_stdin[i]) {
            size_t written =
                fwrite(buffer, 1, msg_len + 4, master_state.slave_stdin[i]);
            if (written != (msg_len + 4)) {
                fip_print(0, FIP_WARN, "Failed to write message to slave %d",
                    i + 1);
                continue;
            }
            fip_print(0, FIP_DEBUG, "Sent message to slave %d", i + 1);
            fflush(master_state.slave_stdin[i]);
        }
    }
}

bool fip_master_symbol_request(const fip_msg_t *message) {
    assert(message->tag == FIP_MSG_SYMBOL_REQUEST);
    fip_master_broadcast_message(message);
    uint8_t wrong_msg_count = fip_master_await_responses(master_state.responses,
        &master_state.response_count, FIP_MSG_SYMBOL_RESPONSE);
    if (wrong_msg_count > 0) {
        fip_print(0, FIP_WARN, "Received %u wrong messages", wrong_msg_count);
    }

    bool symbol_found = false;
    for (uint8_t i = 0; i < master_state.response_count; i++) {
        fip_print_msg(&master_state.responses[i], 0);
        if (master_state.responses[i].tag == FIP_MSG_SYMBOL_RESPONSE &&
            master_state.responses[i].u.sym_res.found) {
            symbol_found = true;
        }
    }

    if (symbol_found) {
        fip_print(0, FIP_INFO, "Requested symbol found");
    } else {
        fip_print(0, FIP_WARN, "Requested symbol not found");
    }
    return symbol_found;
}

bool fip_master_compile_request(const fip_msg_t *message) {
    assert(message->tag == FIP_MSG_COMPILE_REQUEST);
    fip_master_broadcast_message(message);
    uint8_t wrong_msg_count = fip_master_await_responses(master_state.responses,
        &master_state.response_count, FIP_MSG_OBJECT_RESPONSE);
    if (wrong_msg_count > 0) {
        fip_print(0, FIP_WARN, "Received %u faulty messages", wrong_msg_count);
    }

    for (uint8_t i = 0; i < master_state.response_count; i++) {
        const fip_msg_t *response = &master_state.responses[i];
        if (response->tag != FIP_MSG_OBJECT_RESPONSE) {
            fip_print(0, FIP_ERROR, "Wrong message as response from slave %d",
                i);
            return false;
        }
        if (response->u.obj_res.has_obj) {
            fip_print(0, FIP_INFO, "Object response from module: %s",
                response->u.obj_res.module_name);
            fip_print(0, FIP_DEBUG, "Paths: %s", response->u.obj_res.paths);
        } else {
            fip_print(0, FIP_INFO, "Object response has no objects");
        }
    }
    return true;
}

fip_tag_request_result_t fip_master_tag_request(const fip_msg_t *message) {
    assert(message->tag == FIP_MSG_TAG_REQUEST);
    fip_master_broadcast_message(message);

    // Await which slave has the tag
    uint8_t wrong_msg_count = fip_master_await_responses( //
        master_state.responses,                           //
        &master_state.response_count,                     //
        FIP_MSG_TAG_PRESENT_RESPONSE                      //
    );
    if (wrong_msg_count > 0) {
        fip_print(0, FIP_ERROR, "Received %u faulty messages", wrong_msg_count);
        return (fip_tag_request_result_t){
            .status = FIP_TAG_REQUEST_STATUS_ERR_FAULTY,
            .list = NULL,
        };
    }

    uint8_t module_with_tag_count = 0;
    uint8_t module_with_tag_id = 0;
    for (uint8_t i = 0; i < master_state.response_count; i++) {
        assert(master_state.responses[i].tag == FIP_MSG_TAG_PRESENT_RESPONSE);
        if (master_state.responses[i].u.tag_pres_res.is_present) {
            module_with_tag_count++;
            module_with_tag_id = i;
        }
    }

    if (module_with_tag_count == 0) {
        fip_print(0, FIP_INFO, "No module owns tag %s", message->u.tag_req.tag);
        return (fip_tag_request_result_t){
            .status = FIP_TAG_REQUEST_STATUS_ERR_UNKNOWN_TAG,
            .list = NULL,
        };
    }
    if (module_with_tag_count > 1) {
        fip_print(                                              //
            0, FIP_ERROR, "Tag %s present in more than one IM", //
            message->u.tag_req.tag                              //
        );
        return (fip_tag_request_result_t){
            .status = FIP_TAG_REQUEST_STATUS_ERR_AMBIGUOUS_TAG,
            .list = NULL,
        };
    }

    // Get the stdout and stdin of the slave we got the tag id from
    uint8_t slave_index = module_with_tag_id;
    FILE *slave_in = master_state.slave_stdin[module_with_tag_id];

    // We keep sending `FIP_MSG_TAG_NEXT_SYMBOL_REQUEST` to the slave and we
    // will recieve `FIP_MSG_TAG_SYMBOL_RESPONSE` messages from the slave until
    // the symbol is empty, indicating the end of the list. We first tell the
    // slave to "give" us the next message, then it sends the message contianing
    // the empty symbol.
    fip_sig_list_t *sig_list = (fip_sig_list_t *)malloc(sizeof(fip_sig_list_t));
    sig_list->count = 0;
    while (true) {
        // Send the next symbol request message to the slave
        fip_msg_t request;
        memset(&request, 0, sizeof(fip_msg_t));
        request.tag = FIP_MSG_TAG_NEXT_SYMBOL_REQUEST;
        fip_encode_msg(&request);
        uint32_t msg_len;
        memcpy(&msg_len, buffer, sizeof(uint32_t));
        size_t written_bytes = fwrite(buffer, 1, msg_len + 4, slave_in);
        if (written_bytes != msg_len + 4) {
            fip_print(0, FIP_ERROR, "Failed to write message to slave");
            return (fip_tag_request_result_t){
                .status = FIP_TAG_REQUEST_STATUS_ERR_WRITE,
                .list = sig_list,
            };
        }
        fflush(slave_in);

        // Wait for the response of the slave
        // Simple timeout using non-blocking read
        while (!fip_master_receive_message_from(module_with_tag_id)) {
            fip_print_slave_streams();
            fip_print(0, FIP_WARN, "No message from slave %u yet...",
                module_with_tag_id);
        }
        fip_print_slave_streams();

        // Decode the slave's message
        fip_msg_t incoming;
        fip_decode_msg(&incoming);
        if (incoming.tag != FIP_MSG_TAG_SYMBOL_RESPONSE) {
            fip_print(0, FIP_ERROR,
                "Received unexpected response from slave %u: %s (expected %s)",
                slave_index + 1, fip_msg_type_str[incoming.tag],
                fip_msg_type_str[FIP_MSG_TAG_SYMBOL_RESPONSE]);
            fip_print_slave_streams();
            break;
        }
        // Handle end of symbol list
        if (incoming.u.tag_sym_res.is_empty) {
            fip_print(0, FIP_DEBUG, "Slave %u indicated end of symbol list",
                slave_index + 1);
            fip_free_msg(&incoming);
            break;
        }
        // Append a new entry to sig_list
        sig_list = (fip_sig_list_t *)realloc(sig_list,    //
            sizeof(fip_sig_list_t) +                      //
                sizeof(fip_sig_t) * (sig_list->count + 1) //
        );
        // Pointer to the freshly appended fip_sig_t
        fip_sig_t *const last_sig = &sig_list->sigs[sig_list->count];
        memset(last_sig, 0, sizeof(fip_sig_t));
        // Store the symbol type
        last_sig->tag = incoming.u.tag_sym_res.sig.tag;
        switch (incoming.u.tag_sym_res.sig.tag) {
            case FIP_SIG_UNKNOWN:
                break;
            case FIP_SIG_FUNCTION: {
                fip_clone_sig_fn(                                     //
                    &incoming.u.tag_sym_res.sig.u.fn, &last_sig->u.fn //
                );
                break;
            }
            case FIP_SIG_DATA: {
                fip_clone_sig_data(                                       //
                    &incoming.u.tag_sym_res.sig.u.data, &last_sig->u.data //
                );
                break;
            }
            case FIP_SIG_ENUM: {
                fip_clone_sig_enum(                       //
                    &incoming.u.tag_sym_res.sig.u.enum_t, //
                    &last_sig->u.enum_t                   //
                );
                break;
            }
            case FIP_SIG_OPAQUE: {
                fip_clone_sig_opaque(                     //
                    &incoming.u.tag_sym_res.sig.u.opaque, //
                    &last_sig->u.opaque                   //
                );
                break;
            }
        }
        sig_list->count++;
        fip_free_msg(&incoming);
    }
    return (fip_tag_request_result_t){
        .status = FIP_TAG_REQUEST_STATUS_OK,
        .list = sig_list,
    };
}

bool fip_master_receive_message_from(const uint32_t id) {
    FILE *slave_stdout = master_state.slave_stdout[id];
    if (slave_stdout == NULL) {
        fip_print(0, FIP_ERROR, "Cannot receive msg from nonexistent slave %u",
            id);
    }
    uint32_t msg_len;
    if (fread(&msg_len, 1, 4, slave_stdout) != 4) {
        return false;
    }
    if (msg_len == 0 || msg_len > FIP_MSG_SIZE - 4) {
        return false;
    }
    memset(buffer, 0, FIP_MSG_SIZE);
    if (fread(buffer, 1, msg_len, slave_stdout) != msg_len) {
        return false;
    }
    return true;
}

void fip_master_send_message_to(uint32_t id, const fip_msg_t *message) {
    FILE *slave_stdin = master_state.slave_stdin[id];
    if (slave_stdin == NULL) {
        fip_print(0, FIP_ERROR, "Cannot send msg to nonexistent slave %u", id);
    }
    fip_encode_msg(message);
    uint32_t msg_len;
    memcpy(&msg_len, buffer, sizeof(uint32_t));
    size_t written_bytes = fwrite(buffer, 1, msg_len + 4, slave_stdin);
    if (written_bytes != msg_len + 4) {
        fip_print(0, FIP_ERROR, "Failed to write message");
        return;
    }
    fip_print(0, FIP_INFO, "Successfully sent message of %u bytes", msg_len);
    fflush(slave_stdin);
}

void fip_master_cleanup() {
    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (master_state.slave_stdin[i]) {
            fclose(master_state.slave_stdin[i]);
            master_state.slave_stdin[i] = NULL;
        }
        if (master_state.slave_stdout[i]) {
            fclose(master_state.slave_stdout[i]);
            master_state.slave_stdout[i] = NULL;
        }
        if (master_state.slave_stderr[i]) {
            fip_copy_stream_lines(master_state.slave_stderr[i], stderr);
            fclose(master_state.slave_stderr[i]);
            master_state.slave_stderr[i] = NULL;
        }
    }
    master_state.slave_count = 0;
    fip_print(0, FIP_INFO, "Master cleaned up");
}

#endif // End of platform-agnostic master functions

#ifdef FIP_SLAVE

bool fip_slave_receive_message() {
    uint32_t msg_len;
    if (fread(&msg_len, 1, 4, stdin) != 4) {
        return false;
    }
    if (msg_len == 0 || msg_len > FIP_MSG_SIZE - 4) {
        return false;
    }
    memset(buffer, 0, FIP_MSG_SIZE);
    if (fread(buffer, 1, msg_len, stdin) != msg_len) {
        return false;
    }
    return true;
}

void fip_slave_send_message(uint32_t id, const fip_msg_t *message) {
    fip_encode_msg(message);
    uint32_t msg_len;
    memcpy(&msg_len, buffer, sizeof(uint32_t));
    size_t written_bytes = fwrite(buffer, 1, msg_len + 4, stdout);
    if (written_bytes != msg_len + 4) {
        fip_print(id, FIP_ERROR, "Failed to write message");
        return;
    }
    fip_print(id, FIP_INFO, "Successfully sent message of %u bytes", msg_len);
    fflush(stdout);
}

void fip_slave_open_log(const uint32_t id) {
    // Redirect stderr to a log file inside the project's `.fip` directory so
    // that the slave never blocks on the stderr pipe it shares with the master
    // (the master only drains it after the blocking stdout read, so a slave
    // which floods stderr would otherwise deadlock with it).
    char path[128] = {0};
#ifdef _WIN32
    CreateDirectoryA(".fip", NULL);
    CreateDirectoryA(".fip\\log", NULL);
    snprintf(path, sizeof(path), ".fip\\log\\slave_%u.log", id);
#else
    mkdir(".fip", 0777);
    mkdir(".fip/log", 0777);
    snprintf(path, sizeof(path), ".fip/log/slave_%u.log", id);
#endif
    if (freopen(path, "a", stderr) != NULL) {
        // Make sure every write is flushed immediately so the log is always
        // up to date (stderr becomes fully buffered after freopen)
        setvbuf(stderr, NULL, _IONBF, 0);
        fip_print(id, FIP_INFO, "Logging to file: %s", path);
    } else {
        fip_print(                                                         //
            id, FIP_WARN, "Could not open log file %s, using stderr", path //
        );
    }
}

void fip_slave_cleanup() {
    fip_print(1, FIP_INFO, "Slave cleaned up");
}

#endif // End of platform-agnostic slave functions

/*
 * ======================
 * WINDOWS IMPLEMENTATION
 * ======================
 */

#ifdef _WIN32

/*
 * =============================
 * WINDOWS MASTER IMPLEMENTATION
 * =============================
 */

#ifdef FIP_MASTER

typedef struct {
    HANDLE process;
    HANDLE stdin_write;
    HANDLE stdout_read;
    HANDLE stderr_read;
    pid_t pid; // For compatibility with existing code
} win_process_info_t;

static win_process_info_t win_processes[FIP_MAX_SLAVES];

bool fip_spawn_interop_module(      //
    fip_interop_modules_t *modules, //
    const char *root_path,          //
    const char *module              //
) {
    char id[8] = {0};
    char ll[8] = {0};
    snprintf(id, 8, "%d", modules->active_count + 1);
    snprintf(ll, 8, "%d", LOG_LEVEL);

    fip_print(0, FIP_INFO, "Spawning slave %s...", id);

    // Create pipes for communication
    HANDLE stdin_read, stdin_write;
    HANDLE stdout_read, stdout_write;
    HANDLE stderr_read, stderr_write;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        fip_print(0, FIP_ERROR, "Failed to create pipes for slave %s", id);
        return false;
    }

    // Make sure parent handles are not inherited
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Create command line
    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s %s", module, id, ll);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    HANDLE parent_std[3];
    DWORD parent_std_flags[3];
    int parent_std_count = 0;
    const HANDLE parent_std_ids[3] = {
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
    };
    for (int i = 0; i < 3; i++) {
        if (                                             //
            parent_std_ids[i] == NULL                    //
            || parent_std_ids[i] == INVALID_HANDLE_VALUE //
        ) {
            continue;
        }
        DWORD flags = 0;
        if (GetHandleInformation(parent_std_ids[i], &flags)) {
            parent_std[parent_std_count] = parent_std_ids[i];
            parent_std_flags[parent_std_count] = flags;
            parent_std_count++;
            SetHandleInformation(parent_std_ids[i], HANDLE_FLAG_INHERIT, 0);
        }
    }

    bool spawned = CreateProcessA(                                    //
        NULL, cmdline, NULL, NULL, TRUE, 0, NULL, root_path, &si, &pi //
    );
    for (int i = 0; i < parent_std_count; i++) {
        SetHandleInformation(                         //
            parent_std[i], HANDLE_FLAG_INHERIT,       //
            parent_std_flags[i] & HANDLE_FLAG_INHERIT //
        );
    }
    if (!spawned) {
        fip_print(                                                          //
            0, FIP_WARN, "Failed to spawn slave %s: %d", id, GetLastError() //
        );
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_read);
        CloseHandle(stderr_write);
        return false;
    }

    // Close child's ends of pipes
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    CloseHandle(pi.hThread);

    // Store process info
    win_processes[modules->active_count].process = pi.hProcess;
    win_processes[modules->active_count].stdin_write = stdin_write;
    win_processes[modules->active_count].stdout_read = stdout_read;
    win_processes[modules->active_count].stderr_read = stderr_read;
    win_processes[modules->active_count].pid = pi.dwProcessId;

    // Store PID in the modules array for compatibility
    modules->pids[modules->active_count] = pi.dwProcessId;

    // Convert to FILE streams
    int stdin_fd =
        _open_osfhandle((intptr_t)stdin_write, _O_WRONLY | _O_BINARY);
    int stdout_fd =
        _open_osfhandle((intptr_t)stdout_read, _O_RDONLY | _O_BINARY);
    int stderr_fd =
        _open_osfhandle((intptr_t)stderr_read, _O_RDONLY | _O_BINARY);

    master_state.slave_stdin[modules->active_count] = _fdopen(stdin_fd, "wb");
    master_state.slave_stdout[modules->active_count] = _fdopen(stdout_fd, "rb");
    master_state.slave_stderr[modules->active_count] = _fdopen(stderr_fd, "rb");

    if (!master_state.slave_stdin[modules->active_count] ||
        !master_state.slave_stdout[modules->active_count]) {
        fip_print(0, FIP_ERROR, "Failed to create FILE streams for slave %s",
            id);
        return false;
    }

    modules->active_count++;
    return true;
}

void fip_terminate_all_slaves(fip_interop_modules_t *modules) {
    for (uint8_t i = 0; i < modules->active_count; i++) {
        if (win_processes[i].process) {
            TerminateProcess(win_processes[i].process, 1);
            CloseHandle(win_processes[i].process);
            win_processes[i].process = NULL;
        }
    }
    modules->active_count = 0;
    master_state.slave_count = 0;
    memset(&master_state.responses, 0, sizeof(fip_msg_t) + FIP_MAX_SLAVES);
    master_state.response_count = 0;
    memset(&master_state.slave_stdin, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
    memset(&master_state.slave_stdout, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
    memset(&master_state.slave_stderr, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
}

bool fip_master_init(fip_interop_modules_t *modules) {
    master_state.slave_count = modules->active_count;
    master_state.response_count = 0;
    fip_print(0, FIP_INFO,
        "Master initialized for stdio communication with %d slaves",
        master_state.slave_count);
    return true;
}

uint8_t fip_master_await_responses(        //
    fip_msg_t responses[FIP_MAX_SLAVES],   //
    uint32_t *response_count,              //
    const fip_msg_type_e expected_msg_type //
) {
    fip_print(0, FIP_INFO, "Awaiting Responses");

    for (uint8_t i = 0; i < *response_count; i++) {
        fip_free_msg(&responses[i]);
    }
    *response_count = 0;
    uint8_t wrong_count = 0;

    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (!master_state.slave_stdout[i]) {
            fip_print(0, FIP_WARN, "No output stream for slave %d", i + 1);
            wrong_count++;
            continue;
        }

        // Simple timeout using non-blocking read
        uint32_t msg_len;
        if (fread(&msg_len, 1, 4, master_state.slave_stdout[i]) != 4) {
            fip_print(0, FIP_WARN,
                "Failed to read message length from slave %d", i + 1);
            wrong_count++;
            continue;
        }

        if (msg_len == 0 || msg_len > FIP_MSG_SIZE - 4) {
            fip_print(0, FIP_WARN, "Invalid message length from slave %d: %u",
                i + 1, msg_len);
            wrong_count++;
            continue;
        }

        memset(buffer, 0, FIP_MSG_SIZE);
        if (fread(buffer, 1, msg_len, master_state.slave_stdout[i]) !=
            msg_len) {
            fip_print(0, FIP_WARN,
                "Failed to read complete message from slave %d", i + 1);
            wrong_count++;
            continue;
        }

        fip_decode_msg(buffer, &responses[*response_count]);
        fip_print(0, FIP_INFO, "Received message from slave %d: %s", i + 1,
            fip_msg_type_str[responses[*response_count].type]);

        if (responses[*response_count].type != expected_msg_type) {
            wrong_count++;
        }

        (*response_count)++;
        if (*response_count >= FIP_MAX_SLAVES)
            break;
    }

    // Print all the debug output of all the slaves
    fip_print_slave_streams();
    return wrong_count;
}

fip_master_config_t fip_master_load_config(const char *config_path) {
    FILE *fp = fopen(config_path, "r");
    fip_master_config_t config = {0};
    config.ok = false;
    if (!fp) {
        fip_print(0, FIP_WARN, "Config file not found: %s", config_path);
        return config;
    }
    toml_result_t toml = toml_parse_file(fp);
    fclose(fp);
    if (!toml.ok) {
        fip_print(0, FIP_ERROR, "Failed to parse fip.toml file: %s",
            toml.errmsg);
        return config;
    }

    if (toml.toptab.type != TOML_TABLE) {
        assert(false);
    }
    for (int32_t i = 0; i < toml.toptab.u.tab.size; i++) {
        const char *section_name = toml.toptab.u.tab.key[i];
        if (strncmp(section_name, "fip-", 4) != 0) {
            continue;
        }
        toml_datum_t section = toml.toptab.u.tab.value[i];
        if (section.type != TOML_TABLE) {
            continue;
        }
        toml_datum_t enabled = toml_get(section, "enable");
        if (enabled.type != TOML_BOOLEAN || !enabled.u.boolean) {
            fip_print(0, FIP_WARN,
                "Module %s is disabled or missing enable field", section_name);
            continue;
        }
        if (config.enabled_count >= FIP_MAX_ENABLED_MODULES) {
            fip_print(0, FIP_ERROR, "There are too many active modules (%d)!",
                config.enabled_count);
            continue;
        }
        strncpy(config.enabled_modules[config.enabled_count], section_name,
            FIP_MAX_MODULE_NAME_LEN - 1);
        config.enabled_modules[config.enabled_count]
                              [FIP_MAX_MODULE_NAME_LEN - 1] = '\0';
        config.enabled_count++;
        fip_print(0, FIP_INFO, "Enabled module: %s", section_name);
    }

    toml_free(toml);
    fip_print(0, FIP_INFO, "Found %d enabled modules", config.enabled_count);
    config.ok = true;
    return config;
}

#endif // End of Windows MASTER Implementation

/*
 * ============================
 * WINDOWS SLAVE IMPLEMENTATION
 * ============================
 */

#ifdef FIP_SLAVE

bool fip_slave_init(uint32_t slave_id) {
    // Set stdin/stdout to binary mode on Windows
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    fip_slave_open_log(slave_id);
    fip_print(slave_id, FIP_INFO, "Slave initialized for stdio communication");
    return true;
}

toml_result_t fip_slave_load_config( //
    const uint32_t id,               //
    const char *module_name          //
) {
    char file_path[64] = {0};
    snprintf(file_path, sizeof(file_path), ".fip/config/%s.toml", module_name);
    toml_result_t toml = {0};
    toml.ok = false;
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        fip_print(id, FIP_ERROR, "Config file not found: %s", file_path);
        return toml;
    }
    toml = toml_parse_file(fp);
    fclose(fp);
    if (!toml.ok) {
        fip_print(id, FIP_ERROR, "Failed to parse %s.toml file: %s",
            module_name, toml.errmsg);
        return toml;
    }
    return toml;
}

#endif // End of Windows SLAVE Implementation

#else // End of Windows Implementation

/*
 * ====================
 * LINUX IMPLEMENTATION
 * ====================
 */

#ifdef FIP_MASTER

/*
 * ===========================
 * LINUX MASTER IMPLEMENTATION
 * ===========================
 */

bool fip_spawn_interop_module(      //
    fip_interop_modules_t *modules, //
    const char *root_path,          //
    const char *module              //
) {
    char _module[256] = {0};
    char id[8] = {0};
    char ll[8] = {0};
    memcpy(_module, module, strlen(module));
    snprintf(id, 8, "%d", modules->active_count + 1);
    snprintf(ll, 8, "%d", LOG_LEVEL);
    char *argv[] = {_module, id, ll, NULL};

    fip_print(0, FIP_INFO, "Spawning slave %s...", id);

    // Create pipes for communication with slave
    int stdin_pipe[2];  // master writes to stdin_pipe[1], slave reads from
                        // stdin_pipe[0]
    int stdout_pipe[2]; // slave writes to stdout_pipe[1], master reads from
                        // stdout_pipe[0]
    int stderr_pipe[2]; // slave writes to stderr_pipe[1], master reads from
                        // stderr_pipe[0]

    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1 ||
        pipe(stderr_pipe) == -1) {
        fip_print(0, FIP_ERROR, "Failed to create pipes for slave %s", id);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        // fork failed
        fip_print(0, FIP_ERROR, "Failed to fork to spawn slave %s", id);
        // cleanup
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child: set up child's end of pipes, change cwd, exec

        // Redirect child's stdin/stdout/stderr to pipes
        // Child should read from stdin_pipe[0], write to stdout_pipe[1],
        // stderr_pipe[1]
        if (dup2(stdin_pipe[0], STDIN_FILENO) == -1)
            _exit(127);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1)
            _exit(127);
        if (dup2(stderr_pipe[1], STDERR_FILENO) == -1)
            _exit(127);

        // Close unused descriptors in child
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        // Change working directory to root directoy
        if (chdir(root_path) != 0) {
            // cannot change dir => fatal for child
            // You can log to stderr but prefer to exit
            perror("chdir");
            _exit(127);
        }

        // Execute the module. Use execvp so PATH lookup behaves like
        // posix_spawn did.
        execvp(_module, argv);

        // If execvp returns, it failed
        perror("execvp");
        _exit(127);
    }

    // Parent: close child's ends of the pipes
    close(stdin_pipe[0]);  // parent's write end is stdin_pipe[1]
    close(stdout_pipe[1]); // parent's read end is stdout_pipe[0]
    close(stderr_pipe[1]); // parent's read end is stderr_pipe[0]

    // Store PID
    modules->pids[modules->active_count] = pid;

    // Store master's ends of the pipes
    master_state.slave_stdin[modules->active_count] =
        fdopen(stdin_pipe[1], "w");
    master_state.slave_stdout[modules->active_count] =
        fdopen(stdout_pipe[0], "r");
    master_state.slave_stderr[modules->active_count] =
        fdopen(stderr_pipe[0], "r");

    if (!master_state.slave_stdin[modules->active_count] ||
        !master_state.slave_stdout[modules->active_count] ||
        !master_state.slave_stderr[modules->active_count]) {
        fip_print(0, FIP_ERROR, "Failed to create FILE streams for slave %s",
            id);
        // Cleanup: if needed, kill child? up to your policy. We'll close fds.
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        return false;
    }

    modules->active_count++;
    return true;
}

void fip_terminate_all_slaves(fip_interop_modules_t *modules) {
    // We terminate all slaves as their workloads must have been finished by
    // now (the master has collected the results in the form of the .o
    // files)
    for (uint8_t i = 0; i < modules->active_count; i++) {
        if (kill(modules->pids[i], 0)) {
            // Is still running and we are allowed to kill it
            kill(modules->pids[i], SIGTERM);
        }
    }
    modules->active_count = 0;
    master_state.slave_count = 0;
    memset(&master_state.responses, 0, sizeof(fip_msg_t) + FIP_MAX_SLAVES);
    master_state.response_count = 0;
    memset(&master_state.slave_stdin, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
    memset(&master_state.slave_stdout, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
    memset(&master_state.slave_stderr, 0, sizeof(FILE *) * FIP_MAX_SLAVES);
}

bool fip_master_init(fip_interop_modules_t *modules) {
    // Initialize master state
    master_state.slave_count = modules->active_count;
    master_state.response_count = 0;

    fip_print(0, FIP_INFO,
        "Master initialized for stdio communication with %d slaves",
        master_state.slave_count);
    return true;
}

uint8_t fip_master_await_responses(      //
    fip_msg_t responses[FIP_MAX_SLAVES], //
    uint32_t *response_count,            //
    const fip_msg_tag_e expected_msg_tag //
) {
#define FIP_TIMEOUT 1.0
    fip_print(0, FIP_INFO, "Awaiting Responses");

    // First we need to clear all old message responses
    for (uint8_t i = 0; i < *response_count; i++) {
        fip_free_msg(&responses[i]);
    }
    *response_count = 0;
    uint8_t wrong_count = 0;

    // Set all stderr file descriptors to non-blocking mode
    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (master_state.slave_stderr[i]) {
            int stderr_fd = fileno(master_state.slave_stderr[i]);
            int flags = fcntl(stderr_fd, F_GETFL, 0);
            fcntl(stderr_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    // Read responses from each slave with timeout
    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (!master_state.slave_stdout[i]) {
            fip_print(0, FIP_WARN, "No output stream for slave %d", i + 1);
            wrong_count++;
            continue;
        }

        int stdout_fd = fileno(master_state.slave_stdout[i]);
        int stderr_fd = master_state.slave_stderr[i]
            ? fileno(master_state.slave_stderr[i])
            : -1;

        fd_set read_fds;
        struct timeval timeout;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);

        bool message_received = false;

        // Keep trying until we get a message or timeout (1 second)
        while (!message_received) {
            // Check if we've exceeded timeout
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = ((double)(now.tv_sec - start.tv_sec)) +
                ((double)(now.tv_nsec - start.tv_nsec) / 1000000000.0);

            if (elapsed > FIP_TIMEOUT) {
                fip_print(0, FIP_WARN,
                    "Timeout waiting for slave %d response for %f seconds",
                    i + 1, elapsed);
                wrong_count++;
                break;
            }

            // Set up select with remaining time
            FD_ZERO(&read_fds);
            FD_SET(stdout_fd, &read_fds);
            int max_fd = stdout_fd;
            if (stderr_fd >= 0) {
                FD_SET(stderr_fd, &read_fds);
                if (stderr_fd > max_fd)
                    max_fd = stderr_fd;
            }

            double remaining = FIP_TIMEOUT - elapsed;
            timeout.tv_sec = (long)remaining;
            timeout.tv_usec = (long)((remaining - timeout.tv_sec) * 1000000);

            int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

            if (activity < 0) {
                fip_print(0, FIP_WARN, "Select error for slave %d", i + 1);
                wrong_count++;
                break;
            }

            // Always drain stderr if available (non-blocking)
            if (stderr_fd >= 0) {
                char stderr_buf[FIP_LINE_BUF_SIZE];
                ssize_t n;
                while ((n = read(stderr_fd, stderr_buf,
                            sizeof(stderr_buf) - 1)) > 0) {
                    stderr_buf[n] = '\0';
                    fprintf(stderr, "%s", stderr_buf);
                    fflush(stderr);
                }
            }

            // Check if stdout has data
            if (FD_ISSET(stdout_fd, &read_fds)) {
                // Read message length (4 bytes)
                uint32_t msg_len;
                if (fread(&msg_len, 1, 4, master_state.slave_stdout[i]) != 4) {
                    fip_print(0, FIP_WARN,
                        "Failed to read message length from slave %d", i + 1);
                    wrong_count++;
                    break;
                }

                // Validate message length
                if (msg_len == 0 || msg_len > FIP_MSG_SIZE - 4) {
                    fip_print(0, FIP_WARN,
                        "Invalid message length from slave %d: %u", i + 1,
                        msg_len);
                    wrong_count++;
                    break;
                }
                fip_print(0, FIP_DEBUG, "Received message length: %u", msg_len);

                // Read message data
                memset(buffer, 0, FIP_MSG_SIZE);
                if (fread(buffer, 1, msg_len, master_state.slave_stdout[i]) !=
                    msg_len) {
                    fip_print(0, FIP_WARN,
                        "Failed to read complete message from slave %d", i + 1);
                    wrong_count++;
                    break;
                }

                // Decode message
                fip_decode_msg(&responses[*response_count]);
                fip_print(0, FIP_INFO, "Received message from slave %d: %s",
                    i + 1, fip_msg_type_str[responses[*response_count].tag]);

                if (responses[*response_count].tag != expected_msg_tag) {
                    wrong_count++;
                }

                (*response_count)++;
                message_received = true;

                // Prevent buffer overflow
                if (*response_count >= FIP_MAX_SLAVES) {
                    break;
                }
            }

            // Small sleep to avoid busy-waiting
            if (!message_received) {
                struct timespec sleep_time = {0, 1000000}; // 1ms
                nanosleep(&sleep_time, NULL);
            }
        }
    }

    // Final drain of all stderr streams
    for (uint32_t i = 0; i < master_state.slave_count; i++) {
        if (master_state.slave_stderr[i]) {
            int stderr_fd = fileno(master_state.slave_stderr[i]);
            char stderr_buf[FIP_LINE_BUF_SIZE];
            ssize_t n;
            while (
                (n = read(stderr_fd, stderr_buf, sizeof(stderr_buf) - 1)) > 0) {
                stderr_buf[n] = '\0';
                fprintf(stderr, "%s", stderr_buf);
                fflush(stderr);
            }
        }
    }

    return wrong_count;
}

fip_master_config_t fip_master_load_config(const char *config_path) {
    FILE *fp = fopen(config_path, "r");
    fip_master_config_t config = {0};
    config.ok = false;
    if (!fp) {
        fip_print(0, FIP_WARN, "Config file not found: %s", config_path);
        return config;
    }
    toml_result_t toml = toml_parse_file(fp);
    fclose(fp);
    // Check for parse error
    if (!toml.ok) {
        fip_print(0, FIP_ERROR, "Failed to parse fip.toml file: %s",
            toml.errmsg);
        return config;
    }

    // The toptab is a table, so it not being a table should have been a parse
    // error
    if (toml.toptab.type != TOML_TABLE) {
        assert(false);
    }
    // Iterate through its keys
    for (int32_t i = 0; i < toml.toptab.u.tab.size; i++) {
        const char *section_name = toml.toptab.u.tab.key[i];

        // Check if section starts with "fip-"
        if (strncmp(section_name, "fip-", 4) != 0) {
            continue;
        }
        // Get the section (it's a table)
        toml_datum_t section = toml.toptab.u.tab.value[i];
        if (section.type != TOML_TABLE) {
            continue;
        }
        // Look for "enable" key in this section
        toml_datum_t enabled = toml_get(section, "enable");
        if (enabled.type != TOML_BOOLEAN || !enabled.u.boolean) {
            fip_print(0, FIP_WARN,
                "Module %s is disabled or missing enable field", section_name);
            continue;
        }

        // Check whether we have reached the maximum module count
        if (config.enabled_count >= FIP_MAX_ENABLED_MODULES) {
            fip_print(0, FIP_ERROR, "There are too many active modules (%d)!",
                config.enabled_count);
            continue;
        }

        // Add to enabled modules list
        strncpy(config.enabled_modules[config.enabled_count], section_name,
            FIP_MAX_MODULE_NAME_LEN - 1);
        config.enabled_modules[config.enabled_count]
                              [FIP_MAX_MODULE_NAME_LEN - 1] = '\0';
        config.enabled_count++;
        fip_print(0, FIP_INFO, "Enabled module: %s", section_name);
    }

    toml_free(toml);
    fip_print(0, FIP_INFO, "Found %d enabled modules", config.enabled_count);
    config.ok = true;
    return config;
}

#endif // End of #ifdef FIP_MASTER

/*
 * ==========================
 * LINUX SLAVE IMPLEMENTATION
 * ==========================
 */

#ifdef FIP_SLAVE

bool fip_slave_init(uint32_t slave_id) {
    fip_slave_open_log(slave_id);
    fip_print(slave_id, FIP_INFO, "Slave initialized for stdio communication");
    return true;
}

toml_result_t fip_slave_load_config( //
    const uint32_t id,               //
    const char *module_name          //
) {
    char file_path[64] = {0};
    snprintf(file_path, sizeof(file_path), ".fip/config/%s.toml", module_name);
    toml_result_t toml = {0};
    toml.ok = false;
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        fip_print(id, FIP_ERROR, "Config file not found: %s", file_path);
        return toml;
    }
    toml = toml_parse_file(fp);
    fclose(fp);
    if (!toml.ok) {
        fip_print(id, FIP_ERROR, "Failed to parse %s.toml file: %s",
            module_name, toml.errmsg);
        return toml;
    }

    return toml;
}

#endif // End of #ifdef FIP_SLAVE

#endif // End of #else (Linux Implementation)

#endif // End of #ifdef FIP_IMPLEMENTATION
