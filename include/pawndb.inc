#if defined _pawndb_included
  #endinput
#endif
#define _pawndb_included

#define PDB: _:
#define PDBSetup: _:
#define PDBResult: _:
#define PDBBatch: _:
#define PDBBatchResult: _:
#define INVALID_PDB (PDB:0)
#define INVALID_PDB_SETUP (PDBSetup:0)
#define INVALID_PDB_RESULT (PDBResult:0)
#define INVALID_PDB_BATCH (PDBBatch:0)
#define INVALID_PDB_BATCH_RESULT (PDBBatchResult:0)

enum PDB_BATCH_ITEM_STATUS {
    BATCH_ITEM_SUCCESS = 0,
    BATCH_ITEM_SQL_ERROR,
    BATCH_ITEM_NOT_EXECUTED,
    BATCH_ITEM_ROLLED_BACK
}

enum PDB_DRIVER {
    PDB_DRIVER_AUTO = 0,
    PDB_DRIVER_MARIADB,
    PDB_DRIVER_POSTGRES
}

enum PDB_SETUP_OPT {
    PDB_OPT_CONNECT_TIMEOUT = 0,
    PDB_OPT_AUTO_RECONNECT,
    PDB_OPT_MULTI_STATEMENTS,
    PDB_OPT_POOL_SIZE
}

enum PDB_METRIC {
    PDB_METRIC_QPS = 0,
    PDB_METRIC_PENDING,
    PDB_METRIC_AVG_LATENCY_US,
    PDB_METRIC_SLOW_QUERIES
}

enum PDBJson {
    PDB_JSON_INVALID = 0
}

native PDBJson:pdb_json_create(bool:retain = false);
native bool:pdb_json_free(PDBJson:handle);
native bool:pdb_json_retain(PDBJson:handle);
native bool:pdb_json_set_int(PDBJson:handle, const key[], value);
native bool:pdb_json_set_float(PDBJson:handle, const key[], Float:value);
native bool:pdb_json_set_str(PDBJson:handle, const key[], const value[]);
native bool:pdb_json_set_bool(PDBJson:handle, const key[], bool:value);
native bool:pdb_json_set_vector(PDBJson:handle, const key[], Float:x, Float:y, Float:z);
native bool:pdb_json_set_vector4(PDBJson:handle, const key[], Float:x, Float:y, Float:z, Float:w);
native bool:pdb_json_to_string(PDBJson:handle, output[], max_len = sizeof(output));

native PDB:pdb_connect(const host[], const user[], const password[], const database[],
                       port = 3306, PDBSetup:setup = INVALID_PDB_SETUP,
                       bool:auto_free_setup = true);
native PDB:pdb_connect_file(const config_file[]);
native bool:pdb_close(PDB:handle);
native bool:pdb_is_connected(PDB:handle);
native bool:pdb_set_debug_level(PDB:handle, level);
native bool:pdb_get_driver_name(PDB:handle, name[], max_len);
native PDBSetup:pdb_setup_init();
native bool:pdb_setup_free(PDBSetup:setup);
native bool:pdb_setup_charset(PDBSetup:setup, const charset[]);
native bool:pdb_setup_option(PDBSetup:setup, PDB_SETUP_OPT:option, value);
native bool:pdb_setup_driver(PDBSetup:setup, PDB_DRIVER:driver);
native bool:pdb_setup_ssl(PDBSetup:setup, const ca_cert[], const client_cert[] = "",
                          const client_key[] = "", bool:verify_server = true);
native bool:pdb_is_update_available();
native pdb_hash(const password[], const callback[], const specifiers[] = "", {Float, _}:...);
native pdb_verify(const password[], const hash[], const callback[], const specifiers[] = "", {Float, _}:...);
native pdb_get_stat(PDB:handle, PDB_METRIC:metric_type);
native bool:pdb_get_metrics(PDB:handle, &qps, &pending, &avg_latency_us, &slow_queries);
native bool:pdb_set_slow_query_threshold(PDB:handle, threshold_us);
native PDBResult:pdb_retain_result();
native bool:pdb_activate_result(PDBResult:result);
native bool:pdb_free_result(PDBResult:result);
native PDBBatch:pdb_batch_create(PDB:handle, bool:atomic = true);
native bool:pdb_batch_free(PDBBatch:batch);
native bool:pdb_batch_add(PDBBatch:batch, const query[], {Float, PDBJson, _}:...);
native bool:pdb_batch_execute(PDBBatch:batch, const callback[] = "",
                              const specifiers[] = "", {Float, _}:...);
