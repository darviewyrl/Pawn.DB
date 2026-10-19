#define PAWNDB_COMPAT
#include <pawndb>
#pragma rational Float

native PDB:fixture_connection();
native PDBResult:fixture_result();
native probe(bool:ok, line);
native bool:equal(const a[], const b[]);
native finished();
native bool:fixture_nested();
forward OnNested();
#define CHECK(%0) probe(bool:(%0), 0)

forward OnLegacy(value, Float:ratio, const text[]);
forward OnPlayer(playerid, value, const text[]);
forward OnHash(value, const hash[]);
forward OnDefaultHash(const hash[]);
forward OnVerify(value, bool:ok);
forward OnDefaultVerify(bool:ok);

main() {
    new MySQL:db = fixture_connection();
    new Cache:res = fixture_result();
    CHECK(cache_num_rows() == 2 && mysql_rs_num_fields() == 5);
    new n, Float:f, bool:b, text[128];
    CHECK(pdb_get_num_rows() == 2 && pdb_get_num_fields() == 5);
    CHECK(pdb_fetch_float("score", f, 1) && f == 2.5);
    CHECK(pdb_fetch_bool_by_index(3, b, 1) && !b);
    CHECK(pdb_fetch_int_by_index(0, n, 0) && n == 42);
    CHECK(pdb_fetch_str_by_index(2, text, sizeof(text), 1) && equal(text, "next"));
    CHECK(pdb_fetch_str("name", text, sizeof(text), 0) && equal(text, "pawn"));
    CHECK(cache_get_row_count(n) == 1 && n == 2);
    CHECK(mysql_rs_field_count(n) == 1 && n == 5);
    CHECK(cache_get_value_int(0, "id", n) && n == 42);
    CHECK(cache_get_value_int(1, 0, n) && n == 43);
    CHECK(cache_get_value_float(0, "score", f) && f == 1.25);
    CHECK(cache_get_value_bool(0, "flag", b) && b);
    CHECK(cache_get_value(0, "name", text) && equal(text, "pawn"));
    new column[] = "id";
    CHECK(cache_get_value_int(0, string:column, n) && n == 42);
    CHECK(cache_is_value_null(0, "nil", b) && b);
    CHECK(cache_is_value_null(0, 4, b) && b);
    CHECK(mysql_rs_get_int(1, 0, n) && n == 43);
    CHECK(mysql_rs_get_float_by(0, "score", f) && f == 1.25);
    CHECK(mysql_rs_value_bool(0, "flag", b) && b);
    CHECK(mysql_rs_get_string(0, 2, text) && equal(text, "pawn"));
    CHECK(mysql_rs_value(0, "name", text) && equal(text, "pawn"));
    CHECK(mysql_rs_is_null(0, 4, b) && b);
    CHECK(mysql_rs_is_null_by(0, "nil", b) && b);
    CHECK(cache_insert_id() == 77 && mysql_rs_affected_rows() == 2);
    CHECK(cache_get_query_exec_time() == 1500 && mysql_rs_exec_time(MILLISECONDS) == 1);
    CHECK(cache_warning_count() == 1 && mysql_rs_warning_count() == 1);
    CHECK(cache_get_field_name(0, text) && equal(text, "id"));
    CHECK(cache_save() == res);
    CHECK(cache_unset_active());
    CHECK(cache_set_active(res));
    CHECK(pdb_get_int("id") == 42);
    CHECK(mysql_rs_deactivate());
    CHECK(mysql_rs_activate(res));
    CHECK(mysql_rs_retain() == res);
    CHECK(mysql_rs_release(res));
    CHECK(pdb_retain_result() == INVALID_PDB_RESULT);
    CHECK(cache_delete(res) == false);
    CHECK(mysql_format(db, text, sizeof(text), "SELECT %d", 42) > 0 && equal(text, "SELECT 42"));
    CHECK(mysql_tquery(db, text, "OnLegacy", "ifs", 7, 1.25, "payload"));
    CHECK(mysql_pquery(db, "SELECT 42", "OnLegacy", "ifs", 7, 1.25, "payload"));
    CHECK(mysql_execute(db, "SELECT 42", "OnLegacy", "ifs", 7, 1.25, "payload"));
    CHECK(mysql_execute_for(db, 99, "SELECT 42", "OnPlayer", "is", 7, "payload"));
    CHECK(mysql_tquery(db, "SELECT no_callback"));
    CHECK(mysql_hash("secret", "OnHash", "i", HASH_ARGON2ID, 77));
    CHECK(mysql_hash("secret", "OnDefaultHash"));
    CHECK(mysql_hash("secret", "OnHash", "i", HASH_BCRYPT, 77) == 0);
    CHECK(mysql_hash("secret", "OnDefaultHash", "", HASH_BCRYPT) == 0 && mysql_hash("secret", "OnDefaultHash", "", HASH_BCRYPT) == 0);
    CHECK(mysql_query(db, "never") == MYSQL_INVALID_CACHE);
    CHECK(mysql_query_file(db, "never.sql") == MYSQL_INVALID_CACHE);
    CHECK(mysql_execute_sync(db, "never") == MYSQL_INVALID_CACHE);
    CHECK(mysql_execute_file_sync(db, "never.sql") == MYSQL_INVALID_CACHE);
    CHECK(mysql_hash_sync("secret", text) == 0 && text[0] == 0);
    CHECK(mysql_verify_sync("secret", "hash") == false);
    CHECK(mysql_escape_string("secret", text) == 0 && text[0] == 0);
    CHECK(mysql_errno(db) == 0);
    CHECK(mysql_error(text, sizeof(text), db) == 0 && text[0] == 0);
    CHECK(orm_create("players", db) == MYSQL_INVALID_ORM);
    CHECK(mysql_model_create("players", db) == MYSQL_INVALID_MODEL);
    CHECK(orm_errno(0) == ERROR_INVALID && mysql_model_errno(0) == MODEL_INVALID);
}

public OnLegacy(value, Float:ratio, const text[]) {
    CHECK(value == 7 && ratio == 1.25 && equal(text, "payload"));
    CHECK(pdb_get_int("id") == 42);
    CHECK(pdb_next_row() && pdb_get_int("id") == 43);
    CHECK(fixture_nested());
    CHECK(pdb_get_int("id") == 43);
    CHECK(pdb_get_int("id", 0) == 42);
    CHECK(pdb_fetch_int("id", value, 1) && value == 43);
    CHECK(cache_get_value_int(0, "id", value) && value == 42);
    finished();
}
public OnPlayer(playerid, value, const text[]) {
    CHECK(playerid == 99 && value == 7 && equal(text, "payload"));
    CHECK(mysql_rs_row_count(value) == 1 && value == 2);
    finished();
}
public OnHash(value, const hash[]) {
    CHECK(value == 77 && hash[0] == '$');
    CHECK(mysql_verify("secret", hash, "OnVerify", "i", value));
    finished();
}
public OnDefaultHash(const hash[]) {
    CHECK(hash[0] == '$');
    CHECK(mysql_verify("wrong", hash, "OnDefaultVerify"));
    finished();
}
public OnVerify(value, bool:ok) { CHECK(value == 77 && ok); finished(); }
public OnDefaultVerify(bool:ok) { CHECK(!ok); finished(); }

public OnNested() { CHECK(pdb_get_int("id") == 99); return 1; }
