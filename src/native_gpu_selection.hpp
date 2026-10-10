#pragma once
#include <string>
#include <string_view>
#include <cstdlib>
#include <algorithm>
namespace kon {
inline std::string native_gpu_selector(std::string requested={}){
 if(requested.empty()){const auto* env=std::getenv("KON_VULKAN_ADAPTER");requested=env?env:"auto";}
 for(auto& c:requested)if(c>='A' && c<='Z')c=char(c-'A'+'a');return requested;
}
inline bool native_gpu_matches(unsigned index,unsigned vendor,unsigned type,std::string_view device,const std::string& selector){
 if(selector.empty() || selector=="auto")return true;
 if(selector=="amd")return vendor==0x1002;
 if(selector=="nvidia")return vendor==0x10DE;
 if(selector=="intel")return vendor==0x8086;
 if(selector=="integrated")return type==1;
 if(selector=="discrete")return type==2;
 if(std::all_of(selector.begin(),selector.end(),[](char c){return c>='0' && c<='9';}))return selector==std::to_string(index);
 std::string name(device);for(auto& c:name)if(c>='A' && c<='Z')c=char(c-'A'+'a');return name.find(selector)!=std::string::npos;
}
}
