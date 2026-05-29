/*
 * Amalgame Standard Library — Amalgame.Database.PostgreSQL
 * Copyright (c) 2026 Bastien MOUGET
 * https://github.com/amalgame-lang/Amalgame
 *
 * PostgreSQL binding — dynamic-linked to the system libpq (the
 * official C client library that ships with PostgreSQL). No
 * vendored implementation; the user binary links against the OS-
 * provided libpq.so / libpq.dylib / libpq.dll.
 *
 * Surface (v1):
 *   Open(connStr) / Close / IsOpen / LastError    lifecycle + diag
 *   Exec(sql)                                      DDL / INSERT / UPDATE / DELETE
 *   QueryAll(sql) -> List<List<string>>            SELECT all rows × cols
 *   Changes()                                      rows affected by last Exec
 *   ServerVersion()                                postgres major.minor as string
 *
 * Connection string follows libpq's keyword=value syntax (also
 * accepts a postgresql:// URI):
 *
 *   "host=localhost port=5432 dbname=mydb user=app password=s3cret"
 *   "postgresql://app:s3cret@localhost:5432/mydb"
 *   "postgresql:///mydb"  (Unix socket, peer auth)
 *
 * Threading: AmalgamePostgreSQL* is single-owner. Concurrent
 * Exec / QueryAll calls against the same handle from different
 * threads will interleave wire bytes — libpq is *not* internally
 * thread-safe for shared connections. Distinct handles per thread
 * are fine. Async / pipelined query support lives in v2.
 *
 * Memory: PGresult* values returned by libpq are PQclear()'d as
 * soon as we've copied the data we need into AmalgameList* /
 * code_string. PGconn* lives for the lifetime of the
 * AmalgamePostgreSQL* handle and is PQfinish()'d in Close().
 * GC finalizer registered so a leaked handle still releases the
 * connection eventually.
 *
 * Out of scope (v2):
 *   - PQexecParams parameter binding ($1, $2)
 *   - PQprepare / PQexecPrepared statement reuse
 *   - Typed column accessors (the current API stringifies every
 *     cell via PQgetvalue, which is libpq's text format)
 *   - COPY-protocol bulk insert
 *   - Async query (PQsendQuery + PQisBusy + PQconsumeInput)
 *   - LISTEN / NOTIFY pub-sub
 *   - Cursor-based streaming for huge result sets
 */

#ifndef AMALGAME_DATABASE_POSTGRESQL_H
#define AMALGAME_DATABASE_POSTGRESQL_H

#include "_runtime.h"
#include "Amalgame_Collections.h"

/* Resolve libpq-fe.h across the two common system layouts.
 * Debian / Ubuntu / Arch ship it under /usr/include/postgresql/.
 * Fedora / RHEL ship it under /usr/include/ directly.
 * Homebrew / MSYS2 may use either depending on the install. */
#if defined(__has_include)
#  if __has_include(<libpq-fe.h>)
#    include <libpq-fe.h>
#  elif __has_include(<postgresql/libpq-fe.h>)
#    include <postgresql/libpq-fe.h>
#  else
#    error "libpq-fe.h not found. Install libpq-dev / libpq-devel / postgresql-libs."
#  endif
#else
#  /* Old compiler without __has_include — assume Debian layout. */
#  include <postgresql/libpq-fe.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AmalgamePostgreSQL {
    PGconn*  conn;         /* the libpq connection; NULL when closed */
    char*    last_error;   /* GC-strdup'd, or NULL */
    i64      last_changes; /* rows affected by the last Exec */
} AmalgamePostgreSQL;

/* ── Helpers ────────────────────────────────────────── */

static inline code_string _ampg_err_dup(const char* msg) {
    if (!msg) return NULL;
    size_t n = strlen(msg);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, msg, n + 1);
    return p;
}

/* Strip a trailing newline from a libpq error message — PQerrorMessage
 * always ends with '\n', which looks weird when surfaced to user
 * code that builds messages with " + " concatenation. */
