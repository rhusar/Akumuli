/** 
 * PUBLIC HEADER
 *
 * Akumuli API.
 * Contains only POD definitions that can be used from "C" code.
 * Should be included only by client code, not by the library itself!
 *
 * Copyright (c) 2016 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "akumuli_config.h"
#include "akumuli_def.h"
#include <apr_errno.h>
#include <stdint.h>

#if !defined _WIN32
#ifdef __cplusplus
#define AKU_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define AKU_EXPORT
#endif
#else
#define AKU_EXPORT __declspec(dllexport)
#endif


//-----------------
// Data structures
//-----------------


//! Database instance.
typedef struct { int padding; } aku_Database;


/**
 * @brief The aku_Cursor struct
 */
typedef struct { int padding; } aku_Cursor;


//! Search stats
typedef struct {
    struct {
        u64 n_times;        //< How many times interpolation search was performed
        u64 n_steps;        //< How many interpolation search steps was performed
        u64 n_overshoots;   //< Number of overruns
        u64 n_undershoots;  //< Number of underruns
        u64 n_matches;      //< Number of matches by interpolation search only
        u64 n_reduced_to_one_page;
        u64 n_page_in_core_checks;  //< Number of page in core checks
        u64 n_page_in_core_errors;  //< Number of page in core check errors
        u64 n_pages_in_core_found;  //< Number of page in core found
        u64 n_pages_in_core_miss;   //< Number of page misses
    } istats;
    struct {
        u64 n_times;  //< How many times binary search was performed
        u64 n_steps;  //< How many binary search steps was performed
    } bstats;
    struct {
        u64 fwd_bytes;  //< Number of scanned bytes in forward direction
        u64 bwd_bytes;  //< Number of scanned bytes in backward direction
    } scan;
} aku_SearchStats;


//! Storage stats
typedef struct {
    u64 n_entries;   //< Total number of entries
    u64 n_volumes;   //< Total number of volumes
    u64 free_space;  //< Free space total
    u64 used_space;  //< Space in use
} aku_StorageStats;


//-------------------
// Utility functions
//-------------------

/** This function must be called before any other library function.
  * @param optional_panic_handler Panic handler.
  * @param logger Logger callback function.
  */
AKU_EXPORT void aku_initialize(aku_panic_handler_t optional_panic_handler, aku_logger_cb_t logger);

/** Convert error code to error message.
  * Function returns pointer to statically allocated string
  * there is no need to free it.
  */
AKU_EXPORT const char* aku_error_message(int error_code);

/** Default logger that is used if no logging function is
  * specified. Exported for testing reasons, no need to use it
  * explicitly.
  */
AKU_EXPORT void aku_console_logger(aku_LogLevel tag, const char* message);

/**
 * @brief Destroy any object created with aku_make_*** function
 */
AKU_EXPORT void aku_destroy(void* any);


//------------------------------
// Storage management functions
//------------------------------


/**
 * @brief Creates storage for new database on the hard drive
 * @param file_name database file name
 * @param metadata_path path to metadata file
 * @param volumes_path path to volumes
 * @param num_volumes number of volumes to create
 * @return APR errorcode or APR_SUCCESS
 * TODO: move from apr_status_t to aku_Status
 */
AKU_EXPORT apr_status_t aku_create_database(const char* file_name, const char* metadata_path,
                                            const char* volumes_path, i32 num_volumes,
                                            aku_logger_cb_t logger);

/**
 * @brief Creates storage for new test database on the hard drive (smaller size then normal DB)
 * @param file_name database file name
 * @param metadata_path path to metadata file
 * @param volumes_path path to volumes
 * @param num_volumes number of volumes to create
 * @return APR errorcode or APR_SUCCESS
 * TODO: move from apr_status_t to aku_Status
 */
AKU_EXPORT apr_status_t aku_create_database_ex(const char* file_name, const char* metadata_path,
                                               const char* volumes_path, i32 num_volumes,
                                               u64 page_size, aku_logger_cb_t logger);


/** Remove all volumes.
  * @param file_name
  * @param logger
  * @returns status
  */
AKU_EXPORT apr_status_t aku_remove_database(const char* file_name, aku_logger_cb_t logger);


/** Open recenlty create storage.
  * @param path path to storage metadata file
  * @param parameters open parameters
  * @return pointer to new db instance, null if db doesn't exists.
  */
AKU_EXPORT aku_Database* aku_open_database(const char* path, aku_FineTuneParams parameters);


