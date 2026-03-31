#include "storage.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
namespace fs = std::filesystem;

/* ── helpers ──────────────────────────────────────────────────────── */
static std::string join_tab(const std::vector<std::string>&v){
    std::string o; for(size_t i=0;i<v.size();++i){if(i)o+='\t';o+=v[i];}return o;}
static std::vector<std::string> split_tab(const std::string&s){
    std::vector<std::string> o; std::string c;
    for(char ch:s){if(ch=='\t'){o.push_back(c);c.clear();}else c+=ch;}
    o.push_back(c); return o;}

/* ── Table paths ──────────────────────────────────────────────────── */
std::string Table::schema_path()const{return data_dir+"/"+name+".sch";}
std::string Table::data_path()  const{return data_dir+"/"+name+".dat";}

/* ── Schema save ──────────────────────────────────────────────────── */
bool Table::save_schema()const{
    std::ofstream f(schema_path(),std::ios::trunc); if(!f)return false;
    for(auto&cd:schema)
        f<<cd.name<<'\t'<<coltype_to_str(cd.type)<<'\t'
         <<(cd.not_null?'1':'0')<<'\t'<<(cd.primary_key?'1':'0')<<'\n';
    return true;}

/* ── Row serialisation ────────────────────────────────────────────── */
std::string Table::serialise_row(const Row&r){
    std::string pl=join_tab(r.values);
    uint32_t plen=(uint32_t)pl.size();
    int64_t  exp =(int64_t)r.expires;
    uint8_t  del =r.deleted?1:0;
    std::string out(8+8+1+4+plen, '\0');
    char *p=&out[0];
    memcpy(p,     &r.id,  8); p+=8;
    memcpy(p,     &exp,   8); p+=8;
    memcpy(p,     &del,   1); p+=1;
    memcpy(p,     &plen,  4); p+=4;
    memcpy(p,     pl.data(), plen);
    return out;
}

/* ── WAL open (persistent file descriptor) ────────────────────────── */
void Table::open_wal(){
    if(wal_file_.is_open()) return;
    wal_file_.open(data_path(),
                   std::ios::binary|std::ios::app|std::ios::out);
    write_buf_.reserve(WRITE_BUF_BYTES * 2);
}

/* ── append_row — buffered, no open/close per row ─────────────────── */
bool Table::append_row(const Row&r){
    open_wal();
    if(!wal_file_.is_open()) return false;
    write_buf_ += serialise_row(r);
    ++unflushed_;
    if(unflushed_ >= WRITE_BATCH_SIZE || write_buf_.size() >= WRITE_BUF_BYTES){
        wal_file_.write(write_buf_.data(), (std::streamsize)write_buf_.size());
        wal_file_.flush();
        write_buf_.clear();
        unflushed_ = 0;
    }
    return true;
}

/* ── flush_wal — force all buffered bytes to disk ─────────────────── */
void Table::flush_wal(){
    if(!write_buf_.empty() && wal_file_.is_open()){
        wal_file_.write(write_buf_.data(), (std::streamsize)write_buf_.size());
        wal_file_.flush();
        write_buf_.clear();
        unflushed_ = 0;
    }
}

/* ── Load schema + rows from disk ─────────────────────────────────── */
bool Table::load(){
    std::ifstream sf(schema_path()); if(!sf)return false;
    std::string line; int idx=0;
    while(std::getline(sf,line)){
        if(line.empty())continue;
        auto p=split_tab(line); if(p.size()<4)continue;
        ColDef cd; cd.name=p[0]; cd.type=str_to_coltype(p[1]);
        cd.not_null=(p[2]=="1"); cd.primary_key=(p[3]=="1");
        if(cd.primary_key) pk_col=idx;
        schema.push_back(cd); ++idx;}
    if(schema.empty())return false;
    rows.clear(); pk_hash.clear();
    std::ifstream df(data_path(),std::ios::binary); if(!df)return true;
    uint64_t row_id; int64_t exp; uint8_t del; uint32_t plen;
    while(df.read((char*)&row_id,8)&&df.read((char*)&exp,8)
         &&df.read((char*)&del,1)&&df.read((char*)&plen,4)){
        std::string pl(plen,'\0');
        if(!df.read(&pl[0],plen))break;
        Row row; row.id=row_id; row.expires=(time_t)exp;
        row.deleted=(del!=0); row.values=split_tab(pl);
        row.values.resize(schema.size());
        size_t i=rows.size(); rows.push_back(std::move(row));
        if(!rows.back().deleted&&pk_col>=0&&pk_col<(int)rows.back().values.size()){
            const std::string&pkv=rows.back().values[(size_t)pk_col];
            pk_hash[pkv]=i;}}
    return true;
}

