#if defined _pawndb_included
  #endinput
#endif
#define _pawndb_included

#define PDB: _:
#define PDBSetup: _:
#define INVALID_PDB (PDB:0)
#define INVALID_PDB_SETUP (PDBSetup:0)

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

native pdb_format(PDB:handle, output[], max_len = sizeof(output), const format[],
                  {Float, PDBJson, _}:...);
native pdb_execute(PDB:handle, const format[], {Float, _}:...);
native pdb_query(PDB:handle, const format[], const callback[], const specifiers[] = "",
                 {Float, _}:...);

forward OnQueryError(PDB:handle, error_id, const error_msg[], const query[]);
