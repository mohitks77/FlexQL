#include "parser.h"
#include <stdexcept>
#include <cctype>
#include <algorithm>

/* ── helpers ──────────────────────────────────────────────────────── */
std::string Parser::upper(const std::string &s) {
    std::string r=s;
    for(char &c:r) c=(char)toupper((unsigned char)c);
    return r;
}

std::string Parser::strip_quotes(const std::string &s) {
    if(s.size()>=2&&((s.front()=='\''&&s.back()=='\'')||(s.front()=='"'&&s.back()=='"')))
        return s.substr(1,s.size()-2);
    return s;
}

/* ── tokeniser ────────────────────────────────────────────────────── */
std::vector<std::string> Parser::tokenise(const std::string &sql) {
    std::vector<std::string> toks;
    std::string cur;
    bool sq=false,dq=false;

    for(size_t i=0;i<sql.size();++i){
        char c=sql[i];
        if(sq){cur+=c;if(c=='\'')sq=false;continue;}
        if(dq){cur+=c;if(c=='"')dq=false;continue;}
        if(c=='\''){sq=true;cur+=c;continue;}
        if(c=='"'){dq=true;cur+=c;continue;}

        /* single-char punctuation tokens */
        if(c=='('||c==')'||c==','||c==';'){
            if(!cur.empty()){toks.push_back(cur);cur.clear();}
            toks.push_back({c});continue;
        }
        /* comparison operators (possibly two chars) */
        if(c=='<'||c=='>'||c=='='||c=='!'){
            if(!cur.empty()){toks.push_back(cur);cur.clear();}
            if(i+1<sql.size()){
                char nc=sql[i+1];
                if((c=='<'&&nc=='=')||(c=='>'&&nc=='=')||(c=='!'&&nc=='=')){
                    toks.push_back({c,nc});++i;continue;
                }
            }
            toks.push_back({c});continue;
        }
        if(isspace((unsigned char)c)){
            if(!cur.empty()){toks.push_back(cur);cur.clear();}
            continue;
        }
        cur+=c;
    }
    if(!cur.empty())toks.push_back(cur);
    return toks;
}

/* ── WHERE helper ─────────────────────────────────────────────────── */
WhereClause Parser::parse_where(const std::vector<std::string> &t,size_t &pos){
    WhereClause w;
    if(pos>=t.size()) throw std::runtime_error("Expected column after WHERE");
    w.col=upper(t[pos++]);
    /* handle table.col notation */
    if(w.col.find('.')!=std::string::npos)
        w.col=w.col.substr(w.col.find('.')+1);
    if(pos>=t.size()) throw std::runtime_error("Expected operator in WHERE");
    w.op=t[pos++];
    if(pos>=t.size()) throw std::runtime_error("Expected value in WHERE");
    w.val=strip_quotes(t[pos++]);
    return w;
}

/* ── main parse ───────────────────────────────────────────────────── */

static std::vector<std::string> merge_now(std::vector<std::string> toks){
    std::vector<std::string> out;
    for(size_t i=0;i<toks.size();++i){
        std::string u=toks[i];
        for(char &c:u) c=(char)toupper((unsigned char)c);
        if(u=="NOW"&&i+2<toks.size()&&toks[i+1]=="("&&toks[i+2]==")"){
            out.push_back("NOW()"); i+=2; continue;
        }
        if((u=="CURRENT_TIMESTAMP")&&i+2<toks.size()&&toks[i+1]=="("&&toks[i+2]==")"){
            out.push_back("NOW()"); i+=2; continue;
        }
        out.push_back(toks[i]);
    }
    return out;
}

