#pragma once
#ifdef __ANDROID__
// The diagnostic sampling profiler uses Windows' thread suspension and
// DbgHelp. Android profiling is done with simple_perfetto/perfetto instead;
// keep the interface as a no-op so the runtime compiles.
#include <filesystem>
#include <functional>
namespace kon {
class NativeSamplingProfiler {
public:
    using Window=std::function<bool()>;
    NativeSamplingProfiler(unsigned long,Window){}
    ~NativeSamplingProfiler()=default;
    void stop(){}
    void write(const std::filesystem::path&){}
};
}
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
namespace kon {
// Diagnostic sampling profiler: a host thread suspends the target thread about
// once per millisecond, records its call stack and resumes it. The samples are
// resolved through the executable's PDB at the end. It never touches guest
// state and is only enabled by an explicit command-line option.
class NativeSamplingProfiler {
public:
    using Window=std::function<bool()>; // true while samples should be kept
    NativeSamplingProfiler(DWORD thread_id,Window window):window_(std::move(window)){
        target_=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,thread_id);
        if(!target_)return;
        worker_=std::thread([this]{
            timeBeginPeriod(1);
            while(!stop_.load(std::memory_order_relaxed)){
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(!window_())continue;
                if(SuspendThread(target_)==DWORD(-1))continue;
                // No allocation while the target is suspended: it may hold the heap lock.
                CONTEXT context{};context.ContextFlags=CONTEXT_FULL;std::array<DWORD64,32> frames{};unsigned depth=0;
                if(GetThreadContext(target_,&context))depth=walk(context,frames);
                ResumeThread(target_);
                if(depth)samples_.emplace_back(frames.begin(),frames.begin()+depth);
            }
            timeEndPeriod(1);
        });
    }
    ~NativeSamplingProfiler(){stop();if(target_)CloseHandle(target_);}
    void stop(){stop_=true;if(worker_.joinable())worker_.join();}
    void write(const std::filesystem::path& path){
        stop();const auto process=GetCurrentProcess();
        wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);const auto folder=std::filesystem::path(module).parent_path().string();
        SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_ANYTHING);const bool symbols=SymInitialize(process,folder.c_str(),TRUE);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+512]{};auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);
        std::map<DWORD64,std::string> names;
        const auto name_of=[&](DWORD64 address)->const std::string&{
            const auto found=names.find(address);if(found!=names.end())return found->second;
            symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=511;DWORD64 displacement=0;
            return names[address]=symbols && SymFromAddr(process,address,&displacement,symbol)?std::string(symbol->Name,symbol->NameLen):"<unknown>";
        };
        // Leaf functions, inclusive time (each function once per stack) and the
        // innermost frames of each sample's stack.
        std::map<std::string,std::uint64_t> leaf,inclusive,chains;
        for(const auto& stack:samples_){
            if(stack.empty())continue;++leaf[name_of(stack[0])];
            std::vector<std::string> seen;
            for(const auto address:stack){const auto& n=name_of(address);if(std::find(seen.begin(),seen.end(),n)==seen.end()){seen.push_back(n);++inclusive[n];}}
            std::string chain;for(std::size_t i=0;i<stack.size() && i<6;++i){if(i)chain+=" <- ";chain+=name_of(stack[i]);}
            ++chains[chain];
        }
        if(symbols)SymCleanup(process);
        std::ofstream out(path);out<<"samples="<<samples_.size()<<"\n";
        const auto section=[&](const char* title,const std::map<std::string,std::uint64_t>& table,std::size_t count){
            std::vector<std::pair<std::string,std::uint64_t>> sorted(table.begin(),table.end());
            std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
            out<<"\n["<<title<<"]\n";
            for(std::size_t n=0;n<std::min(sorted.size(),count);++n)out<<sorted[n].second*100.0/std::max<std::size_t>(samples_.size(),1)<<"% "<<sorted[n].second<<" "<<sorted[n].first<<"\n";
        };
        section("leaf",leaf,80);section("inclusive",inclusive,80);section("stacks",chains,60);
    }
private:
    HANDLE target_=nullptr;Window window_;std::thread worker_;std::atomic<bool> stop_{false};std::vector<std::vector<DWORD64>> samples_;
    // x64 unwind of the suspended thread through the images' unwind tables.
    static unsigned walk(CONTEXT context,std::array<DWORD64,32>& stack){
        unsigned depth=0;
        for(;depth<32 && context.Rip;){
            stack[depth++]=context.Rip;DWORD64 base=0;
            const auto entry=RtlLookupFunctionEntry(context.Rip,&base,nullptr);
            if(!entry){context.Rip=*reinterpret_cast<DWORD64*>(context.Rsp);context.Rsp+=8;continue;}
            void* handler=nullptr;DWORD64 frame=0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,entry,&context,&handler,&frame,nullptr);
        }
        return depth;
    }
};
}
#endif // __ANDROID__
