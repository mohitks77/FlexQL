#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstdint>
#include <ctime>
#include <optional>
#include <fstream>
#include <deque>

/* ── Column types ─────────────────────────────────────────────────── */
enum class ColType { INT, DECIMAL, VARCHAR, DATETIME };

inline std::string coltype_to_str(ColType t){
    switch(t){
        case ColType::INT:      return "INT";
        case ColType::DECIMAL:  return "DECIMAL";
        case ColType::VARCHAR:  return "VARCHAR";
        case ColType::DATETIME: return "DATETIME";
    } return "VARCHAR";
}
inline ColType str_to_coltype(const std::string &s){
    /* INT and DATETIME silently alias to DECIMAL and VARCHAR per spec */
    if(s=="INT"||s=="INTEGER")                            return ColType::DECIMAL;
    if(s=="DECIMAL"||s=="FLOAT"||s=="DOUBLE"||s=="REAL")  return ColType::DECIMAL;
    if(s=="DATETIME"||s=="TIMESTAMP")                     return ColType::VARCHAR;
    return ColType::VARCHAR;
}

struct ColDef {
    std::string name;
    ColType     type        = ColType::VARCHAR;
    bool        not_null    = false;
    bool        primary_key = false;
};

/* ── Row ──────────────────────────────────────────────────────────── */
struct Row {
    uint64_t                 id;
    std::vector<std::string> values;
    time_t                   expires;
    bool                     deleted;
};

/*
 * ── Table ──────────────────────────────────────────────────────────
 *
 * Performance design:
 *   • Rows live entirely in memory (std::vector<Row>).
 *   • Disk write uses a PERSISTENT open file descriptor (no open/close per row).
 *   • Writes go into a std::string write_buf_ first; flushed to disk either:
 *       - every WRITE_BATCH_SIZE rows, OR
 *       - on explicit flush_wal() call (server shutdown / checkpoint).
 *   • This makes INSERT O(1) memory append + amortised O(1/N) disk I/O.
 *   • Reads never touch disk — always served from the in-memory vector.
 *
 * Primary-key indexes:
 *   pk_hash  : unordered_map  — O(1) equality lookup

 */
static constexpr size_t WRITE_BATCH_SIZE = 256;   // flush every N rows
static constexpr size_t WRITE_BUF_BYTES  = 256 * 1024; // or every 256 KB

class Table {
public:
    std::string          name, db_name, data_dir;
    std::vector<ColDef>  schema;
    int                  pk_col = -1;

    std::vector<Row>                       rows;
    std::unordered_map<std::string,size_t> pk_hash;
    mutable std::mutex                     mu;

    Table() = default;
    Table(const Table&)            = delete;
    Table& operator=(const Table&) = delete;

    ~Table() { flush_wal(); }

    int col_idx(const std::string &n) const {
        for(int i=0;i<(int)schema.size();++i) if(schema[i].name==n) return i;
        return -1;
    }
    static bool alive(const Row &r){
        if(r.deleted) return false;
        if(r.expires==0) return true;
        return std::time(nullptr) < r.expires;
    }

    bool load();
    bool save_schema() const;

    /* Buffer a row for disk write; flushes automatically when buffer is full */
    bool append_row(const Row &r);

    /* Force all buffered rows to disk (call on shutdown / checkpoint) */
    void flush_wal();

private:
    std::string schema_path() const;
    std::string data_path()   const;

    /* Persistent write file — opened once, kept open */
    std::ofstream  wal_file_;
    std::string    write_buf_;
    size_t         unflushed_ = 0;

    /* Serialise one row to binary string */
    static std::string serialise_row(const Row &r);

    void open_wal();
};

/* ── Database ─────────────────────────────────────────────────────── */
class Database {
public:
    std::string name, data_dir;

    Database() = default;
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    Table*                   get_table   (const std::string &name);
    bool                     create_table(const std::string &name,
                                          const std::vector<ColDef> &schema);
    std::vector<std::string> list_tables () const;
    bool                     load_from_disk();
    void                     flush_all   ();   // flush all tables' WAL buffers

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string,std::unique_ptr<Table>> tables_;
};

/* ── Catalog ──────────────────────────────────────────────────────── */
class Catalog {
public:
    explicit Catalog(const std::string &data_root);
    ~Catalog();

    bool                     create_database(const std::string &name);
    bool                     drop_database  (const std::string &name);
    Database*                get_database   (const std::string &name);
    std::vector<std::string> list_databases () const;
    void                     flush_all      ();

private:
    std::string               data_root_;
    mutable std::mutex        mu_;
    std::unordered_map<std::string,std::unique_ptr<Database>> dbs_;
    void scan_disk();
};
