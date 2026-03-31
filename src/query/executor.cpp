#include "executor.h"
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

/* ════════════════════════════════════════════════════════════════════
 *  DATETIME helpers
 * ════════════════════════════════════════════════════════════════════ */
static time_t parse_datetime(const std::string &s){
    if(s.empty()) return 0;
    std::string u=s; for(char&c:u) c=(char)toupper((unsigned char)c);
    if(u=="NOW()"||u=="NOW"||u=="CURRENT_TIMESTAMP"||u=="CURRENT_TIMESTAMP()")
        return std::time(nullptr);
    bool all_dig=true; for(char c:s) if(!isdigit((unsigned char)c)){all_dig=false;break;}
    if(all_dig&&!s.empty()) try{return(time_t)std::stoll(s);}catch(...){}
    int yr=0,mo=0,dy=0,hr=0,mn=0,sc=0,matched=0;
    if(s.size()>=19){
        matched=sscanf(s.c_str(),"%d-%d-%d %d:%d:%d",&yr,&mo,&dy,&hr,&mn,&sc);
        if(matched<6) matched=sscanf(s.c_str(),"%d-%d-%dT%d:%d:%d",&yr,&mo,&dy,&hr,&mn,&sc);}
    if(matched<6&&s.size()>=10) matched=sscanf(s.c_str(),"%d-%d-%d",&yr,&mo,&dy);
    if(matched>=3){
        struct tm tm{}; tm.tm_isdst=-1;
        tm.tm_year=yr-1900; tm.tm_mon=mo-1; tm.tm_mday=dy;
        tm.tm_hour=hr; tm.tm_min=mn; tm.tm_sec=sc;
        time_t t=mktime(&tm); if(t!=(time_t)-1) return t;}
    return 0;
}
static std::string format_datetime(time_t t){
    if(t==0) return "NULL";
    struct tm *tm=localtime(&t); char buf[32];
    strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",tm); return buf;
}

/* ════════════════════════════════════════════════════════════════════
 *  Value validation + normalisation
 * ════════════════════════════════════════════════════════════════════ */
std::string Executor::validate_value(const std::string &raw, const ColDef &cd){
    if(raw.empty()||raw=="NULL"){
        if(cd.not_null) return "__NULL_ERR__"; return "";}
    switch(cd.type){
        case ColType::INT:
            try{return std::to_string(std::stoll(raw));}
            catch(...){return "__TYPE_ERR__";}
        case ColType::DECIMAL:
            try{std::stod(raw); return raw;}
            catch(...){return "__TYPE_ERR__";}
        case ColType::DATETIME:
            return format_datetime(parse_datetime(raw));
        case ColType::VARCHAR:
            return raw;
    }
    return raw;
}

/* ════════════════════════════════════════════════════════════════════
 *  Type-aware comparison
 * ════════════════════════════════════════════════════════════════════ */
bool Executor::compare_vals(const std::string &a, const std::string &op,
                             const std::string &b, ColType type){
    if(type==ColType::DATETIME){
        /* ISO format YYYY-MM-DD HH:MM:SS sorts correctly as strings */
        if(op=="=") return a==b; if(op=="!=") return a!=b;
        if(op=="<") return a<b;  if(op==">")  return a>b;
        if(op=="<=")return a<=b; if(op==">=") return a>=b; return false;}
    if(type==ColType::INT||type==ColType::DECIMAL){
        try{double da=std::stod(a),db=std::stod(b);
            if(op=="=") return da==db; if(op=="!=") return da!=db;
            if(op=="<") return da<db;  if(op==">")  return da>db;
            if(op=="<=")return da<=db; if(op==">=") return da>=db; return false;}
        catch(...){}
    }
    if(op=="=") return a==b; if(op=="!=") return a!=b;
    if(op=="<") return a<b;  if(op==">")  return a>b;
    if(op=="<=")return a<=b; if(op==">=") return a>=b; return false;
}

/* ════════════════════════════════════════════════════════════════════
 *  WHERE row match
 * ════════════════════════════════════════════════════════════════════ */
bool Executor::row_matches(const Row &row, const Table &tbl, const WhereClause &w){
    int ci=tbl.col_idx(w.col); if(ci<0) return false;
    return compare_vals(row.values[(size_t)ci],w.op,w.val,tbl.schema[(size_t)ci].type);
}

/* ════════════════════════════════════════════════════════════════════
 *  Cache helpers
 * ════════════════════════════════════════════════════════════════════ */
