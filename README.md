# amalgame-database-postgresql

PostgreSQL binding for [Amalgame](https://github.com/amalgame-lang/Amalgame).
Dynamic-linked to the system libpq — **first amalgame-lang
package to exercise the dynamic-link pattern**. Nothing is
vendored; the user binary links against the OS-provided
`libpq.so` / `libpq.dylib` / `libpq.dll`.

## Why dynamic-link instead of vendor (like SQLite / DuckDB)

| Trade-off | Vendor | Dynamic-link |
|---|---|---|
| Install steps for end user | None | 1 system command |
| Binary size | +600 KB (libpq is heavy) | minimal |
| Tracks server protocol version | No (snapshot drifts) | Yes (matches OS libpq) |
| Bootstrap from a clean OS | Yes | Needs libpq-dev |
| Single binary deploy | Yes | Needs libpq runtime on target |

libpq is universally packaged so the install cost is one command;
deploying a Postgres-using app already assumes the server is
reachable, so libpq presence on the deploy host is also a given.

## Prerequisites

Install the libpq development package on your build machine:

| OS / distro | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install libpq-dev` |
| Fedora / RHEL / Rocky | `sudo dnf install libpq-devel` |
| Arch / Manjaro | `sudo pacman -S postgresql-libs` |
| Alpine | `apk add libpq-dev` |
| macOS (Homebrew) | `brew install libpq` (may need `brew link --force libpq`) |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-postgresql` |
| Windows (vanilla) | install PostgreSQL from postgresql.org |

On the **deploy** machine you need the runtime variant: `libpq5`
on Debian/Ubuntu, `libpq` on Fedora, etc. Usually pulled in as a
dependency of the Postgres server itself, or you can install just
the client.

## Install

```bash
amc package add postgresql                                                # via index
amc package add github.com/amalgame-lang/amalgame-database-postgresql@v0.2.0
```

Requires **amc 0.8.40+** (for `returns_generic` on `QueryAll`).

## Surface

```amalgame
import Amalgame.Database.PostgreSQL

let db = PostgreSQL.Open("host=localhost dbname=mydb user=app password=s3cret")
if (!PostgreSQL.IsOpen(db)) {
    Console.WriteLine("connect failed: " + PostgreSQL.LastError(db))
    return
}

PostgreSQL.Exec(db, "CREATE TABLE IF NOT EXISTS notes (id SERIAL, body TEXT)")
PostgreSQL.Exec(db, "INSERT INTO notes (body) VALUES ('hello postgres')")
Console.WriteLine("inserted " + String_FromInt(PostgreSQL.Changes(db)) + " row")

// Since v0.2.0 the manifest declares `returns_generic =
// "List<List<string>>"` on QueryAll, so amc infers every cell as
// `string` without any explicit annotations on `row` / `Get(j)`.
let rows = PostgreSQL.QueryAll(db, "SELECT id, body FROM notes ORDER BY id")
for row in rows {
    Console.WriteLine(row.Get(0) + ": " + row.Get(1))
}

PostgreSQL.Close(db)
```

### v0.2.0 method surface

| Method | Returns | Notes |
|---|---|---|
| `PostgreSQL.Open(connStr)` | `AmalgamePostgreSQL*` | libpq keyword=value or `postgresql://` URI |
| `PostgreSQL.Close(db)` | `void` | Idempotent; GC also frees the conn |
| `PostgreSQL.IsOpen(db)` | `bool` | Live connection check |
| `PostgreSQL.LastError(db)` | `string` | Empty on success; libpq's trailing `\n` stripped |
| `PostgreSQL.Exec(db, sql)` | `bool` | DDL / INSERT / UPDATE / DELETE; updates Changes() |
| `PostgreSQL.QueryAll(db, sql)` | `List<List<string>>` | SELECT all rows × cols (text mode) |
| `PostgreSQL.Changes(db)` | `int` | Rows affected by last Exec / row count of last QueryAll |
| `PostgreSQL.ServerVersion(db)` | `string` | "16.2", "14.10", … |

### Connection string

libpq accepts two equivalent forms:

```
"host=localhost port=5432 dbname=mydb user=app password=s3cret"
"postgresql://app:s3cret@localhost:5432/mydb"
"postgresql:///mydb"                            # Unix socket, peer auth
"host=/var/run/postgresql dbname=mydb"          # explicit Unix socket
```

Sensitive credentials should come from env vars (`PGHOST`,
`PGUSER`, `PGPASSWORD`, …) — libpq reads them automatically when
the conn string omits the matching keyword. See
[libpq's environment variables](https://www.postgresql.org/docs/current/libpq-envars.html).

## Pixel layout / data model

The v1 surface stringifies every cell via `PQgetvalue` (libpq's
text mode). NULL cells materialise as the empty string — callers
that need to distinguish `NULL` from `''` should use parameter
binding + typed accessors in v2.

## Deferred to v2

- Parameter binding via `PQexecParams` (`$1`, `$2`)
- Prepared statement reuse (`PQprepare` + `PQexecPrepared`)
- Typed column accessors (`AsInt(col)`, `AsBytes(col)`, `AsTimestamp(col)`)
- COPY-protocol bulk insert / bulk export
- Async query mode (`PQsendQuery` + `PQisBusy` + `PQconsumeInput`)
- `LISTEN` / `NOTIFY` pub-sub (`PQnotifies`)
- Cursor-based streaming for large result sets
- SSL/TLS-specific controls (libpq already handles TLS via the
  conn string automatically — `sslmode=require` etc.)

## Threading

`AmalgamePostgreSQL*` is single-owner. Concurrent `Exec` /
`QueryAll` calls against the same handle from different threads
will interleave wire bytes — libpq is **not** internally
thread-safe for shared connections. Use distinct handles per
thread. Async / pipelined query lands in v2.

## Tests

```bash
./tests/run_tests.sh /path/to/amc
```

The runner gates on **two** things:
1. libpq-dev is installed (header probe via `__has_include`)
2. A PostgreSQL server is reachable on `127.0.0.1:5432`

If either is missing, every case SKIPs cleanly. Start a server
locally with:

```bash
docker run --rm -p 5432:5432 \
  -e POSTGRES_PASSWORD=test -e POSTGRES_DB=amctest \
  postgres:16-alpine
```

then export `PGHOST=127.0.0.1 PGUSER=postgres PGPASSWORD=test PGDATABASE=amctest`
before running the suite.

## Licence

Apache-2.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).
PostgreSQL itself is under the PostgreSQL Licence (BSD-style),
compatible with Apache-2.0 redistribution.
