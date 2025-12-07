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

#include "toml/tomlc17.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __WIN32__
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

// #define _GNU_SOURCE
// #define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

// POSIX constants
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#endif

#define FIP_MAX_SLAVES 64
#define FIP_MSG_SIZE 1024
#define FIP_SLAVE_DELAY_MS 10

#ifdef __WIN32__
#include <windows.h>
[[maybe_unused]]
static void msleep(unsigned int ms) {
    Sleep(ms);
}
#else
[[maybe_unused]]
static void msleep(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* nanosleep may be interrupted; ignore remaining time for simplicity */
    nanosleep(&ts, NULL);
}
#endif

// The version of the FIP
#define FIP_MAJOR 0
#define FIP_MINOR 2
#define FIP_PATCH 3

#define FIP_TYPE_COUNT 12
#define FIP_MAX_MODULE_NAME_LEN 16

/// @typedef `fip_type_prim_enum_t`
/// @brief Enum of all possible primitive types supported by FIP
typedef enum : uint8_t {
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
} fip_type_prim_enum_t;

/// @typedef `fip_msg_type_t`
/// @bfief Enum of all possible messages the FIP can handle
typedef enum : uint8_t {
    FIP_MSG_UNKNOWN = 0,     // Unknown message
    FIP_MSG_CONNECT_REQUEST, // Slave trying to connect to master
    FIP_MSG_SYMBOL_REQUEST,  // Master requesting symbol resolution
    FIP_MSG_SYMBOL_RESPONSE, // Slave response of FN_REQ
    FIP_MSG_COMPILE_REQUEST, // Master requesting all slaves to compile
    FIP_MSG_OBJECT_RESPONSE, // Slave responding compilation with .o file
    FIP_MSG_KILL,            // Kill command comes last
} fip_msg_type_t;

/// @typedef `fip_msg_symbol_type_t`
/// @brief Enum of all possible symbol types
typedef enum : uint8_t {
    FIP_SYM_UNKNOWN = 0,
    FIP_SYM_FUNCTION,
    FIP_SYM_DATA,
} fip_msg_symbol_type_t;

/// @typedef `fip_log_level_t`
/// @breif Enum of all possible log levels of FIP
typedef enum : uint8_t {
    FIP_NONE = 0,
    FIP_ERROR,
    FIP_WARN,
    FIP_INFO,
    FIP_DEBUG,
    FIP_TRACE,
} fip_log_level_t;

extern fip_log_level_t LOG_LEVEL;

/*
 * ===============
 * ARRAYS AND MAPS
 * ===============
 */

/// @var `fip_msg_type_str`
/// @brief A simple array containing all the string names of the possible
/// message types
extern const char *fip_msg_type_str[];

/*
 * ===============
 * TYPE STRUCTURES
 * ===============
 */

/// @brief Forward-Declaration of the `fip_type_t` struct type
struct fip_type_t;

/// @typedef `fip_type_ptr_t`
/// @brief The struct representing a pointer type
typedef struct {
    struct fip_type_t *base_type;
} fip_type_ptr_t;

/// @typedef `fip_type_struct_t`
/// @brief The struct representing a struct type
typedef struct {
    uint8_t field_count;
    struct fip_type_t *fields;
} fip_type_struct_t;

/// @typedef `fip_type_enum_t`
/// @brief The enum containing all possible FIP types there are
typedef enum {
    FIP_TYPE_PRIMITIVE,
    FIP_TYPE_PTR,
    FIP_TYPE_STRUCT,
} fip_type_enum_t;

/// @typedef `fip_type_t`
/// @brief The struct representing a type in FIP
typedef struct fip_type_t {
    fip_type_enum_t type;
    bool is_mutable;
    union {
        fip_type_prim_enum_t prim;
        fip_type_ptr_t ptr;
        fip_type_struct_t struct_t;
    } u;
} fip_type_t;

/*
 * =================
 * SYMBOL STRUCTURES
 * =================
 */

/// @typedef `fip_fn_sig_t`
/// @brief Struct representing the signature of a FIP-defined function
typedef struct {
    char name[128];
    uint8_t args_len;
    fip_type_t *args;
    uint8_t rets_len;
    fip_type_t *rets;
} fip_sig_fn_t;

/*
 * ==================
 * MESSAGE STRUCTURES
 * ==================
 */

/// @typedef `fip_msg_connect_request_t`
/// @breif Struct representing all information from a connection request
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
    fip_msg_symbol_type_t type;
    union {
        fip_sig_fn_t fn;
    } sig;
} fip_msg_symbol_request_t;