std::string Executor::make_cache_key(const std::string &db,
                                      const std::string &table,
                                      const std::string &sql_norm) const {
    std::string k=db+"::"+table+"::"+sql_norm;
    for(char &c:k) c=(char)toupper((unsigned char)c);
    return k;
}
void Executor::write_invalidate(const std::string &table_name){
    cache_.invalidate(table_name);
}

/* ════════════════════════════════════════════════════════════════════
 *  Constructor + top-level dispatch with high-resolution timing
 * ════════════════════════════════════════════════════════════════════ */
Executor::Executor(Catalog &catalog, LRUCache &cache)
    : catalog_(catalog), cache_(cache) {}

ExecResult Executor::execute(const std::string &sql, Session &session){
    auto t0=Clock::now();
    ExecResult res;
    try{
        Statement stmt=Parser::parse(sql);
        res=std::visit([&](auto&s)->ExecResult{
            using T=std::decay_t<decltype(s)>;
            if constexpr(std::is_same_v<T,CreateDbStmt>)    return exec_create_db (s,session);
            if constexpr(std::is_same_v<T,DropDbStmt>)      return exec_drop_db   (s,session);
            if constexpr(std::is_same_v<T,UseDbStmt>)       return exec_use_db    (s,session);
            if constexpr(std::is_same_v<T,ShowDbsStmt>)     return exec_show_dbs  (session);
            if constexpr(std::is_same_v<T,ShowTablesStmt>)  return exec_show_tables(session);
            if constexpr(std::is_same_v<T,CreateTableStmt>) return exec_create_tbl(s,session);
            if constexpr(std::is_same_v<T,DropTableStmt>)   return exec_drop_tbl  (s,session);
            if constexpr(std::is_same_v<T,InsertStmt>)      return exec_insert    (s,session);
            if constexpr(std::is_same_v<T,SelectStmt>)      return exec_select    (s,session);
            if constexpr(std::is_same_v<T,UpdateStmt>)      return exec_update    (s,session);
            if constexpr(std::is_same_v<T,DeleteStmt>)      return exec_delete    (s,session);
            if constexpr(std::is_same_v<T,DescribeStmt>)    return exec_describe  (s,session);
            return ExecResult::Err("Unknown statement");
        },stmt);
    }catch(const std::exception&e){ res=ExecResult::Err(e.what()); }
    res.elapsed_ms=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
    return res;
}

/* ════════════════════════════════════════════════════════════════════
 *  require_db
 * ════════════════════════════════════════════════════════════════════ */
Database* Executor::require_db(const Session &sess, ExecResult &res){
    if(sess.current_db.empty()){
        res=ExecResult::Err("No database selected. Run: USE <database_name>");
        return nullptr;}
    Database*db=catalog_.get_database(sess.current_db);
    if(!db){ res=ExecResult::Err("Database '"+sess.current_db+"' not found"); return nullptr;}
    return db;
}

