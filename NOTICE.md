# NOTICE — amalgame-database-postgresql

## Authorship

Copyright 2026 Bastien Mouget. Original work — see
`runtime/Amalgame_Database_PostgreSQL.h`.

Part of the Amalgame ecosystem
([github.com/amalgame-lang/Amalgame](https://github.com/amalgame-lang/Amalgame)).
External contributions are paused at the ecosystem level; see the
main repo's `CONTRIBUTING.md` for the policy.

AI tools (Anthropic Claude) were used during development. Per
the project's authorship policy, AI is treated as a tool, not a
co-author at law.

## Licence

Apache License 2.0. See `LICENSE` for the full text.

## Third-party content

**None vendored.** This package binds to libpq, the official C
client library that ships with PostgreSQL. libpq is provided by
the user's operating system at compile time (via
`libpq-dev` / `libpq-devel` / `postgresql-libs`) and at run time
(via `libpq5` / `libpq.so` / `libpq.dylib` / `libpq.dll`).

PostgreSQL itself is distributed under the
[PostgreSQL License](https://www.postgresql.org/about/licence/),
a BSD-style permissive licence (Apache-2.0 compatible).

> The PostgreSQL Global Development Group is the copyright holder
> for PostgreSQL.

This package does not include or redistribute any PostgreSQL or
libpq code; users obtain those independently from their OS package
manager or from postgresql.org. The binding header in
`runtime/Amalgame_Database_PostgreSQL.h` is original work that
declares the libpq function signatures (or rather, includes the
official `libpq-fe.h` header at compile time).

## Trademarks

"PostgreSQL" is a trademark of the PostgreSQL Community Association
of Canada. This repository uses the name solely to identify the
database the package binds to. No trademark claim is asserted.
