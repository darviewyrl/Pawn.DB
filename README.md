# Pawn.DB

[![Build](https://github.com/darviewyrl/Pawn.DB/actions/workflows/build.yml/badge.svg)](https://github.com/darviewyrl/Pawn.DB/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/darviewyrl/Pawn.DB)](https://github.com/darviewyrl/Pawn.DB/releases)

Pawn.DB is a C++20 asynchronous database plugin for Pawn servers. It supports MariaDB/MySQL and PostgreSQL and provides separate adapters for open.mp and legacy SA-MP.

- Documentation site source: [Mintlify docs repository](https://github.com/darviewyrl/pawndb.docs)
- Migration guide: [MIGRATION_GUIDE.md](MIGRATION_GUIDE.md)
- Releases: [GitHub Releases](https://github.com/darviewyrl/Pawn.DB/releases)

> The Mintlify project has no verified public site URL in the repository yet. Use the docs repository link until its deployment domain is confirmed.

## Technical highlights

- Database I/O and Argon2id work run on background workers; Pawn callbacks run on the server thread. Completion dispatch is budgeted to at most 64 callbacks or 5 ms per tick by default.
- Lock-free task/completion queues separate the game thread from worker execution.
- Callback-scoped active results are released automatically unless retained explicitly.
- Native JSON builder, bounded JSONPath extraction, and 3D/4D vector helpers are exposed to Pawn.
- The registered native table contains 75 `pdb_*` names. Optional legacy aliases are implemented in `pawndb.inc` and do not add native symbols.

## Quick start

Install a matching server binary and `include/pawndb.inc`; create `configs/database.json` and then:

```pawn
#include <a_samp>
#include <pawndb>

new PDB:gDatabase;

public OnGameModeInit()
{
    gDatabase = pdb_connect_file("configs/database.json");
    return gDatabase != INVALID_PDB;
}

public OnPlayerConnect(playerid)
{
    new query[128];
    if (!pdb_format(gDatabase, query, sizeof(query),
        "SELECT score FROM players WHERE id = %d", playerid)) return 0;
    pdb_query(gDatabase, query, "OnPlayerLoaded", "i", playerid);
    return 1;
}

forward OnPlayerLoaded(playerid);
public OnPlayerLoaded(playerid)
{
    if (pdb_num_rows() > 0)
        printf("score=%d", pdb_get_int("score"));
    return 1;
}
```

A connect handle may be pending. Handle later failure via `OnConnectionError`; query callbacks receive only the supplied user arguments and use the active result implicitly.

## Binary installation matrix

| Server adapter | Windows | Linux | Architecture |
| --- | --- | --- | --- |
| open.mp (`pawndb_omp`) | `.dll` | `.so` | x86 and x64 |
| SA-MP legacy (`pawndb_samp`) | `.dll` | `.so` | x86 only |

Download a matching artifact from [Releases](https://github.com/darviewyrl/Pawn.DB/releases). Asset names may differ between releases; choose for server, OS, and architecture. Copy `pawndb.inc` into the Pawn compiler include directory.

## Build from source

CMake 3.25+, a C++20 toolchain, recursive submodules, and platform build tools are required. CI scripts prepare pinned dependencies, configure/build with CMake and Ninja, run CTest, and check linkage.

Linux (Ubuntu or equivalent toolchain):

```sh
git clone --recursive https://github.com/darviewyrl/Pawn.DB.git
cd Pawn.DB
bash ci/build-linux.sh x64 omp
```

Windows, from Visual Studio 2022 Developer PowerShell with Perl, Ninja, NASM, Meson, and pkg-config available:

```powershell
.\ci\build-windows.ps1 -Arch x64 -Adapter omp
```

Select `x86` for 32-bit; use `-Adapter samp` only with x86. Linux x86 builds require multilib compiler and system development libraries. The scripts use `build/ci`, so clean that directory before switching architecture. See [full source-build notes](https://github.com/darviewyrl/pawndb.docs/blob/main/guides/building-from-source.mdx).

## Compatibility

Selected pBlueG R41-4 and omp-mysql APIs can be enabled with `#define PAWNDB_COMPAT` before `<pawndb>`. This is a source migration aid, not a full behavioral replacement. Deprecated synchronous, ORM, and legacy bcrypt APIs compile as warnings and return fallback values without doing work. Review the [migration guide](MIGRATION_GUIDE.md).