native bool:pdb_retain_batch_result(PDBBatchResult:result);
native bool:pdb_free_batch_result(PDBBatchResult:result);
native pdb_batch_size(PDBBatchResult:result);
native PDB_BATCH_ITEM_STATUS:pdb_batch_status(PDBBatchResult:result, index);
native PDBResult:pdb_batch_get_result(PDBBatchResult:result, index);
native bool:pdb_batch_get_error(PDBBatchResult:result, index, output[], max_len = sizeof(output));
native pdb_num_rows();
native pdb_num_fields();
native bool:pdb_next_row();
native bool:pdb_has_next_result();
native bool:pdb_next_result();
native pdb_insert_id();
native pdb_affected_rows();
native pdb_exec_time();
native pdb_warning_count();
native bool:pdb_seek_row(row_index);
native bool:pdb_field_name(field_index, output[], max_len = sizeof(output));
native bool:pdb_is_null(const field_name[], row = -1);
native bool:pdb_is_null_by_index(field_index, row = -1);
native pdb_get_int(const field_name[], row = -1);
native Float:pdb_get_float(const field_name[], row = -1);
native bool:pdb_get_str(const field_name[], output[], max_len = sizeof(output), row = -1);
native bool:pdb_get_bool(const field_name[], row = -1);
native pdb_get_int_by_index(field_index, row = -1);
native Float:pdb_get_float_by_index(field_index, row = -1);
native bool:pdb_get_str_by_index(field_index, output[], max_len = sizeof(output), row = -1);
native bool:pdb_get_bool_by_index(field_index, row = -1);
native pdb_json_get_int(const field_name[], const json_path[], row = -1);
native Float:pdb_json_get_float(const field_name[], const json_path[], row = -1);
native bool:pdb_json_get_bool(const field_name[], const json_path[], row = -1);
native bool:pdb_json_get_str(const field_name[], const json_path[], output[], max_len = sizeof(output), row = -1);
native bool:pdb_json_get_vector(const field_name[], const json_path[], &Float:x, &Float:y, &Float:z, row = -1);
native bool:pdb_json_get_vector4(const field_name[], const json_path[], &Float:x, &Float:y, &Float:z, &Float:w, row = -1);
native bool:pdb_get_vector(const field_name[], &Float:x, &Float:y, &Float:z, row = -1);
native bool:pdb_get_vector_by_index(field_index, &Float:x, &Float:y, &Float:z, row = -1);
native bool:pdb_get_vector4(const field_name[], &Float:x, &Float:y, &Float:z, &Float:w, row = -1);
native bool:pdb_get_vector4_by_index(field_index, &Float:x, &Float:y, &Float:z, &Float:w, row = -1);

stock pdb_get_num_rows(&destination = 0) {
    destination = pdb_num_rows();
    return destination;
}

stock pdb_get_num_fields(&destination = 0) {
    destination = pdb_num_fields();
    return destination;
}

stock bool:pdb_fetch_int(const field_name[], &destination, row = -1) {
    destination = pdb_get_int(field_name, row);
    return !pdb_is_null(field_name, row);
}

stock bool:pdb_fetch_float(const field_name[], &Float:destination, row = -1) {
    destination = pdb_get_float(field_name, row);
    return !pdb_is_null(field_name, row);
}

stock bool:pdb_fetch_bool(const field_name[], &bool:destination, row = -1) {
    destination = pdb_get_bool(field_name, row);
    return !pdb_is_null(field_name, row);
}

stock bool:pdb_fetch_str(const field_name[], destination[], max_len = sizeof(destination), row = -1) {
    return pdb_get_str(field_name, destination, max_len, row);
}

stock bool:pdb_fetch_int_by_index(field_index, &destination, row = -1) {
    destination = pdb_get_int_by_index(field_index, row);
    return !pdb_is_null_by_index(field_index, row);
}

