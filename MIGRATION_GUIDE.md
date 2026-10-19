# Migration guide: pBlueG MySQL R41-4 and omp-mysql

This guide covers the compatibility surface implemented by Pawn.DB in `pawndb.inc`. It is intended to make common source conversions explicit; compile with warnings enabled and review every deprecated warning. It does not claim full API or behavioral equivalence with either upstream project.

## 1. Enable compatibility mode and order includes

```pawn
#define PAWNDB_COMPAT
#include <pawndb>
```

Remove `<a_mysql>` or `<omp_mysql>` from the script and its include chain. `pawndb.inc` defines known guards `mysql_included` and `_INC_mysql` after loading its compatibility declarations. If either guard is already defined when compatibility mode is enabled, the include emits an error asking you to remove the legacy include or load Pawn.DB first. Do not include both APIs and rely on header order to choose definitions.

Without `PAWNDB_COMPAT`, only the Pawn.DB API is exposed. The C++ AMX native table remains the 75 `pdb_*` names; legacy names are Pawn preprocessor aliases and stocks in `pawndb.inc`.

## 2. Connection and configuration

Legacy calls often stored credentials in Pawn source or created a connection with many positional parameters. Move those secrets into a server-side `configs/database.json` and call `pdb_connect_file`:

```pawn
// Before (representative legacy code)
new MySQL:gSQL = mysql_connect("127.0.0.1", "root", "secret", "server_db");

// After
new PDB:gDatabase = pdb_connect_file("configs/database.json");
```

```json
{
  "driver": "mysql",
  "host": "127.0.0.1",
  "port": 3306,
  "user": "game_server",
  "password": "read-from-deployment-secret",
  "database": "server_db",
  "charset": "utf8mb4",
  "auto_reconnect": true,
  "multi_statements": false,
  "pool_size": 4,
  "debug_level": 1,
  "ssl": { "enable": false, "ca_cert": "", "client_cert": "", "client_key": "", "verify_server_cert": true }
}
```

The handle can be pending. `INVALID_PDB` means Pawn.DB could not accept/setup the request immediately; a later connect/authentication failure is reported asynchronously by `OnConnectionError`. Do not use connection establishment as a synchronous readiness check.

## 3. Query callbacks and active result

```pawn
// Before: pBlueG R41-4
new query[128];
mysql_format(gSQL, query, sizeof(query),
    "SELECT name FROM players WHERE id = %d", playerid);
mysql_tquery(gSQL, query, "OnPlayerLoaded", "i", playerid);

forward OnPlayerLoaded(playerid);
public OnPlayerLoaded(playerid)
{
    new name[32];
    cache_get_value_name(0, "name", name);
    return 1;
}

// After: same user argument list, result is active inside callback
new query[128];
pdb_format(gDatabase, query, sizeof(query),
    "SELECT name FROM players WHERE id = %d", playerid);
pdb_query(gDatabase, query, "OnPlayerLoaded", "i", playerid);

forward OnPlayerLoaded(playerid);
public OnPlayerLoaded(playerid)
{
    new name[32];
    pdb_get_str("name", name, sizeof name, 0);
    return 1;
}
```

In compatibility mode, cache getters route to the active query result; common name/index overload forms are retained. `row = -1` selects the current row, or row zero before cursor advancement. A query's unretained result is released at callback exit. Use `pdb_retain_result`/`pdb_activate_result`/`pdb_free_result` if it must outlive the callback.

## 4. Error callback signature

Pawn.DB declares:

```pawn
forward OnQueryError(PDB:handle, error_id, const error_msg[], const query[]);
```

Legacy pBlueG and omp variants place error id/message/callback/query before the handle and include the callback name. Change the handler to the Pawn.DB signature and remove code that relies on a callback-name parameter:

```pawn
// Before (legacy shape)
public OnQueryError(errorid, error[], callback[], query[], MySQL:handle)

// After
public OnQueryError(PDB:handle, error_id, const error_msg[], const query[])
```

The Pawn.DB callback has no callback-name field. Avoid logging credentials or sensitive literals from SQL text.

## 5. Formatting differences