static inline code_string _ampg_err_from_conn(PGconn* c) {
    if (!c) return _ampg_err_dup("null connection");
    const char* raw = PQerrorMessage(c);
    if (!raw || !*raw) return _ampg_err_dup("");
    size_t n = strlen(raw);
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) n--;
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, raw, n);
    p[n] = '\0';
    return p;
}

/* Same for a result's error. */
static inline code_string _ampg_err_from_result(PGresult* r) {
    if (!r) return _ampg_err_dup("null result");
    const char* raw = PQresultErrorMessage(r);
    if (!raw || !*raw) return _ampg_err_dup("");
    size_t n = strlen(raw);
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) n--;
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, raw, n);
    p[n] = '\0';
    return p;
}

/* GC finalizer — runs PQfinish if the user dropped the handle
 * without calling Close. */
static void _ampg_finalize(void* obj, void* cd) {
    (void) cd;
    AmalgamePostgreSQL* db = (AmalgamePostgreSQL*) obj;
    if (db && db->conn) {
        PQfinish(db->conn);
        db->conn = NULL;
    }
}

static inline AmalgamePostgreSQL* _ampg_alloc(void) {
    AmalgamePostgreSQL* db =
        (AmalgamePostgreSQL*) GC_MALLOC(sizeof(AmalgamePostgreSQL));
    db->conn         = NULL;
    db->last_error   = NULL;
    db->last_changes = 0;
    GC_register_finalizer(db, _ampg_finalize, NULL, NULL, NULL);
    return db;
}

/* ── Lifecycle ──────────────────────────────────────── */

static inline AmalgamePostgreSQL* Amalgame_Database_PostgreSQL_Open(code_string connStr) {
    AmalgamePostgreSQL* db = _ampg_alloc();
    if (!connStr) {
        db->last_error = _ampg_err_dup("null connection string");
        return db;
    }
    PGconn* c = PQconnectdb(connStr);
    if (!c) {
        db->last_error = _ampg_err_dup("PQconnectdb returned NULL");
        return db;
    }
    if (PQstatus(c) != CONNECTION_OK) {
        db->last_error = _ampg_err_from_conn(c);
        PQfinish(c);
        return db;
    }
    db->conn = c;
    return db;
}

static inline void Amalgame_Database_PostgreSQL_Close(AmalgamePostgreSQL* db) {
    if (!db || !db->conn) return;
    PQfinish(db->conn);
    db->conn = NULL;
}

static inline code_bool Amalgame_Database_PostgreSQL_IsOpen(AmalgamePostgreSQL* db) {
    return (db && db->conn && PQstatus(db->conn) == CONNECTION_OK) ? 1 : 0;
}

static inline code_string Amalgame_Database_PostgreSQL_LastError(AmalgamePostgreSQL* db) {
    if (!db || !db->last_error) return (code_string) "";
    return db->last_error;
}

/* ── Exec ───────────────────────────────────────────── */

/* Run a no-result SQL statement (DDL / INSERT / UPDATE / DELETE).
 * Returns true on PGRES_COMMAND_OK or PGRES_TUPLES_OK (the latter
 * lets `RETURNING id`-style statements succeed even though we
 * discard the returned rows here). On failure, LastError carries
 * the postgres error message. Updates Changes() with the number
 * of affected rows. */
static inline code_bool Amalgame_Database_PostgreSQL_Exec(
        AmalgamePostgreSQL* db, code_string sql) {
    if (!db || !db->conn) {
        if (db) db->last_error = _ampg_err_dup("connection not open");
        return 0;
    }
    if (!sql) {
        db->last_error = _ampg_err_dup("null sql");
        return 0;
    }
    PGresult* r = PQexec(db->conn, sql);
    if (!r) {
        db->last_error = _ampg_err_from_conn(db->conn);
        return 0;
    }
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        db->last_error = _ampg_err_from_result(r);
        PQclear(r);
        return 0;
    }
    const char* tuples = PQcmdTuples(r);
    db->last_changes = (tuples && *tuples) ? (i64) strtoll(tuples, NULL, 10) : 0;
    PQclear(r);
    db->last_error = _ampg_err_dup("");
    return 1;
}

