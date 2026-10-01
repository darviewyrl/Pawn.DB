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
native pdb_get_stat(PDB:handle, PDB_METRIC:metric_type);
native bool:pdb_get_metrics(PDB:handle, &qps, &pending, &avg_latency_us, &slow_queries);
native bool:pdb_set_slow_query_threshold(PDB:handle, threshold_us);
native bool:pdb_retain_result(PDBResult:result);
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
native pdb_num_rows(PDBResult:result);
native pdb_num_fields(PDBResult:result);
native bool:pdb_next_row(PDBResult:result);
native bool:pdb_has_next_result(PDBResult:result);
native bool:pdb_next_result(PDBResult:result);
native pdb_insert_id(PDBResult:result);
native pdb_affected_rows(PDBResult:result);
native pdb_exec_time(PDBResult:result);
native pdb_warning_count(PDBResult:result);
native bool:pdb_seek_row(PDBResult:result, row_index);
native bool:pdb_field_name(PDBResult:result, field_index, output[], max_len = sizeof(output));
native bool:pdb_is_null(PDBResult:result, const field_name[]);
native bool:pdb_is_null_by_index(PDBResult:result, field_index);
native pdb_get_int(PDBResult:result, const field_name[]);
native Float:pdb_get_float(PDBResult:result, const field_name[]);
native bool:pdb_get_str(PDBResult:result, const field_name[], output[], max_len = sizeof(output));
native bool:pdb_get_bool(PDBResult:result, const field_name[]);
native pdb_get_int_by_index(PDBResult:result, field_index);
native Float:pdb_get_float_by_index(PDBResult:result, field_index);
native bool:pdb_get_str_by_index(PDBResult:result, field_index, output[], max_len = sizeof(output));
native bool:pdb_get_bool_by_index(PDBResult:result, field_index);

stock pdb_get_num_rows(PDBResult:res, &destination) {
    destination = pdb_num_rows(res);
    return destination;
}

stock pdb_get_num_fields(PDBResult:res, &destination) {
    destination = pdb_num_fields(res);
    return destination;
}

stock bool:pdb_fetch_int(PDBResult:res, const field_name[], &destination) {
    destination = pdb_get_int(res, field_name);
    return !pdb_is_null(res, field_name);
}

stock bool:pdb_fetch_float(PDBResult:res, const field_name[], &Float:destination) {
    destination = pdb_get_float(res, field_name);
    return !pdb_is_null(res, field_name);
}

stock bool:pdb_fetch_bool(PDBResult:res, const field_name[], &bool:destination) {
    destination = pdb_get_bool(res, field_name);
    return !pdb_is_null(res, field_name);
}

stock bool:pdb_fetch_int_by_index(PDBResult:res, field_index, &destination) {
    destination = pdb_get_int_by_index(res, field_index);
    return !pdb_is_null_by_index(res, field_index);
}

stock bool:pdb_fetch_float_by_index(PDBResult:res, field_index, &Float:destination) {
    destination = pdb_get_float_by_index(res, field_index);
    return !pdb_is_null_by_index(res, field_index);
}

stock bool:pdb_fetch_bool_by_index(PDBResult:res, field_index, &bool:destination) {
    destination = pdb_get_bool_by_index(res, field_index);
    return !pdb_is_null_by_index(res, field_index);
}

native pdb_format(PDB:handle, output[], max_len = sizeof(output), const format[],
                  {Float, PDBJson, _}:...);
native pdb_execute(PDB:handle, const format[], {Float, _}:...);
native pdb_query(PDB:handle, const format[], const callback[], const specifiers[] = "",
                 {Float, _}:...);

forward OnQueryError(PDB:handle, error_id, const error_msg[], const query[]);
forward OnBatchComplete(PDB:handle, PDBBatchResult:result);