stock bool:pdb_fetch_float_by_index(field_index, &Float:destination, row = -1) {
    destination = pdb_get_float_by_index(field_index, row);
    return !pdb_is_null_by_index(field_index, row);
}

stock bool:pdb_fetch_bool_by_index(field_index, &bool:destination, row = -1) {
    destination = pdb_get_bool_by_index(field_index, row);
    return !pdb_is_null_by_index(field_index, row);
}

stock bool:pdb_fetch_str_by_index(field_index, destination[], max_len = sizeof(destination), row = -1) {
    return pdb_get_str_by_index(field_index, destination, max_len, row);
}

stock bool:pdb_json_fetch_int(const field_name[], const json_path[], &destination, row = -1) {
    destination = pdb_json_get_int(field_name, json_path, row);
    return !pdb_is_null(field_name, row);
}

stock bool:pdb_json_fetch_float(const field_name[], const json_path[], &Float:destination, row = -1) {
    destination = pdb_json_get_float(field_name, json_path, row);
    return !pdb_is_null(field_name, row);
}

stock bool:pdb_json_fetch_bool(const field_name[], const json_path[], &bool:destination, row = -1) {
    destination = pdb_json_get_bool(field_name, json_path, row);
    return !pdb_is_null(field_name, row);
}

native pdb_format(PDB:handle, output[], max_len = sizeof(output), const format[],
                  {Float, PDBJson, _}:...);
native pdb_execute(PDB:handle, const format[], {Float, PDBJson, _}:...);
native pdb_query(PDB:handle, const format[], const callback[] = "", const specifiers[] = "",
                 {Float, PDBJson, _}:...);

forward OnQueryError(PDB:handle, error_id, const error_msg[], const query[]);
forward OnBatchComplete(PDB:handle, PDBBatchResult:result);