Statement Parser::parse(const std::string &sql){
    auto toks=merge_now(tokenise(sql));
    if(toks.empty()) throw std::runtime_error("Empty query");
    size_t pos=0;
    auto peek=[&]()->std::string{
        return pos<toks.size()?upper(toks[pos]):"";
    };
    auto consume=[&]()->std::string{
        if(pos>=toks.size()) throw std::runtime_error("Unexpected end of query");
        return toks[pos++];
    };
    auto expect=[&](const std::string &kw){
        std::string t=upper(consume());
        if(t!=kw) throw std::runtime_error("Expected '"+kw+"' got '"+t+"'");
    };

    std::string kw=upper(consume());

    /* ── SHOW ──────────────────────────────────────────────────────── */
    if(kw=="SHOW"){
        std::string what=upper(consume());
        if(what=="DATABASES"||what=="SCHEMAS") return ShowDbsStmt{};
        if(what=="TABLES")                     return ShowTablesStmt{};
        throw std::runtime_error("Unknown SHOW target: "+what);
    }

    /* ── CREATE ────────────────────────────────────────────────────── */
    if(kw=="CREATE"){
        std::string obj=upper(consume());
        if(obj=="DATABASE"||obj=="SCHEMA"){
            CreateDbStmt s; s.name=upper(consume()); return s;
        }
        if(obj=="TABLE"){
            CreateTableStmt s;
            /* skip optional IF NOT EXISTS */
            if(peek()=="IF"){consume();
                if(upper(peek())=="NOT")consume();
                if(upper(peek())=="EXISTS")consume();}
            s.table=upper(consume());
            expect("(");
            while(peek()!=")"){
                if(peek()==","){ consume(); continue; }
                ColDef cd;
                cd.name=upper(consume());
                std::string tp=upper(consume());
                /* strip VARCHAR(n) */
                if(peek()=="("){consume();consume();consume();}
                cd.type=str_to_coltype(tp);
                while(peek()!=")"&&peek()!=","&&!peek().empty()){
                    std::string m=upper(consume());
                    if(m=="PRIMARY"){if(peek()=="KEY")consume();cd.primary_key=true;}
                    else if(m=="NOT"){if(peek()=="NULL")consume();cd.not_null=true;}
                    else if(m=="NULL"||m=="AUTO_INCREMENT"||m=="DEFAULT"){}
                    /* skip DEFAULT value */
                    else if(m=="UNIQUE"){}
                }
                s.cols.push_back(cd);
            }
            expect(")");
            return s;
        }
        throw std::runtime_error("Unknown CREATE target: "+obj);
    }

    /* ── DROP ──────────────────────────────────────────────────────── */
    if(kw=="DROP"){
        std::string obj=upper(consume());
        if(obj=="DATABASE"||obj=="SCHEMA"){
            DropDbStmt s; s.name=upper(consume()); return s;
        }
        if(obj=="TABLE"){
            DropTableStmt s; s.table=upper(consume()); return s;
        }
        throw std::runtime_error("Unknown DROP target: "+obj);
    }

    /* ── USE ───────────────────────────────────────────────────────── */
    if(kw=="USE"){
        UseDbStmt s; s.name=upper(consume()); return s;
    }

    /* ── DESCRIBE / DESC ───────────────────────────────────────────── */
    if(kw=="DESCRIBE"||kw=="DESC"){
        DescribeStmt s; s.table=upper(consume()); return s;
    }

    /* ── INSERT ────────────────────────────────────────────────────── */
    if(kw=="INSERT"){
        expect("INTO");
        InsertStmt s;
        s.table=upper(consume());
        /* optional column list */
        if(peek()=="("){
            consume();
            while(peek()!=")"){
                if(peek()==","){consume();continue;}
                s.col_names.push_back(upper(consume()));
            }
            consume();
        }
        expect("VALUES");
        /* Support multi-row INSERT: VALUES (...),(...),...  */
        /* Parse all tuples and return one InsertStmt per tuple via multi_inserts */
        std::vector<InsertStmt> extra_rows;
        bool first_tuple = true;
        while(true){
            if(!first_tuple){
                /* after first tuple, expect comma then another tuple or end */
                if(peek()!=",") break;
                consume(); /* eat comma between tuples */
                if(peek()!="(") break;
            }
            first_tuple = false;
            expect("(");
            std::vector<std::string> tuple_vals;
            while(peek()!=")"){
                if(peek()==","){consume();continue;}
                std::string val=strip_quotes(consume());
                if(val.size()==10 && val[4]=='-' && val[7]=='-' &&
                   peek().size()==8 && peek()[2]==':' && peek()[5]==':' &&
                   peek()!=")" && peek()!=",") {
                    val += " " + consume();
                }
                tuple_vals.push_back(val);
            }
            consume(); /* closing ) */
            if(s.values.empty()){
                s.values = tuple_vals; /* first tuple goes into s */
            } else {
                InsertStmt extra = s;
                extra.values = tuple_vals;
                extra.expires = s.expires;
                extra_rows.push_back(extra);
            }
        }
        /* optional EXPIRES <unix_ts> */
        if(peek()=="EXPIRES"){
            consume();
            s.expires=(time_t)std::stoll(consume());
        }
        /* Store extra rows so executor can retrieve them */
        s.extra_rows = extra_rows;
        return s;
    }

    /* ── SELECT ────────────────────────────────────────────────────── */
    if(kw=="SELECT"){
        SelectStmt s;
        if(peek()=="*"){consume();s.star=true;}
        else{
            while(!peek().empty()&&upper(peek())!="FROM"){
                if(peek()==","){consume();continue;}
                s.cols.push_back(upper(consume()));
            }
        }
        expect("FROM");
        s.table=upper(consume());

        /* INNER JOIN */
        if(peek()=="INNER"){
            consume(); expect("JOIN");
            s.join_table=upper(consume());
            expect("ON");
            s.join_left =upper(consume());
            expect("=");
            s.join_right=upper(consume());
        }
        /* WHERE */
        if(peek()=="WHERE"){
            consume();
            s.where=parse_where(toks,pos);
        }
        /* ORDER BY */
        if(peek()=="ORDER"){
            consume(); expect("BY");
            s.order_col=upper(consume());
            if(peek()=="DESC"){consume();s.order_asc=false;}
            else if(peek()=="ASC"){consume();}
        }
        /* LIMIT */
        if(peek()=="LIMIT"){
            consume();
            s.limit=std::stoi(consume());
        }
        return s;
    }

    /* ── UPDATE ────────────────────────────────────────────────────── */
    if(kw=="UPDATE"){
        UpdateStmt s;
        s.table=upper(consume());
        expect("SET");
        s.set_col=upper(consume());
        expect("=");
        s.set_val=strip_quotes(consume());
        if(peek()=="WHERE"){
            consume();
            s.where=parse_where(toks,pos);
        }
        return s;
    }

    /* ── DELETE ────────────────────────────────────────────────────── */
    if(kw=="DELETE"){
        expect("FROM");
        DeleteStmt s;
        s.table=upper(consume());
        if(peek()=="WHERE"){
            consume();
            s.where=parse_where(toks,pos);
        }
        return s;
    }

    throw std::runtime_error("Unsupported statement: "+kw);
}
