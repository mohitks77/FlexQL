# FlexQL — Design Document

> **GitHub repository:** `https://github.com/mohitks77/FlexQL`

---

## 1. System Architecture

FlexQL is a client-server SQL-like database written entirely in **C++17**. The system consists of two independently compiled binaries connected over TCP, plus a C API library that third-party programs can link against.

```
┌──────────────────────────────┐         TCP (framed)           ┌─────────────────────────────────────┐
│         CLIENT               │  ──── QUERY frame ──────────►  │             SERVER                  │
│  flexql.h   (public C API)   │  ◄─── RESULT_ROW frames ────   │  Parser → Executor → Storage        │
│  flexql_client.cpp           │  ◄─── DONE / ERROR frame ───   │  Catalog (all DBs)                  │
│  repl.cpp   (REPL terminal)  │                                 │  select() multi-client event loop   │
└──────────────────────────────┘                                 └──────────────────────────────────────┘
                                                                              │
                                                                    data/<dbname>/<table>.sch
                                                                    data/<dbname>/<table>.dat
                                                                    (persistent disk files)
```

### Wire Protocol

Every message is a **length-prefixed frame**:

| Bytes | Field                                                  |
| ----- | ------------------------------------------------------ |
| 1     | `MsgType` enum: QUERY / RESULT_ROW / OK / ERROR / DONE |
| 4     | Payload length (big-endian `uint32_t`)                 |
| N     | Payload (UTF-8 text)                                   |

**Flow for a SELECT query:**

1. Client sends one `QUERY` frame containing the SQL string.
2. Server sends zero or more `RESULT_ROW` frames (one per result row).
3. Server sends a single `DONE` frame: `"OK|<elapsed_ms>ms|<n> rows"`.

**Flow for DDL/DML (no rows):**

1. Client sends `QUERY`.
2. Server sends `DONE` immediately.
3. On error: server sends `ERROR` frame with a human-readable message.

`RESULT_ROW` payload format:

```
<num_cols>\t<col0_name>\t<col1_name>...\n<val0>\t<val1>...\n
```

---

## 2. Database & Table Management (MySQL-style)

FlexQL supports MySQL-style multi-database management:

```sql
CREATE DATABASE school;
USE school;              -- sets session's active database
SHOW DATABASES;
SHOW TABLES;
DROP DATABASE school;
```

Each database maps to a subdirectory under `data/`. Tables within a database are files inside that directory. The server maintains a **`Catalog`** object (one per process) holding all loaded databases. Each client connection carries its own **`Session`** struct tracking `current_db`.

---

## 3. Storage Design

### Layout: Row-Major (Append-Only Data File)

**Rationale:** This is an OLTP-style workload — inserts and full-row retrievals dominate. Row-major storage keeps all fields of one record contiguous in memory, which is optimal for `SELECT *` and single-row lookups. Column-major storage benefits analytical aggregations over single columns (e.g. SUM), which are not required here.

### File Format

Each table is stored as three files:

| File          | Contents                                                      |
| ------------- | ------------------------------------------------------------- |
| `<table>.sch` | Tab-separated schema: `col_name\ttype\tnot_null\tprimary_key` |
| `<table>.dat` | Binary row records (append-only)                              |

**Row record format** (21-byte fixed header + variable payload):

```
[8 bytes]  uint64_t  row_id
[8 bytes]  int64_t   expires    (unix timestamp; 0 = never)
[1 byte]   uint8_t   deleted    (tombstone flag)
[4 bytes]  uint32_t  payload_len
[N bytes]  char[]    payload    (tab-separated column values)
```

**Append-only writes:** Every INSERT appends a new record. Deletes set the `deleted` flag in memory (soft delete). The file is append-only for maximum write throughput — no in-place updates to the data file. A background compaction pass (future work) would rewrite the file without tombstoned rows.

### In-Memory Representation

On server startup, the entire data file is loaded into `std::vector<Row>` in memory. All queries run against this in-memory representation. This gives:

- O(1) indexed access
- Zero disk I/O during queries
- Full 10M row support (10M rows × ~100 bytes avg ≈ 1 GB RAM)

---

## 4. Indexing

### Primary Key: Dual-Index (Hash + B-Tree)

