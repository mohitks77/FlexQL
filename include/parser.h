#pragma once
#include "storage.h"
#include <string>
#include <vector>
#include <variant>
#include <optional>

/* ── WHERE clause ─────────────────────────────────────────────────── */
struct WhereClause {
    std::string col, op, val;
};

/* ── AST nodes ────────────────────────────────────────────────────── */

struct CreateDbStmt   { std::string name; };
struct DropDbStmt     { std::string name; };
struct UseDbStmt      { std::string name; };
struct ShowDbsStmt    {};
struct ShowTablesStmt {};

struct CreateTableStmt {
    std::string         table;
    std::vector<ColDef> cols;
};

struct DropTableStmt  { std::string table; };

struct InsertStmt {
    std::string              table;
    std::vector<std::string> col_names;   // optional explicit col list
    std::vector<std::string> values;
    time_t                   expires = 0; // 0 = now+3600 (default 1h TTL)
    std::vector<InsertStmt>  extra_rows;  // additional rows for multi-row INSERT
};

struct SelectStmt {
    bool                      star  = false;
    std::vector<std::string>  cols;
    std::string               table;
    std::optional<WhereClause> where;
    /* JOIN */
    std::string               join_table;
    std::string               join_left;  // e.g. "A.COL"
    std::string               join_right; // e.g. "B.COL"
    /* ORDER BY */
    std::string               order_col;
    bool                      order_asc = true;
    /* LIMIT */
    int                       limit = -1;
};

struct UpdateStmt {
    std::string table;
    std::string set_col;
    std::string set_val;
    std::optional<WhereClause> where;
};

struct DeleteStmt {
    std::string table;
    std::optional<WhereClause> where;
};

struct DescribeStmt { std::string table; };

using Statement = std::variant<
    CreateDbStmt, DropDbStmt, UseDbStmt,
    ShowDbsStmt, ShowTablesStmt,
    CreateTableStmt, DropTableStmt,
    InsertStmt, SelectStmt,
    UpdateStmt, DeleteStmt,
    DescribeStmt
>;

/* ── Parser ───────────────────────────────────────────────────────── */
class Parser {
public:
    static Statement parse(const std::string &sql);

private:
    static std::vector<std::string> tokenise(const std::string &sql);
    static std::string upper(const std::string &s);
    static std::string strip_quotes(const std::string &s);
    static WhereClause parse_where(const std::vector<std::string> &t, size_t &pos);
};