#if defined PAWNDB_COMPAT
    #if defined mysql_included || defined _INC_mysql
        #error Remove the legacy MySQL include or include pawndb before it.
    #endif
    #define mysql_included
    #define _INC_mysql
    #define MySQL: PDB:
    #define Cache: PDBResult:
    #define ResultSet: PDBResult:
    #define ORM: _:
    #define Model: _:
    #define MYSQL_INVALID_HANDLE INVALID_PDB
    #define INVALID_MYSQL_ID INVALID_PDB
    #define MYSQL_INVALID_CACHE INVALID_PDB_RESULT
    #define MYSQL_INVALID_ORM (0)
    #define MYSQL_INVALID_MODEL (0)
    #define MYSQL_DEFAULT_HANDLE INVALID_PDB

    #define ER_DBACCESS_DENIED_ERROR (1044)
    #define ER_ACCESS_DENIED_ERROR (1045)
    #define ER_DUP_ENTRY (1062)
    #define ER_UNKNOWN_TABLE (1109)
    #define ER_SYNTAX_ERROR (1149)
    #define CR_SERVER_GONE_ERROR (2006)
    #define CR_SERVER_LOST (2013)
    #define CR_COMMAND_OUT_OF_SYNC (2014)
    #define CR_SSL_CONNECTION_ERROR (2026)
    #define CR_SERVER_LOST_EXTENDED (2055)
    enum E_MYSQL_EXECTIME_UNIT { MILLISECONDS, MICROSECONDS }
    enum E_MYSQL_FIELD_TYPE {
        MYSQL_TYPE_INVALID = -1, MYSQL_TYPE_DECIMAL, MYSQL_TYPE_TINY, MYSQL_TYPE_SHORT,
        MYSQL_TYPE_LONG, MYSQL_TYPE_FLOAT, MYSQL_TYPE_DOUBLE, MYSQL_TYPE_NULL,
        MYSQL_TYPE_TIMESTAMP, MYSQL_TYPE_LONGLONG, MYSQL_TYPE_INT24, MYSQL_TYPE_DATE,
        MYSQL_TYPE_TIME, MYSQL_TYPE_DATETIME, MYSQL_TYPE_YEAR, MYSQL_TYPE_NEWDATE,
        MYSQL_TYPE_VARCHAR, MYSQL_TYPE_BIT, MYSQL_TYPE_TIMESTAMP2, MYSQL_TYPE_DATETIME2,
        MYSQL_TYPE_TIME2, MYSQL_TYPE_JSON = 245, MYSQL_TYPE_NEWDECIMAL, MYSQL_TYPE_ENUM,
        MYSQL_TYPE_SET, MYSQL_TYPE_TINY_BLOB, MYSQL_TYPE_MEDIUM_BLOB, MYSQL_TYPE_LONG_BLOB,
        MYSQL_TYPE_BLOB, MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_STRING, MYSQL_TYPE_GEOMETRY
    }
    enum E_MYSQL_HASH_ALGO { HASH_ARGON2ID, HASH_BCRYPT }
    enum E_ORM_ERROR { ERROR_INVALID, ERROR_OK, ERROR_NO_DATA }
    enum E_MYSQL_MODEL_ERROR { MODEL_OK, MODEL_NO_DATA, MODEL_INVALID }

    #define mysql_format pdb_format
    #define mysql_update_available pdb_is_update_available
    // shortcut: query aliases use PDB formatting; double literal % until a raw-query API is specified.
    #define mysql_tquery pdb_query
    #define mysql_pquery pdb_query
    #define mysql_execute pdb_query
    #define mysql_verify pdb_verify
    #define cache_save pdb_retain_result
    #define mysql_rs_retain pdb_retain_result
    #define cache_set_active pdb_activate_result
    #define mysql_rs_activate pdb_activate_result
    #define cache_delete pdb_free_result
    #define mysql_rs_release pdb_free_result
    #define cache_unset_active() pdb_activate_result(INVALID_PDB_RESULT)
    #define mysql_rs_deactivate() pdb_activate_result(INVALID_PDB_RESULT)
    #define cache_num_rows pdb_num_rows
    #define cache_num_fields pdb_num_fields
    #define mysql_rs_num_rows pdb_num_rows
    #define mysql_rs_num_fields pdb_num_fields
    stock cache_get_row_count(&destination) { destination = pdb_num_rows(); return 1; }
    stock cache_get_field_count(&destination) { destination = pdb_num_fields(); return 1; }
    #define mysql_rs_row_count cache_get_row_count
    #define mysql_rs_field_count cache_get_field_count
    #define cache_get_field_name pdb_field_name
    #define mysql_rs_field_name pdb_field_name
    #define cache_insert_id pdb_insert_id
    #define cache_affected_rows pdb_affected_rows
    #define cache_warning_count pdb_warning_count
    #define mysql_rs_insert_id pdb_insert_id
    #define mysql_rs_affected_rows pdb_affected_rows
    #define mysql_rs_warning_count pdb_warning_count
    #define mysql_rs_exec_time cache_get_query_exec_time

    stock cache_get_query_exec_time(E_MYSQL_EXECTIME_UNIT:unit = MICROSECONDS) {
        return unit == MILLISECONDS ? pdb_exec_time() / 1000 : pdb_exec_time();
    }

    // Only forward traversal exists in the PDB result-chain API.
    #pragma deprecated Forward relative offset only; migrate indexed result selection to pdb_next_result.
    stock cache_set_result(result_index) {
        if (result_index < 0) return 0;
        while (result_index--) if (!pdb_next_result()) return 0;
        return 1;
    }
    #define mysql_rs_select_set cache_set_result

    stock PDB_CompatPlayerFormat(const format[]) {
        new combined[128];
        combined[0] = 'i';
        new i;
        for (i = 0; format[i] && i < sizeof(combined) - 2; ++i) combined[i + 1] = format[i];
        if (format[i]) combined[1] = '!';
        return combined;
    }
    #define mysql_execute_for(%0) (_:PDBEF0:PDBEF1:PDBEF2:PDBEF3:PDB_CompatExecuteFor(%0)PDB_EXEC_END)
    #define PDBEF0:PDBEF1:PDBEF2:PDBEF3:PDB_CompatExecuteFor(%0,%1,%2,%3,%4,%5)PDB_EXEC_END pdb_query(%0,%2,%3,PDB_CompatPlayerFormat(%4),%1,%5)
    #define PDBEF1:PDBEF2:PDBEF3:PDB_CompatExecuteFor(%0,%1,%2,%3,%4)PDB_EXEC_END pdb_query(%0,%2,%3,PDB_CompatPlayerFormat(%4),%1)
    #define PDBEF2:PDBEF3:PDB_CompatExecuteFor(%0,%1,%2,%3)PDB_EXEC_END pdb_query(%0,%2,%3,"i",%1)
    #define PDBEF3:PDB_CompatExecuteFor(%0,%1,%2)PDB_EXEC_END pdb_query(%0,%2,"","i",%1)

    stock bool:PDB_CompatArgon(E_MYSQL_HASH_ALGO:algo) { return algo == HASH_ARGON2ID; }
    #define mysql_hash(%0) (_:PDBH0:PDBH1:PDBH2:PDBH3:PDB_CompatHash(%0)PDB_HASH_END)
    #define PDBH0:PDBH1:PDBH2:PDBH3:PDB_CompatHash(%0,%1,%2,%3,%4)PDB_HASH_END (PDB_CompatArgon(%3) ? pdb_hash(%0,%1,%2,%4) : 0)
    #define PDBH1:PDBH2:PDBH3:PDB_CompatHash(%0,%1,%2,%3)PDB_HASH_END (PDB_CompatArgon(%3) ? pdb_hash(%0,%1,%2) : 0)
    #define PDBH2:PDBH3:PDB_CompatHash(%0,%1,%2)PDB_HASH_END pdb_hash(%0,%1,%2)
    #define PDBH3:PDB_CompatHash(%0,%1)PDB_HASH_END pdb_hash(%0,%1)

    stock cache_get_value_name(row, const column[], destination[], max_len = sizeof(destination)) {
        return pdb_get_str(column, destination, max_len, row);
    }
    stock cache_get_value_name_int(row, const column[], &destination) {
        destination = pdb_get_int(column, row);
        return !pdb_is_null(column, row);
    }
    stock cache_get_value_name_float(row, const column[], &Float:destination) {
        destination = pdb_get_float(column, row);
        return !pdb_is_null(column, row);
    }
    stock cache_get_value_name_bool(row, const column[], &bool:destination) {
        destination = pdb_get_bool(column, row);
        return !pdb_is_null(column, row);
    }
    stock cache_is_value_name_null(row, const column[], &bool:destination) {
        destination = pdb_is_null(column, row);
        return 1;
    }

    stock cache_get_value_index(row, column, destination[], max_len = sizeof(destination)) {
        return pdb_get_str_by_index(column, destination, max_len, row);
    }
    stock cache_get_value_index_int(row, column, &destination) {
        destination = pdb_get_int_by_index(column, row);
        return !pdb_is_null_by_index(column, row);
    }
    stock cache_get_value_index_float(row, column, &Float:destination) {
        destination = pdb_get_float_by_index(column, row);
        return !pdb_is_null_by_index(column, row);
    }
    stock cache_get_value_index_bool(row, column, &bool:destination) {
        destination = pdb_get_bool_by_index(column, row);
        return !pdb_is_null_by_index(column, row);
    }
    stock cache_is_value_index_null(row, column, &bool:destination) {
        destination = pdb_is_null_by_index(column, row);
        return 1;
    }
    #define cache_get_value(%0) (_:PDBCVstr0:PDBCVstr1:PDBCVstr2:PDBCVstr(%0))
    #define PDBCVstr0:PDBCVstr1:PDBCVstr2:PDBCVstr(%0,"%1",%2) cache_get_value_name(%0,#%1,%2)
    #define PDBCVstr1:PDBCVstr2:PDBCVstr(%0,%8string%9:%1,%2) cache_get_value_name(%0,%1,%2)
    #define PDBCVstr2:PDBCVstr(%0,%1,%2) cache_get_value_index(%0,%1,%2)
    #define cache_get_value_int(%0) (_:PDBCVint0:PDBCVint1:PDBCVint2:PDBCVint(%0))
    #define PDBCVint0:PDBCVint1:PDBCVint2:PDBCVint(%0,"%1",%2) cache_get_value_name_int(%0,#%1,%2)
    #define PDBCVint1:PDBCVint2:PDBCVint(%0,%8string%9:%1,%2) cache_get_value_name_int(%0,%1,%2)
    #define PDBCVint2:PDBCVint(%0,%1,%2) cache_get_value_index_int(%0,%1,%2)
    #define cache_get_value_float(%0) (_:PDBCVfloat0:PDBCVfloat1:PDBCVfloat2:PDBCVfloat(%0))
    #define PDBCVfloat0:PDBCVfloat1:PDBCVfloat2:PDBCVfloat(%0,"%1",%2) cache_get_value_name_float(%0,#%1,%2)
    #define PDBCVfloat1:PDBCVfloat2:PDBCVfloat(%0,%8string%9:%1,%2) cache_get_value_name_float(%0,%1,%2)
    #define PDBCVfloat2:PDBCVfloat(%0,%1,%2) cache_get_value_index_float(%0,%1,%2)
    #define cache_get_value_bool(%0) (_:PDBCVbool0:PDBCVbool1:PDBCVbool2:PDBCVbool(%0))
    #define PDBCVbool0:PDBCVbool1:PDBCVbool2:PDBCVbool(%0,"%1",%2) cache_get_value_name_bool(%0,#%1,%2)
    #define PDBCVbool1:PDBCVbool2:PDBCVbool(%0,%8string%9:%1,%2) cache_get_value_name_bool(%0,%1,%2)
    #define PDBCVbool2:PDBCVbool(%0,%1,%2) cache_get_value_index_bool(%0,%1,%2)
    #define cache_is_value_null(%0) (_:PDBCVnull0:PDBCVnull1:PDBCVnull2:PDBCVnull(%0))
    #define PDBCVnull0:PDBCVnull1:PDBCVnull2:PDBCVnull(%0,"%1",%2) cache_is_value_name_null(%0,#%1,%2)
    #define PDBCVnull1:PDBCVnull2:PDBCVnull(%0,%8string%9:%1,%2) cache_is_value_name_null(%0,%1,%2)
    #define PDBCVnull2:PDBCVnull(%0,%1,%2) cache_is_value_index_null(%0,%1,%2)
    #define mysql_rs_get_string cache_get_value_index
    #define mysql_rs_get_string_by cache_get_value_name
    #define mysql_rs_value cache_get_value_name
    #define mysql_rs_get_int cache_get_value_index_int
    #define mysql_rs_get_int_by cache_get_value_name_int
    #define mysql_rs_value_int cache_get_value_name_int
    #define mysql_rs_get_float cache_get_value_index_float
    #define mysql_rs_get_float_by cache_get_value_name_float
    #define mysql_rs_value_float cache_get_value_name_float
    #define mysql_rs_get_bool cache_get_value_index_bool
    #define mysql_rs_get_bool_by cache_get_value_name_bool
    #define mysql_rs_value_bool cache_get_value_name_bool
    #define mysql_rs_is_null cache_is_value_index_null
    #define mysql_rs_is_null_by cache_is_value_name_null
    #define mysql_rs_value_is_null cache_is_value_name_null

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock ORM:orm_create(const table[], MySQL:handle = MYSQL_DEFAULT_HANDLE) {
        #pragma unused table, handle
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_destroy(ORM:id) {
        #pragma unused id
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock E_ORM_ERROR:orm_errno(ORM:id) {
        #pragma unused id
        return ERROR_INVALID;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_apply_cache(ORM:id, row_idx, result_idx = 0) {
        #pragma unused id, row_idx, result_idx
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_select(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_update(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_insert(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_delete(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_load(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_save(ORM:id, const callback[] = "", const format[] = "", {Float, _}:...) {
        #pragma unused id, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_addvar_int(ORM:id, &var, const columnname[]) {
        #pragma unused id, var, columnname
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_addvar_float(ORM:id, &Float:var, const columnname[]) {
        #pragma unused id, var, columnname
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_addvar_string(ORM:id, var[], var_maxlen, const columnname[]) {
        #pragma unused id, var, var_maxlen, columnname
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_clear_vars(ORM:id) {
        #pragma unused id
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_delvar(ORM:id, const columnname[]) {
        #pragma unused id, columnname
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock orm_setkey(ORM:id, const columnname[]) {
        #pragma unused id, columnname
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock Model:mysql_model_create(const table[], MySQL:handle = MYSQL_DEFAULT_HANDLE) {
        #pragma unused table, handle
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_destroy(Model:model) {
        #pragma unused model
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock E_MYSQL_MODEL_ERROR:mysql_model_errno(Model:model) {
        #pragma unused model
        return MODEL_INVALID;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_bind_int(Model:model, &variable, const column[]) {
        #pragma unused model, variable, column
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_bind_float(Model:model, &Float:variable, const column[]) {
        #pragma unused model, variable, column
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_bind_string(Model:model, variable[], variable_maxlen, const column[]) {
        #pragma unused model, variable, variable_maxlen, column
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_unbind(Model:model, const column[]) {
        #pragma unused model, column
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_clear(Model:model) {
        #pragma unused model
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_set_key(Model:model, const column[]) {
        #pragma unused model, column
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_find(Model:model, const callback[] = "", const format[] = "", {Float,_}:...) {
        #pragma unused model, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_insert(Model:model, const callback[] = "", const format[] = "", {Float,_}:...) {
        #pragma unused model, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_update(Model:model, const callback[] = "", const format[] = "", {Float,_}:...) {
        #pragma unused model, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_save(Model:model, const callback[] = "", const format[] = "", {Float,_}:...) {
        #pragma unused model, callback, format
        return 0;
    }

    #pragma deprecated ORM/model removed; migrate to SQL and PDBJson.
    stock mysql_model_delete(Model:model, const callback[] = "", const format[] = "", {Float,_}:...) {
        #pragma unused model, callback, format
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock Cache:mysql_query(MySQL:handle, const query[], bool:use_cache = true) {
        #pragma unused handle, query, use_cache
        return INVALID_PDB_RESULT;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock Cache:mysql_query_file(MySQL:handle, const file_path[], bool:use_cache = false) {
        #pragma unused handle, file_path, use_cache
        return INVALID_PDB_RESULT;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock ResultSet:mysql_execute_sync(MySQL:handle, const query[], bool:use_cache = true) {
        #pragma unused handle, query, use_cache
        return INVALID_PDB_RESULT;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock ResultSet:mysql_execute_file_sync(MySQL:handle, const file_path[], bool:use_cache = false) {
        #pragma unused handle, file_path, use_cache
        return INVALID_PDB_RESULT;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock mysql_hash_sync(const password[], destination[], maxlength = sizeof(destination), E_MYSQL_HASH_ALGO:algo = HASH_ARGON2ID) {
        #pragma unused password, algo
        if (maxlength > 0) destination[0] = 0;
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock bool:mysql_verify_sync(const password[], const hash[]) {
        #pragma unused password, hash
        return false;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock mysql_escape_string(const source[], destination[], max_len = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE) {
        #pragma unused source, handle
        if (max_len > 0) destination[0] = 0;
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock mysql_errno(MySQL:handle = MYSQL_DEFAULT_HANDLE) {
        #pragma unused handle
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock mysql_error(destination[], max_len = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE) {
        #pragma unused handle
        if (max_len > 0) destination[0] = 0;
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock bool:cache_is_any_active() {
        return false;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock bool:cache_is_valid(Cache:cache_id) {
        #pragma unused cache_id
        return false;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock cache_get_result_count(&destination) {
        destination = 0;
        return 0;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock E_MYSQL_FIELD_TYPE:cache_get_field_type(field_index) {
        #pragma unused field_index
        return MYSQL_TYPE_INVALID;
    }

    #pragma deprecated Unsupported legacy API; migrate to the documented asynchronous PDB API.
    stock cache_get_query_string(destination[], max_len = sizeof(destination)) {
        if (max_len > 0) destination[0] = 0;
        return 0;
    }

    #define mysql_rs_is_active cache_is_any_active
    #define mysql_rs_is_valid cache_is_valid
    #define mysql_rs_set_count cache_get_result_count
    #define mysql_rs_field_type cache_get_field_type
    #define mysql_rs_query cache_get_query_string
    stock cache_num_results() { new n; cache_get_result_count(n); return n; }
    #define mysql_rs_num_sets cache_num_results
#endif
