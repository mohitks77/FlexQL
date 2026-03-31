#pragma once
#include "parser.h"
#include "storage.h"
#include "cache.h"
#include <string>
#include <vector>
#include <chrono>

struct ExecResult {
    bool                                  ok         = true;
    std::string                           err        = {};
    std::vector<std::string>              col_names  = {};
    std::vector<std::vector<std::string>> rows       = {};
    double                                elapsed_ms = 0.0;
    bool                                  from_cache = false;

    static ExecResult OK()                        { ExecResult r; r.ok=true;  return r; }
    static ExecResult Err(const std::string &m)   { ExecResult r; r.ok=false; r.err=m;  return r; }
};

struct Session {
    std::string current_db;
};

class Executor {
public:
    Executor(Catalog &catalog, LRUCache &cache);
    ExecResult execute(const std::string &sql, Session &session);

private:
    Catalog   &catalog_;
    LRUCache  &cache_;

    ExecResult exec_create_db  (const CreateDbStmt    &s, Session &sess);
    ExecResult exec_drop_db    (const DropDbStmt      &s, Session &sess);
    ExecResult exec_use_db     (const UseDbStmt       &s, Session &sess);
    ExecResult exec_show_dbs   (Session &sess);
    ExecResult exec_show_tables(Session &sess);
    ExecResult exec_create_tbl (const CreateTableStmt &s, Session &sess);
    ExecResult exec_drop_tbl   (const DropTableStmt   &s, Session &sess);
    ExecResult exec_insert     (const InsertStmt      &s, Session &sess);
    ExecResult exec_select     (const SelectStmt      &s, Session &sess);
    ExecResult exec_update     (const UpdateStmt      &s, Session &sess);
    ExecResult exec_delete     (const DeleteStmt      &s, Session &sess);
    ExecResult exec_describe   (const DescribeStmt    &s, Session &sess);

    Database*  require_db(const Session &sess, ExecResult &out);

    static bool        row_matches   (const Row &r, const Table &t,
                                      const WhereClause &w);
    static std::string validate_value(const std::string &val, const ColDef &cd);
    static bool        compare_vals  (const std::string &a, const std::string &op,
                                      const std::string &b, ColType type);
    std::string        make_cache_key(const std::string &db,
                                      const std::string &table,
                                      const std::string &sql_norm) const;
    void               write_invalidate(const std::string &table_name);
};