/* ── QueryAll ───────────────────────────────────────── */

/* SELECT and return every row as a List<List<string>>. Each inner
 * list holds the row's columns stringified (libpq's text mode).
 * NULL cells materialise as the empty string — callers that need
 * to distinguish NULL from "" should use PQexecParams in v2.
 *
 * On error the outer list is empty and LastError is set. */
static inline AmalgameList* Amalgame_Database_PostgreSQL_QueryAll(
        AmalgamePostgreSQL* db, code_string sql) {
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->conn) {
        if (db) db->last_error = _ampg_err_dup("connection not open");
        return rows;
    }
    if (!sql) {
        db->last_error = _ampg_err_dup("null sql");
        return rows;
    }
    PGresult* r = PQexec(db->conn, sql);
    if (!r) {
        db->last_error = _ampg_err_from_conn(db->conn);
        return rows;
    }
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK) {
        db->last_error = _ampg_err_from_result(r);
        PQclear(r);
        return rows;
    }

    int nrows = PQntuples(r);
    int ncols = PQnfields(r);
    for (int i = 0; i < nrows; i++) {
        AmalgameList* row = AmalgameList_new();
        for (int j = 0; j < ncols; j++) {
            const char* v = PQgetvalue(r, i, j);
            size_t n = v ? strlen(v) : 0;
            char* dup = (char*) code_alloc(n + 1);
            if (n > 0) memcpy(dup, v, n);
            dup[n] = '\0';
            AmalgameList_add(row, (void*) dup);
        }
        AmalgameList_add(rows, (void*) row);
    }
    db->last_changes = (i64) nrows;
    PQclear(r);
    db->last_error = _ampg_err_dup("");
    return rows;
}

/* Rows affected by the last Exec, or row count of the last QueryAll. */
static inline i64 Amalgame_Database_PostgreSQL_Changes(AmalgamePostgreSQL* db) {
    return db ? db->last_changes : 0;
}

/* ────────────────────────────────────────────────────────────────
 * v0.3 surface — parameter binding + transactions.
 *
 * **Placeholders differ from SQLite/DuckDB**: PostgreSQL uses
 * `$1, $2, ...` (1-indexed), not `?`. Pass them verbatim in the SQL
 * string. The Amalgame `List<string>` binding side stays identical
 * across backends — element 0 maps to `$1`, element 1 to `$2`, etc.
 *
 * Every value goes through `PQexecParams` in text mode, so libpq
 * does the same client-side escape work PostgreSQL would do for
 * a server-side prepared statement. NULL list entries become
 * SQL NULL.
 *
 * Arity mismatches surface as libpq's native error ("bind message
 * supplies N parameters, but prepared statement requires M") via
 * the standard LastError path — we don't pre-check param count
 * (would require PQprepare + PQdescribePrepared round-trips per
 * call; the SQL-level error is good enough).
 * ──────────────────────────────────────────────────────────────── */

/* Build the const char* paramValues array on the C stack for small
 * param sets, falling back to heap alloc for larger ones. Returns a
 * caller-owned heap buffer when n > stackCap; caller free()s. The
 * stack buffer is used for n <= stackCap (no free needed). */
static inline const char** _ampg_build_param_values(
    AmalgameList* params, int n, const char** stackBuf, int stackCap)
{
    const char** out = (n <= stackCap) ? stackBuf
                                       : (const char**) malloc((size_t)n * sizeof(char*));
    for (int i = 0; i < n; i++) {
        out[i] = (const char*) AmalgameList_get(params, i);   /* NULL → SQL NULL */
    }
    return out;
}