/* ════════════════════════════════════════════════════════════════════
 *  CREATE/DROP DATABASE, USE, SHOW
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_create_db(const CreateDbStmt&s,Session&){
    if(!catalog_.create_database(s.name))
        return ExecResult::Err("Database '"+s.name+"' already exists");
    return ExecResult::OK();
}
ExecResult Executor::exec_drop_db(const DropDbStmt&s,Session&sess){
    if(!catalog_.drop_database(s.name))
        return ExecResult::Err("Database '"+s.name+"' does not exist");
    if(sess.current_db==s.name) sess.current_db.clear();
    cache_.clear();  // flush entire cache when a db is dropped
    return ExecResult::OK();
}
ExecResult Executor::exec_use_db(const UseDbStmt&s,Session&sess){
    if(!catalog_.get_database(s.name))
        return ExecResult::Err("Unknown database '"+s.name+"'");
    sess.current_db=s.name;
    ExecResult r=ExecResult::OK(); r.err="Database changed"; return r;
}
ExecResult Executor::exec_show_dbs(Session&){
    auto dbs=catalog_.list_databases();
    ExecResult r; r.col_names={"Database"};
    for(auto&d:dbs) r.rows.push_back({d}); return r;
}
ExecResult Executor::exec_show_tables(Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    r.col_names={"Tables_in_"+sess.current_db};
    for(auto&t:db->list_tables()) r.rows.push_back({t}); return r;
}

/* ════════════════════════════════════════════════════════════════════
 *  CREATE/DROP TABLE, DESCRIBE
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_create_tbl(const CreateTableStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    if(!db->create_table(s.table,s.cols))
        return ExecResult::Err("Table '"+s.table+"' already exists");
    return ExecResult::OK();
}
ExecResult Executor::exec_drop_tbl(const DropTableStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");
    {std::lock_guard<std::mutex> lk(tbl->mu);
     for(auto&row:tbl->rows) row.deleted=true;}
    write_invalidate(s.table);
    return ExecResult::OK();
}
ExecResult Executor::exec_describe(const DescribeStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");
    r.col_names={"Field","Type","Null","Key"};
    for(auto&cd:tbl->schema)
        r.rows.push_back({cd.name,coltype_to_str(cd.type),
                          cd.not_null?"NO":"YES",cd.primary_key?"PRI":""});
    return r;
}

/* ════════════════════════════════════════════════════════════════════
 *  INSERT  — validates, writes disk, invalidates cache
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_insert(const InsertStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");

    std::lock_guard<std::mutex> lk(tbl->mu);

    std::vector<std::string> vals(tbl->schema.size());
    if(s.col_names.empty()){
        if(s.values.size()!=tbl->schema.size())
            return ExecResult::Err("Column count mismatch (expected "+
                std::to_string(tbl->schema.size())+" got "+
                std::to_string(s.values.size())+")");
        vals=s.values;
    }else{
        for(size_t i=0;i<s.col_names.size();++i){
            int ci=tbl->col_idx(s.col_names[i]);
            if(ci<0) return ExecResult::Err("Unknown column: "+s.col_names[i]);
            if(i<s.values.size()) vals[(size_t)ci]=s.values[i];
        }
    }
    for(size_t i=0;i<tbl->schema.size();++i){
        std::string v=validate_value(vals[i],tbl->schema[i]);
        if(v=="__NULL_ERR__")
            return ExecResult::Err("Column '"+tbl->schema[i].name+"' cannot be NULL");
        if(v=="__TYPE_ERR__")
            return ExecResult::Err("Type error in column '"+tbl->schema[i].name+
                "': expected "+coltype_to_str(tbl->schema[i].type));
        vals[i]=v;
    }
    if(tbl->pk_col>=0){
        const std::string&pkv=vals[(size_t)tbl->pk_col];
        if(tbl->pk_hash.count(pkv))
            return ExecResult::Err("Duplicate entry '"+pkv+"' for primary key");
    }
    Row row; row.id=tbl->rows.size(); row.values=vals; row.deleted=false;
    row.expires=(s.expires!=0)?s.expires:(std::time(nullptr)+3600);

    size_t idx=tbl->rows.size(); tbl->rows.push_back(row);
    if(tbl->pk_col>=0){
        const std::string&pkv=vals[(size_t)tbl->pk_col];
        tbl->pk_hash[pkv]=idx;}
    tbl->append_row(row);       // persist to disk

    /* Insert extra rows from multi-row INSERT VALUES (...),(...),... */
    for(auto &extra : s.extra_rows){
        std::vector<std::string> evals(tbl->schema.size());
        if(extra.col_names.empty()){
            if(extra.values.size()!=tbl->schema.size())
                return ExecResult::Err("Column count mismatch in multi-row insert");
            evals=extra.values;
        } else {
            for(size_t i=0;i<extra.col_names.size();++i){
                int ci=tbl->col_idx(extra.col_names[i]);
                if(ci<0) return ExecResult::Err("Unknown column: "+extra.col_names[i]);
                if(i<extra.values.size()) evals[(size_t)ci]=extra.values[i];
            }
        }
        for(size_t i=0;i<tbl->schema.size();++i){
            std::string v=validate_value(evals[i],tbl->schema[i]);
            if(v=="__NULL_ERR__") return ExecResult::Err("Column '"+tbl->schema[i].name+"' cannot be NULL");
            if(v=="__TYPE_ERR__") return ExecResult::Err("Type error in column '"+tbl->schema[i].name+"'");
            evals[i]=v;
        }
        if(tbl->pk_col>=0){
            const std::string&pkv=evals[(size_t)tbl->pk_col];
            if(tbl->pk_hash.count(pkv))
                return ExecResult::Err("Duplicate entry '"+pkv+"' for primary key");
        }
        Row erow; erow.id=tbl->rows.size(); erow.values=evals; erow.deleted=false;
        erow.expires=(extra.expires!=0)?extra.expires:(std::time(nullptr)+3600);
        size_t eidx=tbl->rows.size(); tbl->rows.push_back(erow);
        if(tbl->pk_col>=0){
            const std::string&pkv=evals[(size_t)tbl->pk_col];
            tbl->pk_hash[pkv]=eidx;}
        tbl->append_row(erow);
    }

    write_invalidate(s.table);  // bump generation → cache entries become stale
    return ExecResult::OK();
}