/// @typedef `fip_msg_symbol_response_t`
/// @brief Struct representing the symbol response message
typedef struct {
    bool found;
    char module_name[FIP_MAX_MODULE_NAME_LEN];
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

#define FIP_PATH_SIZE 8
#define FIP_PATHS_SIZE FIP_MSG_SIZE - 32

/// @typedef `fip_msg_object_response_t`
/// @brief Struct representing the object response message
typedef struct {
    bool has_obj;
    bool compilation_failed;
    char module_name[FIP_MAX_MODULE_NAME_LEN];
    uint8_t path_count;
    char paths[FIP_PATHS_SIZE];
} fip_msg_object_response_t;

/// @typedef `fip_msg_kill_reason_t`
/// @brief The reason enum for the kill command
typedef enum : uint8_t {
    FIP_KILL_FINISH = 0,
    FIP_KILL_VERSION_MISMATCH,
} fip_msg_kill_reason_t;

/// @typedef `fip_msg_kill_t`
/// @brief Struct representing the kill message
typedef struct {
    fip_msg_kill_reason_t reason;
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
/// @param `log_level` The log level of the current message to print
/// @param `format` The format string of the printed message
/// @param `...` The variadic values to put into the formatted output
void fip_print(                      //
    const uint32_t id,               //
    const fip_log_level_t log_level, //
    const char *format,              //
    ...                              //
);

/// @function `fip_print_msg`
/// @brief Prints the given message from the given module ID
///
/// @param `id` The id of the process to print the message from
/// @param `message` The message to print
void fip_print_msg(uint32_t id, const fip_msg_t *message);

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

/// @function `fip_free_type`
/// @brief Frees the given type
///
/// @param `type` The type to free
void fip_free_type(fip_type_t *type);

/// @function `fip_free_msg`
/// @brief Frees a given message
///
/// @param `message` The message to free
void fip_free_msg(fip_msg_t *message);

/// @function `fip_create_hash`
/// @brief Creates a 8 Byte character hash from the given file path to make
/// differentiating between different files predictable in size. Each character
/// in the 8 character hash is one of 63 possible characters (A-Z, a-z, 0-9, _)
/// which means that the 8 Byte hash can have roughly as many unique hashes as a
/// equivalent 48 bit number.
///
/// @param `hash` The buffer in which to write the hash
/// @param `file_path` The file path to turn into a 8 Byte hash
void fip_create_hash(char hash[8], const char *file_path);

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
    const uint32_t id,            //
    const char *type_str,         //
    const char *type_str_table[], //
    size_t start_idx,             //
    size_t end_idx,               //
    fip_type_t **sig,             //
    uint8_t *sig_len              //
);

/// @function `fip_parse_fn_signature`
/// @brief Parses a given function signature and returns the parsed signature in
/// a struct
///
/// @param `id` The id of the process in which the signature is parsed
/// @param `signature` The function signature to parse
/// @return `fip_sig_fn_t` The parsed function signature
fip_sig_fn_t fip_parse_fn_signature(uint32_t id, const char *signature);

/// @function `fip_print_type`
/// @brief "Prints" a given type into the buffer which then can be used to print
/// the whole type in one fip_print call
///
/// @param `buffer` The buffer in which the type string will be stored in
/// @param `idx` The current index in the buffer, the "write pointer"
/// @param `type` The type to print into the buffer
void fip_print_type(       //
    char buffer[1024],     //
    int *idx,              //
    const fip_type_t *type //
);

/// @function `fip_print_sig_fn`
/// @brief Prints a parsed function signature to the console
///
/// @param `id` The id of the process in which the signature is printed
/// @param `sig` The function signature to print
void fip_print_sig_fn(uint32_t id, const fip_sig_fn_t *sig);

/// @function `fip_clone_sig_fn`
/// @brief Clones a given function signature from the source to the destination
///
/// @brief `dest` The signature to fill
/// @brief `src` The source to clone
void fip_clone_sig_fn(fip_sig_fn_t *dest, const fip_sig_fn_t *src);

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

typedef struct {
    uint8_t active_count;
    pid_t pids[FIP_MAX_SLAVES];
} fip_interop_modules_t;

typedef struct {
    FILE *slave_stdin[FIP_MAX_SLAVES];
    FILE *slave_stdout[FIP_MAX_SLAVES];
    FILE *slave_stderr[FIP_MAX_SLAVES];
    uint32_t slave_count;
    fip_msg_t responses[FIP_MAX_SLAVES];
    uint32_t response_count;
} fip_master_state_t;

typedef struct {
    bool ok;
    char enabled_modules[FIP_MAX_ENABLED_MODULES][FIP_MAX_MODULE_NAME_LEN];
    uint8_t enabled_count;
} fip_master_config_t;

#ifndef __WIN32__
extern char **environ;
#endif
extern fip_master_state_t master_state;

/// @function `fip_copy_stream_lines`
/// @brief Copies all lines from the `src` stream into the `dest` stream. Only
/// copies full lines, and copies the lines as one unit. So, all leftover
/// characters are, well, left, in the `src` and only full lines are written to
/// `dest`
///
/// @param `src` The source stream from which lines are read
/// @param `dest` The destination stream to which lines are copied to
void fip_copy_stream_lines(FILE *src, FILE *dest);

/// @function `fip_print_slave_streams`
/// @brief Prints all the `stderr` streams from all slaves into the `stderr`
/// stream of the master, to gather all the debug output from all slaves
void fip_print_slave_streams();

/// @function `fip_spawn_interop_module`
/// @brief Creates a new interop module and adds it's process ID to the list of
/// modules in the interop modules parameter
///
/// @param `modules` A pointer to the structure containing all the module PIDs
/// @param `binary` The interop module to start
/// @return `bool` Whether the interop module process creation was successful
bool fip_spawn_interop_module(      //
    fip_interop_modules_t *modules, //
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
/// @param `buffer` The buffer in which the message will be encoded before
/// sending it
/// @param `message` The message to send
void fip_master_broadcast_message( //
    char buffer[FIP_MSG_SIZE],     //
    const fip_msg_t *message       //
);

/// @function `fip_master_await_responses`
/// @brief Waits for all slaves to respond with a message from stdin
///
/// @param `buffer` The buffer in which the recieved messages will be stored
/// temporarily
/// @param `responses` The responses of all slaves where the ID of the response
/// in the array corresponds to the ID of the slave itself
/// @param `response_count` How many responses we got
/// @param `expected_msg_type` The type of the expected message
/// @return `uint8_t` How many responses were faulty (unable to be read) or had
/// the wrong type
uint8_t fip_master_await_responses(        //
    char buffer[FIP_MSG_SIZE],             //
    fip_msg_t responses[FIP_MAX_SLAVES],   //
    uint32_t *response_count,              //
    const fip_msg_type_t expected_msg_type //
);

/// @function `fip_master_symbol_request`
/// @brief Broadcasts a symbol request message and then awaits all
/// symbol response messages and returns whether the requested
/// symbol was found
///
/// @param `buffer` The buffer in which the to-be-sent message and the recieved
/// messages will be stored in
/// @param `message` The symbol request message to send
/// @return `bool` Whether the requested symbol was found
///
/// @note This function asserts the message type to be FIP_MSG_SYMBOL_REQUEST
bool fip_master_symbol_request( //
    char buffer[FIP_MSG_SIZE],  //
    const fip_msg_t *message    //
);

/// @function `fip_master_compile_request`
/// @brief Broadcasts a compile request message and then awaits
/// all object response messages and returns whether all modules
/// were able to compile their sources
///
/// @param `buffer` The buffer in which the to-be-sent message and the recieved
/// messages will be stored in
/// @param `message` The compile request message to send
/// @return `bool` Whether all interop modules were able to compile their
/// source files
///
/// @note This function asserts the message type to be FIP_MSG_COMPILE_REQUEST
bool fip_master_compile_request( //
    char buffer[FIP_MSG_SIZE],   //
    const fip_msg_t *message     //
);

/// @function `fip_master_cleanup`
/// @brief Cleans up the master
void fip_master_cleanup();

/// @function `fip_master_load_config`
/// @brief Loads the master config from the `.fip/config/fip.toml` file
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

/// @function `fip_slave_init`
/// @brief Initializes the slave for stdio-based communication with named pipes
///
/// @param `slave_id` The ID of this slave process
/// @return `bool` Whether initialization was successful
bool fip_slave_init(uint32_t slave_id);

/// @function `fip_slave_recieve_message`
/// @brief Reads a message from stdin and stores it in the buffer
///
/// @param `buffer` The buffer where to store the recieved message at
/// @return `bool` Whether a message was recieved
bool fip_slave_receive_message(char buffer[FIP_MSG_SIZE]);

/// @function `fip_slave_send_message`
/// @brief Sends a message to stdout
///
/// @param `id` The id of the slave who tries to send the message
/// @param `buffer` The buffer in which the message to send will be stored
/// @param `message` The message which will be sent
void fip_slave_send_message(   //
    uint32_t id,               //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
);

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

const char *fip_msg_type_str[] = {
    "FIP_MSG_UNKNOWN",
    "FIP_MSG_CONNECT_REQUEST",
    "FIP_MSG_SYMBOL_REQUEST",
    "FIP_MSG_SYMBOL_RESPONSE",
    "FIP_MSG_COMPILE_REQUEST",
    "FIP_MSG_OBJECT_RESPONSE",
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
    const fip_log_level_t log_level, //
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
    GetSystemTimePreciseAsFileTime(&ft); // Windows 8+ for high precision
    // For older Windows, use: GetSystemTimeAsFileTime(&ft);

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
    microseconds =
        (uli.QuadPart / 10) % 1000; // Convert to microseconds, get remainder
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
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Print the prefix and the message to stderr
    fprintf(stderr, "%s%s\n", prefix, message);
    fflush(stderr);
}

void fip_print_msg(uint32_t id, const fip_msg_t *message) {
    switch (message->type) {
        case FIP_MSG_UNKNOWN:
            fip_print(id, FIP_DEBUG, "FIP_MSG_UNKNOWN: {}");
            break;
        case FIP_MSG_CONNECT_REQUEST:
            fip_print(id, FIP_DEBUG, "FIP_MSG_CONNECT_REQUEST: {");
            fip_print(id, FIP_DEBUG, "  .setup_ok: ", //
                message->u.con_req.setup_ok           //
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
            switch (message->u.sym_req.type) {
                case FIP_SYM_UNKNOWN:
                    fip_print(id, FIP_DEBUG, "  .type: UNKNOWN");
                    break;
                case FIP_SYM_FUNCTION:
                    fip_print(id, FIP_DEBUG, "  .type: FUNCTION");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_fn(id, &message->u.sym_req.sig.fn);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SYM_DATA:
                    fip_print(id, FIP_DEBUG, "  .type: DATA");
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
            switch (message->u.sym_res.type) {
                case FIP_SYM_UNKNOWN:
                    fip_print(id, FIP_DEBUG, "  .type: UNKNOWN");
                    break;
                case FIP_SYM_FUNCTION:
                    fip_print(id, FIP_DEBUG, "  .type: FUNCTION");
                    fip_print(id, FIP_DEBUG, "  .signature: {");
                    fip_print_sig_fn(id, &message->u.sym_req.sig.fn);
                    fip_print(id, FIP_DEBUG, "  }");
                    break;
                case FIP_SYM_DATA:
                    fip_print(id, FIP_DEBUG, "  .type: DATA");
                    fip_print(id, FIP_DEBUG, "  .signature: TODO");
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

void fip_encode_type(          //
    char buffer[FIP_MSG_SIZE], //
    uint32_t *idx,             //
    const fip_type_t *type     //
) {
    buffer[(*idx)++] = (char)type->type;
    buffer[(*idx)++] = (char)type->is_mutable;
    switch (type->type) {
        case FIP_TYPE_PRIMITIVE:
            buffer[(*idx)++] = (char)type->u.prim;
            break;
        case FIP_TYPE_PTR:
            fip_encode_type(buffer, idx, type->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT:
            buffer[(*idx)++] = (char)type->u.struct_t.field_count;
            for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                fip_encode_type(buffer, idx, &type->u.struct_t.fields[i]);
            }
            break;
    }
}

void fip_encode_sig_fn(        //
    char buffer[FIP_MSG_SIZE], //
    uint32_t *idx,             //
    const fip_sig_fn_t *sig    //
) {
    // Because the name is a known size of 128 bytes we can store it directly in
    // the buffer
    memcpy(buffer + *idx, sig->name, 128);
    *idx += 128;
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are uint8_t's annyway
    // because which function has more than 256 parameters or return types?
    buffer[(*idx)++] = sig->args_len;
    for (uint8_t i = 0; i < sig->args_len; i++) {
        buffer[(*idx)++] = sig->args[i].is_mutable;
        fip_encode_type(buffer, idx, &sig->args[i]);
    }
    buffer[(*idx)++] = sig->rets_len;
    for (uint8_t i = 0; i < sig->rets_len; i++) {
        buffer[(*idx)++] = sig->rets[i].is_mutable;
        fip_encode_type(buffer, idx, &sig->rets[i]);
    }
}

void fip_encode_msg(char buffer[FIP_MSG_SIZE], const fip_msg_t *message) {
    // Clear the buffer
    memset(buffer, 0, FIP_MSG_SIZE);
    // The message always starts with the length of the message as a 4 byte
    // uint32_teger and then the actual message follows. This is why the
    // 'idx' starts at 4, since the first 4 bytes need to be filled with the
    // size of the message itself
    // The first character in the buffer is the message type
    uint32_t idx = 4;
    buffer[idx++] = message->type;
    switch (message->type) {
        case FIP_MSG_UNKNOWN:
            // Sending unknown or faulty message
            break;
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the version info followed by the
            // module name into the buffer and is done
            buffer[idx++] = (bool)message->u.con_req.setup_ok;
            buffer[idx++] = message->u.con_req.version.major;
            buffer[idx++] = message->u.con_req.version.minor;
            buffer[idx++] = message->u.con_req.version.patch;
            memcpy(buffer + idx, message->u.con_req.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            idx += FIP_MAX_MODULE_NAME_LEN;
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            buffer[idx++] = message->u.sym_req.type;
            switch (message->u.sym_req.type) {
                case FIP_SYM_UNKNOWN:
                    break;
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
            memcpy(buffer + idx, message->u.sym_res.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            idx += FIP_MAX_MODULE_NAME_LEN;
            buffer[idx++] = message->u.sym_res.type;
            switch (message->u.sym_res.type) {
                case FIP_SYM_UNKNOWN:
                    break;
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
            idx += 16;
            break;
        case FIP_MSG_OBJECT_RESPONSE: {
            // The sizes of the buffers are known so we can put them into the
            // buffer directly
            buffer[idx++] = message->u.obj_res.has_obj;
            buffer[idx++] = message->u.obj_res.compilation_failed;
            memcpy(buffer + idx, message->u.obj_res.module_name,
                FIP_MAX_MODULE_NAME_LEN);
            idx += FIP_MAX_MODULE_NAME_LEN;
            const uint8_t path_count = message->u.obj_res.path_count;
            buffer[idx++] = path_count;
            const uint32_t offset = FIP_PATH_SIZE * path_count;
            memcpy(buffer + idx, message->u.obj_res.paths, offset);
            idx += offset;
            break;
        }
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            buffer[idx++] = message->u.kill.reason;
            break;
    }
    uint32_t msg_len = idx - 4;
    memcpy(&buffer[0], &msg_len, sizeof(uint32_t));
}

void fip_decode_type(                //
    const char buffer[FIP_MSG_SIZE], //
    uint32_t *idx,                   //
    fip_type_t *type                 //
) {
    type->type = (fip_type_enum_t)buffer[(*idx)++];
    type->is_mutable = (bool)buffer[(*idx)++];
    switch (type->type) {
        case FIP_TYPE_PRIMITIVE:
            type->u.prim = (fip_type_prim_enum_t)buffer[(*idx)++];
            break;
        case FIP_TYPE_PTR:
            type->u.ptr.base_type = (fip_type_t *)malloc(sizeof(fip_type_t));
            fip_decode_type(buffer, idx, type->u.ptr.base_type);
            break;
        case FIP_TYPE_STRUCT:
            type->u.struct_t.field_count = (uint8_t)buffer[(*idx)++];
            if (type->u.struct_t.field_count > 0) {
                type->u.struct_t.fields = (fip_type_t *)malloc(       //
                    sizeof(fip_type_t) * type->u.struct_t.field_count //
                );
                for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                    fip_decode_type(buffer, idx, &type->u.struct_t.fields[i]);
                }
            }
            break;
    }
}

void fip_decode_sig_fn(              //
    const char buffer[FIP_MSG_SIZE], //
    uint32_t *idx,                   //
    fip_sig_fn_t *sig                //
) {
    // Because the name is a known size of 128 bytes we can store it directly in
    // the signature
    memcpy(sig->name, buffer + *idx, 128);
    *idx += 128;
    // Because each type is a simple char we can store them directly. But we
    // need to store first how many types there are. For that we store the
    // lengths directly in the buffer. The lengths are uint8_t's annyway
    // because which function has more than 256 parameters or return types?
    sig->args_len = buffer[(*idx)++];
    if (sig->args_len > 0) {
        sig->args = (fip_type_t *)malloc(sizeof(fip_type_t) * sig->args_len);
        for (uint8_t i = 0; i < sig->args_len; i++) {
            sig->args[i].is_mutable = buffer[(*idx)++];
            fip_decode_type(buffer, idx, &sig->args[i]);
        }
    } else {
        sig->args = NULL;
    }
    sig->rets_len = buffer[(*idx)++];
    if (sig->rets_len > 0) {
        sig->rets = (fip_type_t *)malloc(sizeof(fip_type_t) * sig->rets_len);
        for (uint8_t i = 0; i < sig->rets_len; i++) {
            sig->rets[i].is_mutable = buffer[(*idx)++];
            fip_decode_type(buffer, idx, &sig->rets[i]);
        }
    } else {
        sig->rets = NULL;
    }
}

void fip_decode_msg(const char buffer[FIP_MSG_SIZE], fip_msg_t *message) {
    // The first character in the buffer is the message type
    uint32_t idx = 0;
    message->type = (fip_msg_type_t)buffer[idx++];
    switch (message->type) {
        case FIP_MSG_UNKNOWN:
            // Recieved unknown or faulty message
            break;
        case FIP_MSG_CONNECT_REQUEST:
            // The connect request just puts the versions into the buffer one by
            // one and is done
            message->u.con_req.setup_ok = (bool)buffer[idx++];
            message->u.con_req.version.major = buffer[idx++];
            message->u.con_req.version.minor = buffer[idx++];
            message->u.con_req.version.patch = buffer[idx++];
            memcpy(message->u.con_req.module_name, buffer + idx,
                FIP_MAX_MODULE_NAME_LEN);
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            message->u.sym_req.type = (fip_msg_symbol_type_t)buffer[idx++];
            switch (message->u.sym_req.type) {
                case FIP_SYM_UNKNOWN:
                    break;
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
            message->u.sym_res.found = (bool)buffer[idx++];
            memcpy(message->u.sym_res.module_name, buffer + idx,
                FIP_MAX_MODULE_NAME_LEN);
            idx += FIP_MAX_MODULE_NAME_LEN;
            message->u.sym_res.type = (fip_msg_symbol_type_t)buffer[idx++];
            switch (message->u.sym_res.type) {
                case FIP_SYM_UNKNOWN:
                    break;
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
        case FIP_MSG_OBJECT_RESPONSE: {
            // The sizes of the buffers are known so we can read them from the
            // buffer directly
            message->u.obj_res.has_obj = (bool)buffer[idx++];
            message->u.obj_res.compilation_failed = (bool)buffer[idx++];
            memcpy(message->u.obj_res.module_name, buffer + idx,
                FIP_MAX_MODULE_NAME_LEN);
            idx += FIP_MAX_MODULE_NAME_LEN;
            const uint8_t path_count = buffer[idx++];
            message->u.obj_res.path_count = path_count;
            const uint32_t offset = FIP_PATH_SIZE * path_count;
            memcpy(message->u.obj_res.paths, buffer + idx, offset);
            break;
        }
        case FIP_MSG_KILL:
            // The kill message just adds why the kill happens
            message->u.kill.reason = (fip_msg_kill_reason_t)buffer[idx++];
            break;
    }
}

void fip_free_type(fip_type_t *type) {
    switch (type->type) {
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
    }
}

void fip_free_msg(fip_msg_t *message) {
    const fip_msg_type_t msg_type = message->type;
    message->type = FIP_MSG_UNKNOWN;
    switch (msg_type) {
        case FIP_MSG_UNKNOWN:
            return;
        case FIP_MSG_CONNECT_REQUEST:
            message->u.con_req.version.major = 0;
            message->u.con_req.version.minor = 0;
            message->u.con_req.version.patch = 0;
            break;
        case FIP_MSG_SYMBOL_REQUEST:
            switch (message->u.sym_req.type) {
                case FIP_SYM_UNKNOWN:
                    // Do nothing on already freed / unknwon symbol
                    break;
                case FIP_SYM_FUNCTION: {
                    message->u.sym_req.type = FIP_SYM_UNKNOWN;
                    memset(message->u.sym_req.sig.fn.name, 0, 128);
                    uint8_t args_len = message->u.sym_req.sig.fn.args_len;
                    if (args_len > 0) {
                        for (uint8_t i = 0; i < args_len; i++) {
                            fip_free_type(&message->u.sym_req.sig.fn.args[i]);
                        }
                        free(message->u.sym_req.sig.fn.args);
                    }
                    message->u.sym_req.sig.fn.args_len = 0;
                    uint8_t rets_len = message->u.sym_req.sig.fn.rets_len;
                    if (rets_len > 0) {
                        for (uint8_t i = 0; i < rets_len; i++) {
                            fip_free_type(&message->u.sym_req.sig.fn.rets[i]);
                        }
                        free(message->u.sym_req.sig.fn.rets);
                    }
                    message->u.sym_req.sig.fn.rets_len = 0;
                    break;
                }
                case FIP_SYM_DATA:
                    // Not implemented yet
                    assert(false);
            }
            break;
        case FIP_MSG_SYMBOL_RESPONSE:
            message->u.sym_res.found = false;
            memset(message->u.sym_res.module_name, 0, FIP_MAX_MODULE_NAME_LEN);
            switch (message->u.sym_res.type) {
                case FIP_SYM_UNKNOWN:
                    // Do nothing on already freed / unknwon symbol
                    break;
                case FIP_SYM_FUNCTION: {
                    message->u.sym_res.type = FIP_SYM_UNKNOWN;
                    memset(message->u.sym_res.sig.fn.name, 0, 128);
                    uint8_t args_len = message->u.sym_res.sig.fn.args_len;
                    if (args_len > 0) {
                        for (uint8_t i = 0; i < args_len; i++) {
                            fip_free_type(&message->u.sym_res.sig.fn.args[i]);
                        }
                        free(message->u.sym_res.sig.fn.args);
                    }
                    message->u.sym_res.sig.fn.args_len = 0;
                    uint8_t rets_len = message->u.sym_res.sig.fn.rets_len;
                    if (message->u.sym_res.sig.fn.rets_len > 0) {
                        for (uint8_t i = 0; i < rets_len; i++) {
                            fip_free_type(&message->u.sym_res.sig.fn.rets[i]);
                        }
                        free(message->u.sym_res.sig.fn.rets);
                    }
                    message->u.sym_res.sig.fn.rets_len = 0;
                    break;
                }
                case FIP_SYM_DATA:
                    // Not implemented yet
                    assert(false);
            }
            break;
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
        case FIP_MSG_KILL:
            // The enum does not need to be changed at all
            break;
    }
}

void fip_create_hash(char hash[8], const char *file_path) {
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

void fip_print_type(       //
    char buffer[1024],     //
    int *idx,              //
    const fip_type_t *type //
) {
    if (*idx == 0) {
        memset(buffer, 0, 1024);
    }
    switch (type->type) {
        case FIP_TYPE_PRIMITIVE: {
            const char *type_name = fip_type_names[type->u.prim];
            uint8_t type_len = (uint8_t)strlen(type_name);
            memcpy(buffer + *idx, type_name, type_len);
            *idx += type_len;
            break;
        }
        case FIP_TYPE_PTR:
            fip_print_type(buffer, idx, type->u.ptr.base_type);
            buffer[(*idx)++] = '*';
            break;
        case FIP_TYPE_STRUCT:
            buffer[(*idx)++] = '{';
            buffer[(*idx)++] = ' ';
            for (uint8_t i = 0; i < type->u.struct_t.field_count; i++) {
                fip_print_type(buffer, idx, &type->u.struct_t.fields[i]);
                if (i + 1 != type->u.struct_t.field_count) {
                    buffer[(*idx)++] = ',';
                }
                buffer[(*idx)++] = ' ';
            }
            buffer[(*idx)++] = '}';
            break;
    }
}

void fip_print_sig_fn(uint32_t id, const fip_sig_fn_t *sig) {
    fip_print(id, FIP_DEBUG, "  Function Signature:");
    fip_print(id, FIP_DEBUG, "    name: %s", sig->name);
    char buffer[1024] = {0};
    int idx = 0;
    for (uint32_t i = 0; i < sig->args_len; i++) {
        idx = 0;
        fip_print_type(buffer, &idx, &sig->args[i]);
        if (sig->args[i].is_mutable) {
            fip_print(id, FIP_DEBUG, "    arg[%u]: mut %s", i, buffer);
        } else {
            fip_print(id, FIP_DEBUG, "    arg[%u]: const %s", i, buffer);
        }
    }
    for (uint32_t i = 0; i < sig->rets_len; i++) {
        idx = 0;
        fip_print_type(buffer, &idx, &sig->rets[i]);
        if (sig->rets[i].is_mutable) {
            fip_print(id, FIP_DEBUG, "    ret[%u]: mut %s", i, buffer);
        } else {
            fip_print(id, FIP_DEBUG, "    ret[%u]: const %s", i, buffer);
        }
    }
}

void fip_clone_sig_fn(fip_sig_fn_t *dest, const fip_sig_fn_t *src) {
    memcpy(dest->name, src->name, 128);
    dest->args_len = src->args_len;
    if (src->args_len > 0) {
        dest->args = (fip_type_t *)malloc(sizeof(fip_type_t) * src->args_len);
        for (uint8_t i = 0; i < src->args_len; i++) {
            dest->args[i] = src->args[i];
        }
    }
    dest->rets_len = src->rets_len;
    if (src->rets_len > 0) {
        dest->rets = (fip_type_t *)malloc(sizeof(fip_type_t) * src->rets_len);
        for (uint8_t i = 0; i < src->rets_len; i++) {
            dest->rets[i] = src->rets[i];
        }
    }
}

int fip_execute_and_capture(char **output, const char *command) {
    if (!output || !command) {
        return -1;
    }

    *output = NULL;
    int exit_code = 0;

#ifdef __WIN32__
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
        char buffer[4096];
        DWORD bytes_read;
        size_t total_size = 0;
        size_t capacity = 4096;
        *output = (char *)malloc(capacity);
        (*output)[0] = '\0';

        // Read from both stdout and stderr
        HANDLE handles[2] = {stdout_read, stderr_read};
        for (int i = 0; i < 2; i++) {
            while (ReadFile(handles[i], buffer, sizeof(buffer), &bytes_read,
                       NULL) &&
                bytes_read > 0) {
                if (total_size + bytes_read >= capacity) {
                    capacity *= 2;
                    *output = (char *)realloc(*output, capacity);
                }
                memcpy(*output + total_size, buffer, bytes_read);
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
    char buffer[4096];
    size_t total_size = 0;
    size_t capacity = 4096;
    *output = (char *)malloc(capacity);
    (*output)[0] = '\0';

    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (total_size + bytes_read >= capacity) {
            capacity *= 2;
            *output = (char *)realloc(*output, capacity);
        }
        memcpy(*output + total_size, buffer, bytes_read);
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

#define FIP_LINE_BUF_SIZE 4096

void fip_copy_stream_lines(FILE *src, FILE *dest) {
    if (!src || !dest) {
        fprintf(stderr, "fip_copy_stream_lines: NULL argument\n");
        abort();
    }

    char buffer[FIP_LINE_BUF_SIZE];

#ifdef __WIN32__
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
            size_t to_read = (bytes_available < sizeof(buffer) - 1)
                ? bytes_available
                : sizeof(buffer) - 1;
            if (ReadFile(handle, buffer, to_read, &bytes_read, NULL)) {
                buffer[bytes_read] = '\0';
                fputs(buffer, dest);
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
        while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            fputs(buffer, dest);
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

void fip_master_broadcast_message( //
    char buffer[FIP_MSG_SIZE],     //
    const fip_msg_t *message       //
) {
    fip_print(0, FIP_INFO, "Broadcasting message to %d slaves",
        master_state.slave_count);
    fip_encode_msg(buffer, message);
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

bool fip_master_symbol_request( //
    char buffer[FIP_MSG_SIZE],  //
    const fip_msg_t *message    //
) {
    assert(message->type == FIP_MSG_SYMBOL_REQUEST);
    fip_master_broadcast_message(buffer, message);
    uint8_t wrong_msg_count =
        fip_master_await_responses(buffer, master_state.responses,
            &master_state.response_count, FIP_MSG_SYMBOL_RESPONSE);
    if (wrong_msg_count > 0) {
        fip_print(0, FIP_WARN, "Received %u wrong messages", wrong_msg_count);
    }

    bool symbol_found = false;
    for (uint8_t i = 0; i < master_state.response_count; i++) {
        fip_print_msg(0, &master_state.responses[i]);
        if (master_state.responses[i].type == FIP_MSG_SYMBOL_RESPONSE &&
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

bool fip_master_compile_request( //
    char buffer[FIP_MSG_SIZE],   //
    const fip_msg_t *message     //
) {
    assert(message->type == FIP_MSG_COMPILE_REQUEST);
    fip_master_broadcast_message(buffer, message);
    uint8_t wrong_msg_count =
        fip_master_await_responses(buffer, master_state.responses,
            &master_state.response_count, FIP_MSG_OBJECT_RESPONSE);
    if (wrong_msg_count > 0) {
        fip_print(0, FIP_WARN, "Received %u faulty messages", wrong_msg_count);
    }

    for (uint8_t i = 0; i < master_state.response_count; i++) {
        const fip_msg_t *response = &master_state.responses[i];
        if (response->type != FIP_MSG_OBJECT_RESPONSE) {
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
    }
    master_state.slave_count = 0;
    fip_print(0, FIP_INFO, "Master cleaned up");
}

#endif // End of platform-agnostic master functions

#ifdef FIP_SLAVE

void fip_slave_send_message(   //
    uint32_t id,               //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    fip_encode_msg(buffer, message);
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

void fip_slave_cleanup() {
    fip_print(1, FIP_INFO, "Slave cleaned up");
}

#endif // End of platform-agnostic slave functions

/*
 * ======================
 * WINDOWS IMPLEMENTATION
 * ======================
 */

#ifdef __WIN32__

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
    const char *module              //
) {
    char _module[256] = {0};
    char id[8] = {0};
    char ll[8] = {0};
    strcpy(_module, module);
    strcat(_module, ".exe"); // Add .exe extension
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
    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s %s", _module, id, ll);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si,
            &pi)) {
        fip_print(0, FIP_WARN, "Failed to spawn slave %s: %d", id,
            GetLastError());
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
    char buffer[FIP_MSG_SIZE],             //
    fip_msg_t responses[FIP_MAX_SLAVES],   //
    uint32_t *response_count,              //
    const fip_msg_type_t expected_msg_type //
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

fip_master_config_t fip_master_load_config() {
    const char *file_path = ".fip/config/fip.toml";
    FILE *fp = fopen(file_path, "r");
    fip_master_config_t config = {0};
    config.ok = false;
    if (!fp) {
        fip_print(0, FIP_WARN, "Config file not found: %s", file_path);
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
    fip_print(slave_id, FIP_INFO, "Slave initialized for stdio communication");
    return true;
}

bool fip_slave_receive_message(char buffer[FIP_MSG_SIZE]) {
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

    // Create file actions to redirect slave's stdin/stdout to our pipes
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Redirect slave's stdin to read from our stdin_pipe
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);

    // Redirect slave's stdout to write to our stdout_pipe
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);

    // Redirect slave's stderr to write to our stderr_pipe
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);

    int status = posix_spawn(&modules->pids[modules->active_count], _module,
        &actions, // Use file actions
        NULL,     // No special attributes
        argv, environ);

    posix_spawn_file_actions_destroy(&actions);

    if (status != 0) {
        fip_print(0, FIP_WARN, "Failed to spawn slave %s: %d", id, status);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return false;
    }

    // Close slave's ends of the pipes (they're now handled by the spawned
    // process)
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

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

uint8_t fip_master_await_responses(        //
    char buffer[FIP_MSG_SIZE],             //
    fip_msg_t responses[FIP_MAX_SLAVES],   //
    uint32_t *response_count,              //
    const fip_msg_type_t expected_msg_type //
) {
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

        // Keep trying until we get a message or timeout (10 seconds)
        while (!message_received) {
            // Check if we've exceeded timeout
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = ((double)(now.tv_sec - start.tv_sec)) +
                ((double)(now.tv_nsec - start.tv_nsec) / 1000000000.0);

            if (elapsed > 10.0) {
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

            double remaining = 10.0 - elapsed;
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
                char stderr_buf[4096];
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
                fip_print(0, FIP_WARN, "Recieved message length: %u", msg_len);

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
                fip_decode_msg(buffer, &responses[*response_count]);
                fip_print(0, FIP_INFO, "Received message from slave %d: %s",
                    i + 1, fip_msg_type_str[responses[*response_count].type]);

                if (responses[*response_count].type != expected_msg_type) {
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
            char stderr_buf[4096];
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

fip_master_config_t fip_master_load_config() {
    const char *file_path = ".fip/config/fip.toml";
    FILE *fp = fopen(file_path, "r");
    fip_master_config_t config = {0};
    config.ok = false;
    if (!fp) {
        fip_print(0, FIP_WARN, "Config file not found: %s", file_path);
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
    fip_print(slave_id, FIP_INFO, "Slave initialized for stdio communication");
    return true;
}

bool fip_slave_receive_message(char buffer[FIP_MSG_SIZE]) {
    // Read message length (4 bytes) from stdin
    uint32_t msg_len;
    if (fread(&msg_len, 1, 4, stdin) != 4) {
        return false; // No message available
    }

    // Validate message length
    if (msg_len == 0 || msg_len > FIP_MSG_SIZE - 4) {
        return false;
    }

    // Read message data
    memset(buffer, 0, FIP_MSG_SIZE);
    if (fread(buffer, 1, msg_len, stdin) != msg_len) {
        return false;
    }

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