static inline code_bool Amalgame_Database_PostgreSQL_ExecBind(
        AmalgamePostgreSQL* db, code_string sql, AmalgameList* params)
{
    if (!db || !db->conn) {
        if (db) db->last_error = _ampg_err_dup("connection not open");
        return 0;
    }
    if (!sql) {
        db->last_error = _ampg_err_dup("null sql");
        return 0;
    }
    int n = params ? (int) AmalgameList_size(params) : 0;
    const char* stack[16];
    const char** values = _ampg_build_param_values(params, n, stack, 16);
    PGresult* r = PQexecParams(db->conn, sql, n, NULL, values, NULL, NULL, 0);
    if (values != stack) free((void*) values);
    if (!r) {
        db->last_error = _ampg_err_from_conn(db->conn);
        return 0;
    }
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        db->last_error = _ampg_err_from_result(r);
        PQclear(r);
        return 0;
    }
    const char* tuples = PQcmdTuples(r);
    db->last_changes = (tuples && *tuples) ? (i64) strtoll(tuples, NULL, 10) : 0;
    PQclear(r);
    db->last_error = _ampg_err_dup("");
    return 1;
}

static inline AmalgameList* Amalgame_Database_PostgreSQL_QueryBindAll(
        AmalgamePostgreSQL* db, code_string sql, AmalgameList* params)
{
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->conn) {
        if (db) db->last_error = _ampg_err_dup("connection not open");
        return rows;
    }
    if (!sql) {
        db->last_error = _ampg_err_dup("null sql");
        return rows;
    }
    int n = params ? (int) AmalgameList_size(params) : 0;
    const char* stack[16];
    const char** values = _ampg_build_param_values(params, n, stack, 16);
    PGresult* r = PQexecParams(db->conn, sql, n, NULL, values, NULL, NULL, 0);
    if (values != stack) free((void*) values);
    if (!r) {
        db->last_error = _ampg_err_from_conn(db->conn);
        return rows;
    }
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK) {
        db->last_error = _ampg_err_from_result(r);
        PQclear(r);
        return rows;
    }
    int nrows = PQntuples(r);
    int ncols = PQnfields(r);
    for (int i = 0; i < nrows; i++) {
        AmalgameList* row = AmalgameList_new();
        for (int j = 0; j < ncols; j++) {
            const char* v = PQgetvalue(r, i, j);
            size_t nn = v ? strlen(v) : 0;
            char* dup = (char*) code_alloc(nn + 1);
            if (nn > 0) memcpy(dup, v, nn);
            dup[nn] = '\0';
            AmalgameList_add(row, (void*) dup);
        }
        AmalgameList_add(rows, (void*) row);
    }
    db->last_changes = (i64) nrows;
    PQclear(r);
    db->last_error = _ampg_err_dup("");
    return rows;
}

static inline code_bool Amalgame_Database_PostgreSQL_Begin(AmalgamePostgreSQL* db) {
    return Amalgame_Database_PostgreSQL_Exec(db, "BEGIN");
}

static inline code_bool Amalgame_Database_PostgreSQL_Commit(AmalgamePostgreSQL* db) {
    return Amalgame_Database_PostgreSQL_Exec(db, "COMMIT");
}

static inline code_bool Amalgame_Database_PostgreSQL_Rollback(AmalgamePostgreSQL* db) {
    return Amalgame_Database_PostgreSQL_Exec(db, "ROLLBACK");
}

/* PostgreSQL server version as a "MAJOR.MINOR" string ("16.2",
 * "14.10", …). Empty when the connection is closed. */
static inline code_string Amalgame_Database_PostgreSQL_ServerVersion(AmalgamePostgreSQL* db) {
    if (!db || !db->conn) return (code_string) "";
    int v = PQserverVersion(db->conn);   /* 16 0002 = 16.2 → 160002 */
    if (v <= 0) return (code_string) "";
    /* libpq packs as MAJOR * 10000 + MINOR (since pg 10);
     * prior to pg 10 it was MAJOR * 10000 + MINOR * 100 + PATCH. */
    int major = v / 10000;
    int minor = v % 10000;
    if (major < 10) {
        /* Old-style packing: MAJOR.MINOR.PATCH. */
        minor = (v / 100) % 100;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d", major, minor);
    size_t n = strlen(buf);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, buf, n + 1);
    return p;
}

#endif /* AMALGAME_DATABASE_POSTGRESQL_H */