/** Check status of previous open operation
  * @param db pointer to database
  */
AKU_EXPORT aku_Status aku_open_status(aku_Database* db);


//! Close database. Free resources.
AKU_EXPORT void aku_close_database(aku_Database* db);

//---------
// Parsing
//---------

/** Try to parse timestamp.
  * @param iso_str should point to the begining of the string
  * @param sample is an output parameter
  * @returns AKU_SUCCESS on success, AKU_EBAD_ARG otherwise
  */
AKU_EXPORT aku_Status aku_parse_timestamp(const char* iso_str, aku_Sample* sample);

/** Convert series name to id. Assign new id to series name on first encounter.
  * @param db opened database instance
  * @param begin should point to the begining of the string
  * @param end should point to the next after end character of the string
  * @param sample is an output parameter
  * @returns AKU_SUCCESS on success, error code otherwise
  */
AKU_EXPORT aku_Status aku_series_to_param_id(aku_Database* db, const char* begin, const char* end,
                                             aku_Sample* sample);

/** Try to parse duration.
  * @param str should point to the begining of the string
  * @param value is an output parameter
  * @returns AKU_SUCCESS on success, AKU_EBAD_ARG otherwise
  */
AKU_EXPORT aku_Status aku_parse_duration(const char* str, int* value);


//---------
// Writing
//---------

/** Write measurement to DB
  * @param db opened database instance
  * @param param_id storage parameter id
  * @param timestamp timestamp
  * @param value parameter value
  * @returns operation status
  */
AKU_EXPORT aku_Status aku_write_double_raw(aku_Database* db, aku_ParamId param_id,
                                           aku_Timestamp timestamp, double value);

/** Write measurement to DB
  * @param db opened database instance
  * @param sample should contain valid measurement value
  * @returns operation status
  */
AKU_EXPORT aku_Status aku_write(aku_Database* db, const aku_Sample* sample);


//---------
// Queries
//---------

/** @brief Query database
  * @param db should point to opened database instance
  * @param query should contain valid query
  * @return cursor instance
  */
AKU_EXPORT aku_Cursor* aku_query(aku_Database* db, const char* query);

/**
 * @brief Close cursor
 * @param pcursor pointer to cursor
 */
AKU_EXPORT void aku_cursor_close(aku_Cursor* pcursor);

/** Read the values under cursor.
  * @param cursor should point to active cursor instance
  * @param dest is an output buffer
  * @param dest_size is an output buffer size
  * @returns number of overwriten bytes
  */
AKU_EXPORT size_t aku_cursor_read(aku_Cursor* cursor, void* dest, size_t dest_size);

//! Check cursor state. Returns zero value if not done yet, non zero value otherwise.
AKU_EXPORT int aku_cursor_is_done(aku_Cursor* pcursor);

//! Check cursor error state. Returns zero value if everything is OK, non zero value otherwise.
AKU_EXPORT int aku_cursor_is_error(aku_Cursor* pcursor, aku_Status* out_error_code_or_null);

/** Convert timestamp to string if possible, return string length
  * @return 0 on bad string, -LEN if buffer is too small, LEN on success
  */
AKU_EXPORT int aku_timestamp_to_string(aku_Timestamp, char* buffer, size_t buffer_size);

/** Convert param-id to series name
  * @param db opened database
  * @param id valid param id
  * @param buffer is a destination buffer
  * @param buffer_size is a destination buffer size
  * @return 0 if no such id, -LEN if buffer is too small, LEN on success
  */
AKU_EXPORT int aku_param_id_to_series(aku_Database* db, aku_ParamId id, char* buffer,
                                      size_t buffer_size);

//--------------------
// Stats and counters
//--------------------

/** DEPRICATED
  * Get search counters.
  * @param rcv_stats pointer to `aku_SearchStats` structure that will be filled with data.
  * @param reset reset all counter if not zero
  */
AKU_EXPORT void aku_global_search_stats(aku_SearchStats* rcv_stats, int reset);

/** DEPRICATED
  * Get storage stats.
  * @param db database instance.
  * @param rcv_stats pointer to destination
  */
AKU_EXPORT void aku_global_storage_stats(aku_Database* db, aku_StorageStats* rcv_stats);

AKU_EXPORT void aku_debug_print(aku_Database* db);

AKU_EXPORT int aku_json_stats(aku_Database* db, char* buffer, size_t size);
