#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
// Member table of a CRI CPK archive (its TOC): which file a read at a given
// offset of the archive belongs to. Same rules as tools/cpk_archive.py (the
// @UTF tables, optionally XOR-masked; offsets relative to the smaller of the
// content and TOC offsets). Only the metadata is read.
namespace kon {
class CpkIndex {
public:
    struct Member {std::uint64_t offset=0,size=0;std::string name;};
    static std::optional<CpkIndex> load(const std::filesystem::path& path){
        try{
            std::ifstream in(path,std::ios::binary);if(!in)return std::nullopt;
            CpkIndex index;
            const auto header=packet(in,0,"CPK ");if(!header)return std::nullopt;
            const auto top=Table::parse(*header);if(!top || top->rows.size()!=1)return std::nullopt;
            const auto toc=top->number(0,"TocOffset"),content=top->number(0,"ContentOffset");
            if(!toc || !*toc)return std::nullopt;
            const auto base=content && *content?std::min(*content,*toc):*toc;
            const auto list=packet(in,*toc,"TOC ");if(!list)return std::nullopt;
            const auto table=Table::parse(*list);if(!table)return std::nullopt;
            for(std::size_t row=0;row<table->rows.size();++row){
                auto dir=table->text(row,"DirName").value_or("");if(dir=="<NULL>")dir.clear();
                const auto file=table->text(row,"FileName");const auto offset=table->number(row,"FileOffset"),size=table->number(row,"FileSize");
                if(!file || !offset || !size)continue;
                index.members_.push_back({base+*offset,*size,dir.empty()?*file:dir+"/"+*file});
            }
            std::sort(index.members_.begin(),index.members_.end(),[](const Member& a,const Member& b){return a.offset<b.offset;});
            return index;
        }catch(const std::exception&){return std::nullopt;}
    }
    // The member containing this archive offset, or null.
    const Member* at(std::uint64_t offset)const{
        auto it=std::upper_bound(members_.begin(),members_.end(),offset,[](std::uint64_t o,const Member& m){return o<m.offset;});
        if(it==members_.begin())return nullptr;--it;
        return offset<it->offset+std::max<std::uint64_t>(it->size,1)?&*it:nullptr;
    }
    std::size_t size()const{return members_.size();}
    const std::vector<Member>& members()const{return members_;}
private:
    std::vector<Member> members_;
    static std::optional<std::vector<std::uint8_t>> packet(std::ifstream& in,std::uint64_t offset,const char* magic){
        in.seekg(std::streamoff(offset));char head[16]{};if(!in.read(head,16) || std::memcmp(head,magic,4))return std::nullopt;
        std::uint64_t length=0;std::memcpy(&length,head+8,8);if(length<32 || length>128u*1024u*1024u)return std::nullopt;
        std::vector<std::uint8_t> data(length);if(!in.read(reinterpret_cast<char*>(data.data()),std::streamsize(length)))return std::nullopt;
        if(std::memcmp(data.data(),"@UTF",4)){std::uint8_t key=0x5F;for(auto& b:data){b^=key;key=std::uint8_t(key*0x15u);}}
        return data;
    }
    // @UTF table: big-endian; offsets relative to byte 8.
    struct Table {
        struct Column {std::string name;unsigned storage,type;std::uint64_t number=0;std::string text;};
        struct Value {std::uint64_t number=0;std::string text;bool is_text=false;};
        std::vector<Column> columns;std::vector<std::vector<Value>> rows;
        std::optional<std::uint64_t> number(std::size_t row,const char* name)const{
            for(std::size_t c=0;c<columns.size();++c)if(columns[c].name==name && !rows[row][c].is_text)return rows[row][c].number;return std::nullopt;}
        std::optional<std::string> text(std::size_t row,const char* name)const{
            for(std::size_t c=0;c<columns.size();++c)if(columns[c].name==name && rows[row][c].is_text)return rows[row][c].text;return std::nullopt;}
        static std::optional<Table> parse(const std::vector<std::uint8_t>& d){
            const auto be=[&](std::size_t o,unsigned n)->std::uint64_t{if(o+n>d.size())throw std::out_of_range("utf");std::uint64_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|d[o+i];return v;};
            if(d.size()<32 || std::memcmp(d.data(),"@UTF",4))return std::nullopt;
            const auto total=be(4,4)+8,row_off=be(8,4)+8,str_off=be(12,4)+8,blob_off=be(16,4)+8;const auto cols=unsigned(be(24,2)),row_size=unsigned(be(26,2));const auto count=be(28,4);
            if(total>d.size() || !(32<=row_off && row_off<=str_off && str_off<=blob_off && blob_off<=total) || cols>4096 || count>1000000)return std::nullopt;
            const auto string=[&](std::uint64_t rel){const auto start=str_off+rel;if(start>=blob_off)throw std::out_of_range("utf string");
                const auto end=std::find(d.begin()+std::ptrdiff_t(start),d.begin()+std::ptrdiff_t(blob_off),0);return std::string(d.begin()+std::ptrdiff_t(start),end);};
            const auto read=[&](unsigned type,std::size_t& at)->Value{
                static constexpr unsigned widths[]{1,1,2,2,4,4,8,8,4,8};
                if(type<10){const auto v=be(at,widths[type]);at+=widths[type];return {v,{},false};}
                if(type==10){const auto p=be(at,4);at+=4;return {0,string(p),true};}
                if(type==11){at+=8;return {};}
                throw std::out_of_range("utf type");};
            Table t;std::size_t at=32;
            for(unsigned c=0;c<cols;++c){
                const auto flags=unsigned(be(at,1));const auto name=string(be(at+1,4));at+=5;
                Column col{name,flags&0xF0u,flags&0x0Fu};
                if(col.storage==0x30u){const auto v=read(col.type,at);col.number=v.number;col.text=v.text;}
                else if(col.storage!=0x10u && col.storage!=0x50u && col.storage!=0)return std::nullopt;
                t.columns.push_back(col);
            }
            for(std::uint64_t r=0;r<count;++r){
                at=std::size_t(row_off+r*row_size);std::vector<Value> row;
                for(const auto& col:t.columns){
                    if(col.storage==0x50u)row.push_back(read(col.type,at));
                    else row.push_back({col.number,col.text,col.type==10});
                }
                t.rows.push_back(std::move(row));
            }
            return t;
        }
    };
};
}