`mysql_format` maps to `pdb_format`; asynchronous aliases feed Pawn.DB's SQL formatter. Literal `%` must be escaped as `%%` in these format strings. `%e` escapes content but does not add quotes; use `%e` within SQL quotes for untrusted values and `%I` for identifiers; `%s` is raw insertion without escaping. `%j` accepts a builder handle or validated JSON.

```pawn
// Before: pBlueG formats SQL separately
new query[512];
mysql_format(gSQL, query, sizeof(query),
    "SELECT * FROM players WHERE name = '%e'", name);
mysql_tquery(gSQL, query, "Loaded");

// After: format SQL, then insert that trusted SQL without parsing it again
new query[512];
pdb_format(gDatabase, query, sizeof(query),
    "SELECT * FROM players WHERE name = '%e'", name);
pdb_query(gDatabase, "%s", "Loaded", "", query);
```

The formatter uses 4,096 bytes inline and spills to heap up to 65,536 bytes. Queries exceeding that limit are rejected, not submitted truncated. Literal percent examples such as `LIKE '%admin%'` become `LIKE '%%admin%%'` in the initial format string. Sending the resulting SQL through the trusted `%s` pass-through prevents a second parse of any percent signs, including percent signs in escaped user values. The callback specifier string remains empty in that example; the SQL argument is consumed by the formatter only.

## 6. Compatibility coverage

| Legacy API area | Status and mapping |
| --- | --- |
| pBlueG `mysql_tquery`, `mysql_pquery`; omp `mysql_execute` | Stock/macro async mapping to `pdb_query`; callback receives user args, result is implicit. |
| omp `mysql_execute_for` | Reviewed compatibility wrapper maps player/user args; verify the installed Pawn.DB include signature rather than assuming every upstream overload. |
| `mysql_format` | Alias to `pdb_format`; use the Pawn.DB specifiers and `%%` rule. |
| Cache getters, row/field counts, null checks | Stocks/macros use active result APIs. Supports common name/index getter overloads. No independent legacy cache object is created. |
| `cache_save` / `cache_delete` / active selection | Maps to retain, free, and activate result operations. Handle lifetime must be respected. |
| Insert id, affected rows, warning count, execution time | Maps to current active result metadata; execution time units are handled by the compatibility stock. |
| Argon2 hash/verify | Maps to asynchronous `pdb_hash` / `pdb_verify`. |
| Bcrypt, synchronous hash/verify | Deprecated stubs; return false/zero/empty outputs and do no work. |

## 7. Deprecated stubs and fallbacks

These names exist to help old scripts compile while producing a deprecation warning. They are not emulations and never execute the legacy behavior.

| Stub category | Representative APIs | Safe fallback |
| --- | --- | --- |
| Blocking query/file query | `mysql_query`, `mysql_query_file`, `mysql_execute_sync`, `mysql_execute_file_sync` | `INVALID_PDB_RESULT` |
| ORM/model operations | `orm_*`, `mysql_model_*` | Invalid handle/status or `0`; no query or persistence |
| Sync bcrypt/hash verification | `mysql_hash_sync`, `mysql_verify_sync` | `0`; output empty / `false` |
| Legacy escaping/error access | `mysql_escape_string`, `mysql_errno`, `mysql_error` | `0`; output empty |
| Cache introspection no longer modeled | `cache_is_any_active`, `cache_is_valid`, result count, field type, query string | `false`, `0`, invalid enum, or empty output |

`cache_set_result` is a separate deprecated traversal helper, not a no-op stub. It advances by a relative forward offset and returns `1` on success or `0` for a negative offset or failed traversal; it does not implement absolute/backward result-set selection. Use native forward traversal explicitly.

Do not remove the warning without migrating the call site. For escaping, use Pawn.DB format tokens; for ORM/model data, use SQL and `PDBJson:` builder as appropriate.

## 8. Manual migration checklist

- Move credentials and TLS material to protected configuration and verify filesystem permissions.
- Replace old include with the compatibility block above; resolve guard errors rather than suppressing them.
- Update `OnQueryError` order and remove its legacy callback-name argument.
- Verify query callback argument count and types; regular query callbacks do not receive a result handle.
- Double literal percent signs in formatted query strings.
- Replace every deprecated stub and test NULL, empty result, connection failure, and retry paths explicitly.
- Recompile and run integration tests against the actual server adapter and database driver.