/* ── Database ─────────────────────────────────────────────────────── */
Table* Database::get_table(const std::string&n){
    std::lock_guard<std::mutex> lk(mu_);
    auto it=tables_.find(n); return it==tables_.end()?nullptr:it->second.get();}

bool Database::create_table(const std::string&n,const std::vector<ColDef>&schema){
    std::lock_guard<std::mutex> lk(mu_);
    if(tables_.count(n))return false;
    auto t=std::make_unique<Table>(); t->name=n; t->db_name=name;
    t->data_dir=data_dir; t->schema=schema;
    for(int i=0;i<(int)schema.size();++i) if(schema[i].primary_key){t->pk_col=i;break;}
    if(!t->save_schema())return false;
    tables_[n]=std::move(t); return true;}

std::vector<std::string> Database::list_tables()const{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> o; for(auto&kv:tables_)o.push_back(kv.first);
    std::sort(o.begin(),o.end()); return o;}

bool Database::load_from_disk(){
    std::lock_guard<std::mutex> lk(mu_);
    if(!fs::exists(data_dir))return true;
    for(auto&e:fs::directory_iterator(data_dir)){
        if(e.path().extension()!=".sch")continue;
        std::string tname=e.path().stem().string();
        if(tables_.count(tname))continue;
        auto t=std::make_unique<Table>(); t->name=tname;
        t->db_name=name; t->data_dir=data_dir;
        if(t->load()) tables_[tname]=std::move(t);}
    return true;}

void Database::flush_all(){
    std::lock_guard<std::mutex> lk(mu_);
    for(auto&kv:tables_) kv.second->flush_wal();}

/* ── Catalog ──────────────────────────────────────────────────────── */
Catalog::Catalog(const std::string&root):data_root_(root){
    fs::create_directories(data_root_); scan_disk();}

Catalog::~Catalog(){ flush_all(); }

void Catalog::scan_disk(){
    if(!fs::exists(data_root_))return;
    for(auto&e:fs::directory_iterator(data_root_)){
        if(!e.is_directory())continue;
        std::string n=e.path().filename().string();
        if(dbs_.count(n))continue;
        auto db=std::make_unique<Database>(); db->name=n;
        db->data_dir=data_root_+"/"+n; db->load_from_disk();
        dbs_[n]=std::move(db);}}

bool Catalog::create_database(const std::string&n){
    std::lock_guard<std::mutex> lk(mu_); if(dbs_.count(n))return false;
    fs::create_directories(data_root_+"/"+n);
    auto db=std::make_unique<Database>(); db->name=n;
    db->data_dir=data_root_+"/"+n; dbs_[n]=std::move(db); return true;}

bool Catalog::drop_database(const std::string&n){
    std::lock_guard<std::mutex> lk(mu_);
    auto it=dbs_.find(n); if(it==dbs_.end())return false;
    fs::remove_all(data_root_+"/"+n); dbs_.erase(it); return true;}

Database* Catalog::get_database(const std::string&n){
    std::lock_guard<std::mutex> lk(mu_);
    auto it=dbs_.find(n); return it==dbs_.end()?nullptr:it->second.get();}

std::vector<std::string> Catalog::list_databases()const{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> o; for(auto&kv:dbs_)o.push_back(kv.first);
    std::sort(o.begin(),o.end()); return o;}

void Catalog::flush_all(){
    std::lock_guard<std::mutex> lk(mu_);
    for(auto&kv:dbs_) kv.second->flush_all();}
