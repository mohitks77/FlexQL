#include "cache.h"

LRUCache::LRUCache(size_t cap):capacity_(cap){}

const CacheEntry* LRUCache::get(const std::string&key,uint64_t cur_gen){
    std::lock_guard<std::mutex> lk(mu_);
    auto it=map_.find(key);
    if(it==map_.end()){++misses_;return nullptr;}
    if(it->second->second.gen!=cur_gen){
        order_.erase(it->second);map_.erase(it);++misses_;return nullptr;}
    order_.splice(order_.begin(),order_,it->second);
    ++hits_; return &order_.front().second;
}

void LRUCache::put(const std::string&key,
                   std::vector<std::string> cn,
                   std::vector<std::vector<std::string>> rows,
                   uint64_t gen){
    std::lock_guard<std::mutex> lk(mu_);
    auto it=map_.find(key);
    if(it!=map_.end()){order_.erase(it->second);map_.erase(it);}
    CacheEntry e; e.col_names=std::move(cn); e.rows=std::move(rows); e.gen=gen;
    order_.push_front({key,std::move(e)}); map_[key]=order_.begin();
    while(map_.size()>capacity_){map_.erase(order_.back().first);order_.pop_back();}
}

uint64_t LRUCache::invalidate(const std::string&tbl){
    std::lock_guard<std::mutex> lk(gen_mu_); return ++gen_map_[tbl];}
uint64_t LRUCache::generation(const std::string&tbl)const{
    std::lock_guard<std::mutex> lk(gen_mu_);
    auto it=gen_map_.find(tbl); return it==gen_map_.end()?0:it->second;}
void LRUCache::clear(){
    std::lock_guard<std::mutex> lk(mu_);
    order_.clear();map_.clear();hits_=misses_=0;}
