#pragma once
#include <string>
#include <list>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

/*
 * LRU Query Result Cache
 * ──────────────────────
 * Key    : normalised "db::table::sql_fingerprint" string
 * Value  : full result set (col_names + rows) + table generation counter
 *
 * Invalidation: generation-counter based.
 *   Every table has a uint64_t generation.
 *   INSERT / UPDATE / DELETE / DROP increments it.
 *   A cache entry is stale when its stored gen ≠ current table gen.
 *   Stale entries are evicted lazily on first access (no background sweep).
 *
 * Capacity: 4096 entries by default (each entry stores a full result set).
 * Thread-safety: one mutex guards the LRU list + map.
 *                A separate mutex guards the generation map.
 */

struct CacheEntry {
    std::vector<std::string>              col_names;
    std::vector<std::vector<std::string>> rows;
    uint64_t                              gen = 0;
};

class LRUCache {
public:
    explicit LRUCache(size_t capacity = 4096);

    /* Returns nullptr on miss or stale entry */
    const CacheEntry* get(const std::string &key, uint64_t current_gen);

    /* Store a result (overwrites existing entry for same key) */
    void put(const std::string &key,
             std::vector<std::string>              col_names,
             std::vector<std::vector<std::string>> rows,
             uint64_t gen);

    /* Increment generation for a table; returns new generation */
    uint64_t invalidate(const std::string &table_name);

    /* Read current generation for a table (0 if never written) */
    uint64_t generation(const std::string &table_name) const;

    void clear();

    /* Diagnostic stats */
    size_t hits()     const { return hits_;   }
    size_t misses()   const { return misses_; }
    size_t size()     const { return map_.size(); }
    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    mutable std::mutex mu_;

    using Node = std::pair<std::string, CacheEntry>;
    std::list<Node>                                           order_; // front = MRU
    std::unordered_map<std::string, std::list<Node>::iterator> map_;

    mutable std::mutex                        gen_mu_;
    std::unordered_map<std::string, uint64_t> gen_map_;

    size_t hits_   = 0;
    size_t misses_ = 0;
};