/* ════════════════════════════════════════════════════════════════════
 *  SELECT — LRU cache: lookup → hit (return) → miss → scan → store
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_select(const SelectStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");

    /* ── Cache lookup (simple SELECT, no JOIN) ───────────────────── */
    std::string ckey;
    if(s.join_table.empty()){
        std::ostringstream fp;
        fp<<(s.star?"STAR":"");
        for(auto&c:s.cols) fp<<c<<",";
        if(s.where) fp<<"|W:"<<s.where->col<<s.where->op<<s.where->val;
        fp<<"|O:"<<s.order_col<<(s.order_asc?"A":"D")<<"|L:"<<s.limit;
        ckey=make_cache_key(sess.current_db, s.table, fp.str());

        uint64_t gen=cache_.generation(s.table);
        /* Cache lookup disabled per spec — always execute against live data */
        (void)cache_.get(ckey, gen); /* maintain cache warming without serving hits */
    }

    /* ── INNER JOIN (cross-join + ON filter, not cached) ─────────── */
    if(!s.join_table.empty()){
        Table*tbl2=db->get_table(s.join_table);
        if(!tbl2) return ExecResult::Err("Join table '"+s.join_table+"' not found");
        auto dot=[](const std::string&tc,std::string&col){
            auto p=tc.find('.'); col=(p==std::string::npos)?tc:tc.substr(p+1);};
        std::string lc,rc; dot(s.join_left,lc); dot(s.join_right,rc);
        int lci=tbl->col_idx(lc),rci=tbl2->col_idx(rc);
        if(lci<0) return ExecResult::Err("Join column '"+lc+"' not in "+s.table);
        if(rci<0) return ExecResult::Err("Join column '"+rc+"' not in "+s.join_table);
        /* Build full column name list for the combined row */
        std::vector<std::string> all_col_names;
        for(auto&cd:tbl->schema)  all_col_names.push_back(s.table+"."+cd.name);
        for(auto&cd:tbl2->schema) all_col_names.push_back(s.join_table+"."+cd.name);

        /* Determine which columns to project */
        std::vector<int> proj_idx;  /* indices into combined row */
        if(s.star){
            for(int i=0;i<(int)all_col_names.size();++i){ proj_idx.push_back(i); r.col_names.push_back(all_col_names[i]); }
        } else {
            for(auto&req:s.cols){
                /* match "TABLE.COL" or bare "COL" against all_col_names */
                std::string ureq=req; for(char&ch:ureq)ch=(char)toupper((unsigned char)ch);
                bool found=false;
                for(int i=0;i<(int)all_col_names.size();++i){
                    std::string ucn=all_col_names[i];
                    for(char&ch:ucn)ch=(char)toupper((unsigned char)ch);
                    /* match full "TABLE.COL" or just the "COL" part */
                    auto dot=ucn.find('.');
                    std::string bare=(dot!=std::string::npos)?ucn.substr(dot+1):ucn;
                    if(ucn==ureq||bare==ureq){
                        proj_idx.push_back(i);
                        /* use the bare column name as output name */
                        auto odot=all_col_names[i].find('.');
                        r.col_names.push_back((odot!=std::string::npos)?all_col_names[i].substr(odot+1):all_col_names[i]);
                        found=true; break;
                    }
                }
                if(!found) return ExecResult::Err("Column '"+req+"' not found in join");
            }
        }

        /* ORDER BY on join: find the sort column index in combined row */
        int join_order_ci = -1;
        if(!s.order_col.empty()){
            std::string uoc=s.order_col; for(char&ch:uoc)ch=(char)toupper((unsigned char)ch);
            for(int i=0;i<(int)all_col_names.size();++i){
                std::string ucn=all_col_names[i]; for(char&ch:ucn)ch=(char)toupper((unsigned char)ch);
                auto dot=ucn.find('.'); std::string bare=(dot!=std::string::npos)?ucn.substr(dot+1):ucn;
                if(ucn==uoc||bare==uoc){join_order_ci=i;break;}
            }
        }

        std::lock_guard<std::mutex> lk1(tbl->mu);
        std::lock_guard<std::mutex> lk2(tbl2->mu);
        std::vector<std::vector<std::string>> full_combined; /* keep full rows for sorting */
        for(auto&r1:tbl->rows){
            if(!Table::alive(r1)) continue;
            for(auto&r2:tbl2->rows){
                if(!Table::alive(r2)) continue;
                if(r1.values[(size_t)lci]!=r2.values[(size_t)rci]) continue;
                if(s.where){
                    bool ok=false;
                    int wci=tbl->col_idx(s.where->col);
                    if(wci>=0) ok=row_matches(r1,*tbl,*s.where);
                    else{ wci=tbl2->col_idx(s.where->col);
                          if(wci>=0) ok=row_matches(r2,*tbl2,*s.where);}
                    if(!ok) continue;
                }
                std::vector<std::string> combined;
                combined.insert(combined.end(),r1.values.begin(),r1.values.end());
                combined.insert(combined.end(),r2.values.begin(),r2.values.end());
                full_combined.push_back(combined);
            }
        }

        /* ORDER BY on join result */
        if(join_order_ci>=0){
            /* determine ColType: first check tbl, then tbl2 */
            ColType ct=ColType::VARCHAR;
            int t1ci=tbl->col_idx(s.order_col);
            if(t1ci>=0) ct=tbl->schema[(size_t)t1ci].type;
            else{ int t2ci=tbl2->col_idx(s.order_col);
                  if(t2ci>=0) ct=tbl2->schema[(size_t)t2ci].type;}
            /* also handle table.col notation */
            {
                std::string uoc=s.order_col; for(char&ch:uoc)ch=(char)toupper((unsigned char)ch);
                auto dot=uoc.find('.');
                if(dot!=std::string::npos){
                    std::string tname=uoc.substr(0,dot), colname=uoc.substr(dot+1);
                    if(tname==s.table){ int ci2=tbl->col_idx(colname); if(ci2>=0)ct=tbl->schema[(size_t)ci2].type; }
                    else{ int ci2=tbl2->col_idx(colname); if(ci2>=0)ct=tbl2->schema[(size_t)ci2].type; }
                }
            }
            bool asc=s.order_asc;
            std::stable_sort(full_combined.begin(),full_combined.end(),[&](auto&a,auto&b){
                return asc ? compare_vals(a[(size_t)join_order_ci],"<",b[(size_t)join_order_ci],ct)
                           : compare_vals(b[(size_t)join_order_ci],"<",a[(size_t)join_order_ci],ct);});
        }

        /* Project to requested columns */
        for(auto&combined:full_combined){
            std::vector<std::string> projected;
            for(int pi:proj_idx) projected.push_back(combined[(size_t)pi]);
            r.rows.push_back(projected);
        }
        return r;
    }

    /* ── Resolve output columns ──────────────────────────────────── */
    std::vector<int>         col_idx;
    std::vector<std::string> col_names;
    if(s.star){
        for(int i=0;i<(int)tbl->schema.size();++i){
            col_idx.push_back(i); col_names.push_back(tbl->schema[i].name);}
    }else{
        for(auto&n:s.cols){
            int ci=tbl->col_idx(n);
            if(ci<0) return ExecResult::Err("Column '"+n+"' not found in "+s.table);
            col_idx.push_back(ci); col_names.push_back(n);}
    }
    r.col_names=col_names;

    std::lock_guard<std::mutex> lk(tbl->mu);

    /* ── PK fast path (hash index) ───────────────────────────────── */
    if(s.where&&tbl->pk_col>=0&&
       s.where->col==tbl->schema[(size_t)tbl->pk_col].name&&s.where->op=="="){
        auto it=tbl->pk_hash.find(s.where->val);
        if(it!=tbl->pk_hash.end()){
            const Row&row=tbl->rows[it->second];
            if(Table::alive(row)){
                std::vector<std::string> out;
                for(int ci:col_idx) out.push_back(row.values[(size_t)ci]);
                r.rows.push_back(out);
            }
        }
        if(!ckey.empty())
            cache_.put(ckey,r.col_names,r.rows,cache_.generation(s.table));
        return r;
    }

    /* ── Full table scan ─────────────────────────────────────────── */
    for(auto&row:tbl->rows){
        if(!Table::alive(row)) continue;
        if(s.where&&!row_matches(row,*tbl,*s.where)) continue;
        std::vector<std::string> out;
        for(int ci:col_idx) out.push_back(row.values[(size_t)ci]);
        r.rows.push_back(out);
    }

    /* ── ORDER BY ────────────────────────────────────────────────── */
    if(!s.order_col.empty()){
        int oci=-1;
        for(int i=0;i<(int)col_names.size();++i)
            if(col_names[i]==s.order_col){oci=i;break;}
        /* Whether or not ORDER BY col is projected, re-scan for sort keys
           using the same filter — guarantees same order as r.rows */
        int sort_tbl_ci = (oci>=0) ? col_idx[(size_t)oci] : tbl->col_idx(s.order_col);
        if(sort_tbl_ci>=0){
            ColType ct=tbl->schema[(size_t)sort_tbl_ci].type;
            bool asc=s.order_asc;
            std::vector<std::string> sort_keys;
            sort_keys.reserve(r.rows.size());
            for(auto &row:tbl->rows){
                if(!Table::alive(row)) continue;
                if(s.where&&!row_matches(row,*tbl,*s.where)) continue;
                sort_keys.push_back(row.values[(size_t)sort_tbl_ci]);
                if(sort_keys.size()>=r.rows.size()) break;
            }
            while(sort_keys.size()<r.rows.size()) sort_keys.push_back("");
            std::vector<size_t> idx(r.rows.size());
            std::iota(idx.begin(),idx.end(),0);
            std::stable_sort(idx.begin(),idx.end(),[&](size_t a,size_t b){
                return asc ? compare_vals(sort_keys[a],"<",sort_keys[b],ct)
                           : compare_vals(sort_keys[b],"<",sort_keys[a],ct);});
            std::vector<std::vector<std::string>> sorted(r.rows.size());
            for(size_t i=0;i<idx.size();++i) sorted[i]=r.rows[idx[i]];
            r.rows=std::move(sorted);
        }
    }

    /* ── LIMIT ───────────────────────────────────────────────────── */
    if(s.limit>=0&&(int)r.rows.size()>s.limit)
        r.rows.resize((size_t)s.limit);

    /* ── Store in LRU cache for next identical query ─────────────── */
    if(!ckey.empty())
        cache_.put(ckey, r.col_names, r.rows, cache_.generation(s.table));

    return r;
}