Each table with a `PRIMARY KEY` column maintains two in-memory indexes:

| Index      | Type                                 | Purpose                       |
| ---------- | ------------------------------------ | ----------------------------- |
| `pk_hash`  | `std::unordered_map<string, size_t>` | O(1) average point lookup     |
| `pk_index` | `std::map<string, size_t>`           | O(log N) ordered / range scan |

**Fast path in executor:** A `SELECT … WHERE pk_col = value` query hits `pk_hash` directly and returns in O(1) without scanning any rows. This is the critical path for 10M-row point lookups.

**Why both structures?**

- The hash map gives the fastest point lookups for equality queries (the dominant pattern).
- The ordered map enables future range queries (`WHERE id BETWEEN 100 AND 200`) and sorted iteration without a full scan + sort.

---

## 5. Expiration Timestamps

Every row stores a `time_t expires` field:

- If `INSERT` includes an explicit `EXPIRES <unix_timestamp>` clause, that value is used.
- Otherwise the row is assigned `now + 3600` seconds (**1-hour default TTL**).
- `expires = 0` means "never expires" (not used by default but supported).

The helper `Table::alive(row)` checks `time(nullptr) < row.expires && !row.deleted` and is called during every scan and index lookup. Expired rows are **logically deleted on read** — they never appear in query results. Physical removal from the data file happens on the next compaction pass.

---

## 6. Supported SQL Commands

| Command                                                 | Notes                                    |
| ------------------------------------------------------- | ---------------------------------------- |
| `CREATE DATABASE n`                                     | Creates `data/n/` directory              |
| `USE n`                                                 | Sets session's active database           |
| `SHOW DATABASES`                                        | Lists all databases                      |
| `SHOW TABLES`                                           | Lists tables in current database         |
| `DROP DATABASE n`                                       | Removes database and all its files       |
| `CREATE TABLE t (col TYPE [PRIMARY KEY] [NOT NULL], …)` | Types: INT, DECIMAL, VARCHAR, DATETIME   |
| `DROP TABLE t`                                          | Soft-deletes all rows (schema preserved) |
| `DESCRIBE t`                                            | Shows column names, types, nullability   |
| `INSERT INTO t VALUES (…)`                              | With optional `EXPIRES <ts>`             |
| `INSERT INTO t (c1,c2) VALUES (…)`                      | Named-column insert                      |
| `SELECT * FROM t`                                       | Full scan                                |
| `SELECT c1,c2 FROM t`                                   | Projected scan                           |
| `SELECT … WHERE col op val`                             | Filter; ops: `=` `!=` `<` `>` `<=` `>=`  |
| `SELECT … INNER JOIN t2 ON t.c = t2.c`                  | Cross-join + ON filter                   |
| `SELECT … ORDER BY col [ASC\|DESC]`                     | Sorted results                           |
| `SELECT … LIMIT n`                                      | Limit result count                       |
| `UPDATE t SET col=val [WHERE …]`                        | In-memory update                         |
| `DELETE FROM t [WHERE …]`                               | Soft delete (tombstone)                  |

### DATETIME Support

Accepted input formats:

- `YYYY-MM-DD HH:MM:SS` (with or without quotes)
- `YYYY-MM-DDTHH:MM:SS`
- `YYYY-MM-DD` (date only, time defaults to 00:00:00)
- `NOW()` / `CURRENT_TIMESTAMP`
- Unix integer timestamp

Stored and displayed as `YYYY-MM-DD HH:MM:SS`. DATETIME columns support all comparison operators (`<`, `>`, `<=`, `>=`, `=`, `!=`) with correct temporal semantics.

---

## 7. Caching Strategy

The LRU cache from v1 was removed in v2. **Rationale:** With the full dataset in memory, query latency is already in the sub-millisecond range for indexed lookups and low milliseconds for full scans. Adding a result cache on top would add memory pressure and cache-invalidation complexity without meaningful benefit for the OLTP workload. The primary performance lever is the hash-map index, not result caching.

---

## 8. Concurrency Design

The server uses a **single-threaded `select()` event loop** that handles multiple simultaneous client connections:

```
while running:
    select() on [listen_fd, client_fd_1, client_fd_2, ...]
    if new connection → accept, add to client list, create Session
    if client readable → handle_request(fd, exec, session[fd])
```

One request is fully processed before the next begins. This eliminates all data-race conditions on shared in-memory structures without any locking overhead.

**Per-table mutexes** are present in the `Table` struct so the system can be upgraded to a thread-per-client model in the future without restructuring the storage layer.

---

## 9. Query Execution Timing

Every query response includes a precise execution time measured with `std::chrono::high_resolution_clock`. The timing covers parse → execute → result assembly, and is returned to the client in the `DONE` frame payload. The REPL displays it after every query:

```
flexql [SCHOOL]> SELECT * FROM STUDENT WHERE GPA > 8.0;
NAME = Alice   GPA = 9.1
NAME = Carol   GPA = 8.5

Query OK  (0.008ms | 2 rows)
```

---

## 10. Compilation & Execution

### Requirements

- g++ ≥ 9 with C++17 (`-std=c++17`)
- POSIX system (Linux / macOS)
- No external libraries required

### Build

```bash
make          # builds bin/flexql-server and bin/flexql-client
```

If on an older GCC that needs explicit filesystem linking:

```bash
make LDFLAGS="-lstdc++fs"
```

### Run

```bash
# Terminal 1 — start server (data stored in ./data/)
./bin/flexql-server 9000

# Terminal 2 — interactive REPL
./bin/flexql-client 127.0.0.1 9000

flexql> CREATE DATABASE mydb;
flexql> USE mydb;
flexql [MYDB]> CREATE TABLE USERS(ID INT PRIMARY KEY NOT NULL, NAME VARCHAR(50) NOT NULL, CREATED DATETIME NOT NULL);
flexql [MYDB]> INSERT INTO USERS VALUES(1, Alice, 2024-06-01 09:00:00);
flexql [MYDB]> SELECT * FROM USERS;
ID = 1
NAME = Alice
CREATED = 2024-06-01 09:00:00

Query OK  (0.005ms | 1 row)
```

### Run Benchmark

```bash
g++ -O3 ./scripts/benchmark_flexql.cpp src/client/flexql_client.cpp src/network/network.cpp -I./include -o bench

# With server running on port 9000:
# # 10M rows (if not specified then benchmark runs with 1M rows and unit tests)
./bench 10000000
```

---

## 11. Performance Results

Run `python3 scripts/benchmark.py 127.0.0.1 9000 1000000` and fill in:

| Metric                   | Result        |
| ------------------------ | ------------- |
| INSERT throughput        | \_\_\_ rows/s |
| SELECT \* (all rows)     | \_\_\_s       |
| PK point-lookup avg      | \_\_\_ms      |
| PK point-lookup p99      | \_\_\_ms      |
| WHERE scan (non-indexed) | \_\_\_s       |

> Fill these in after running the benchmark on your machine.

---

## 12. Project Structure

```
flexql/
├── include/
│   ├── flexql.h        Public C API (flexql_open/exec/close/free)
│   ├── common.h        Wire protocol constants and MsgType enum
│   ├── network.h       Framed TCP I/O interface
│   ├── storage.h       Row, Table, Database, Catalog types
│   ├── parser.h        SQL AST node types and Parser class
│   └── executor.h      Executor class and ExecResult/Session types
├── src/
│   ├── network/        network.cpp   — framed I/O implementation
│   ├── storage/        storage.cpp   — disk persistence, index management
│   ├── parser/         parser.cpp    — tokeniser + AST builder
│   ├── query/          executor.cpp  — all SQL command handlers
│   ├── server/         server.cpp    — select()-loop TCP server
│   └── client/
│       ├── flexql_client.cpp         — public C API implementation
│       └── repl.cpp                  — interactive REPL terminal
├── scripts/
│   └── benchmark.py   Pipelined 10M-row performance benchmark
├── data/              Created at runtime — one subdir per database
├── Makefile
└── DESIGN.md          This document
```

---

## 13. Caching — LRU Result Cache (Added in v2.1)

### Design

The server supports an optional **LRU (Least-Recently-Used) query result cache**, enabled with `--cache`:

```bash
./bin/flexql-server 9000 --cache          # default 2048 entries
./bin/flexql-server 9000 --cache=8192     # custom capacity
```

**Key:** Normalised SQL string + table name (uppercased).  
**Value:** Full serialised result set (`col_names` + `rows` vectors).  
**Invalidation:** Generation-counter based. Every table has a `uint64_t` generation counter. On INSERT, UPDATE, DELETE, or DROP, the counter is incremented. A cached entry is considered stale if its stored generation doesn't match the current table generation — it is evicted on first access. No background sweep needed.

**Data structure:** `std::list<pair<key,CacheEntry>>` (front = MRU) + `std::unordered_map<key, list::iterator>` for O(1) lookup and O(1) LRU eviction.

### Benchmark Results (20,000 rows)

| Scenario                                | No-Cache | With LRU Cache | Speedup                      |
| --------------------------------------- | -------- | -------------- | ---------------------------- |
| Cold SELECT \* (first hit)              | 7.663ms  | 1.407ms        | 5.4× faster                  |
| Warm SELECT \* (20 repeats avg)         | 11.983ms | 1.397ms        | **8.6× faster**              |
| PK point-lookup WHERE (20 repeats)      | 0.004ms  | 0.005ms        | ~same (already O(1))         |
| WHERE scan non-indexed (20 repeats)     | 2.916ms  | 0.488ms        | **6.0× faster**              |
| Post-INSERT re-scan (cache invalidated) | 2.986ms  | 3.548ms        | −19% (invalidation overhead) |
| ORDER BY + LIMIT 10 (10 repeats avg)    | 63.894ms | 6.648ms        | **9.6× faster**              |
| Mixed write+read workload (10 cycles)   | 87.433ms | 41.224ms       | 2.1× faster                  |

### Analysis

**Cache wins:**

- Any repeated read query benefits enormously — 6×–9× speedup on full scans and ORDER BY, because the entire result set is served from memory without re-scanning rows.
- ORDER BY in particular benefits because sorting is O(N log N) — skipping it on cache hits is a massive win.

**Cache is neutral or slightly worse:**

- **PK point-lookups** are already O(1) via the hash index — the cache adds a small lookup overhead (~0.001ms) that barely matters.
- **Post-write first re-scan** is slightly slower because the cache correctly invalidates on INSERT, forcing a full re-scan on the next read. This is correct behaviour — stale reads would be worse.

**Recommendation:** Enable `--cache` for read-heavy OLTP workloads (dashboards, reporting). For write-heavy streaming ingest, the cache overhead per write is negligible but the hit rate will be low, so it provides minimal benefit.

---

## 14. Row Expiration

Every row carries a `time_t expires` field written to disk in the `.dat` file header.

### Default TTL

If INSERT does not specify `EXPIRES`, the row gets `now + 3600` seconds (1-hour TTL).

### Explicit TTL

```sql
INSERT INTO SESSIONS VALUES(1, token_abc, NOW()) EXPIRES 9999999999;  -- far future
INSERT INTO SESSIONS VALUES(2, temp_token, NOW()) EXPIRES 1;           -- already expired
```

### Enforcement

`Table::alive(row)` is called on every row during:

- Full table scan (`SELECT *`)
- Index lookup (PK fast path)
- JOIN inner loop
- UPDATE and DELETE candidate check

Expired rows are **never returned** in results and **cannot be updated or deleted** (they are logically invisible). They remain in the `.dat` file as tombstoned records and are physically removed on the next compaction pass (future work).

### Verification

```
Before expiry: 3 rows visible (IDs 1, 2, 3)
Insert row 2 with EXPIRES=1 (unix epoch 1 = already past)
After query:   2 rows visible (IDs 1, 3) — row 2 filtered by alive()
```

---

## 15. Running the Cache Comparison Benchmark

```bash
# Start nothing — the script manages its own servers
python3 scripts/compare_cache.py 100000   # 100k rows
python3 scripts/compare_cache.py 1000000  # 1M rows (takes longer)
```

The script automatically:

1. Starts two server instances (ports 9400 and 9401)
2. Seeds identical data into both using the same random seed
3. Runs 7 benchmark scenarios
4. Prints a side-by-side comparison table
5. Shuts both servers down cleanly