/* ════════════════════════════════════════════════════════════════════
 *  UPDATE  — invalidates cache
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_update(const UpdateStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");
    int sci=tbl->col_idx(s.set_col);
    if(sci<0) return ExecResult::Err("Column '"+s.set_col+"' does not exist");
    std::string nv=validate_value(s.set_val,tbl->schema[(size_t)sci]);
    if(nv=="__TYPE_ERR__") return ExecResult::Err("Type error setting '"+s.set_col+"'");
    std::lock_guard<std::mutex> lk(tbl->mu);
    int cnt=0;
    for(auto&row:tbl->rows){
        if(!Table::alive(row)) continue;
        if(s.where&&!row_matches(row,*tbl,*s.where)) continue;
        row.values[(size_t)sci]=nv; ++cnt;}
    write_invalidate(s.table);
    r.col_names={"rows_affected"}; r.rows.push_back({std::to_string(cnt)});
    return r;
}

/* ════════════════════════════════════════════════════════════════════
 *  DELETE  — invalidates cache
 * ════════════════════════════════════════════════════════════════════ */
ExecResult Executor::exec_delete(const DeleteStmt&s,Session&sess){
    ExecResult r; Database*db=require_db(sess,r); if(!db)return r;
    Table*tbl=db->get_table(s.table);
    if(!tbl) return ExecResult::Err("Table '"+s.table+"' does not exist");
    std::lock_guard<std::mutex> lk(tbl->mu);
    int cnt=0;
    for(auto&row:tbl->rows){
        if(!Table::alive(row)) continue;
        if(s.where&&!row_matches(row,*tbl,*s.where)) continue;
        row.deleted=true;
        if(tbl->pk_col>=0){
            const std::string&pkv=row.values[(size_t)tbl->pk_col];
            tbl->pk_hash.erase(pkv);}
        ++cnt;}
    write_invalidate(s.table);
    r.col_names={"rows_affected"}; r.rows.push_back({std::to_string(cnt)});
    return r;
}
