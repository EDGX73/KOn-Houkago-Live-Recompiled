#include "native_wait.hpp"
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc>2)throw std::runtime_error("Usage: native_wait_test [report.json]");
    kon::NativeWait timer;timer.wait_for(0);
    bool rejected=false;try{timer.wait_for(60000001);}catch(const std::invalid_argument&){rejected=true;}
    if(!rejected)throw std::runtime_error("Scheduler wait limit was not enforced");
    std::vector<double> ordinary,precise;
    const auto measure=[](const auto& wait){const auto start=std::chrono::steady_clock::now();wait();return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();};
    for(unsigned n=0;n<64;++n){
        ordinary.push_back(measure([]{std::this_thread::sleep_for(std::chrono::milliseconds(1));}));
        const auto elapsed=measure([&]{timer.wait_for(1000);});precise.push_back(elapsed);
        if(elapsed<999)throw std::runtime_error("Relative timer returned before its due time");
    }
    std::sort(ordinary.begin(),ordinary.end());std::sort(precise.begin(),precise.end());
    if(argc==2){std::ofstream out(argv[1]);out<<std::boolalpha<<"{\"high_resolution\":"<<timer.high_resolution()<<",\"samples\":64,\"requested_microseconds\":1000,\"ordinary_sleep_median_microseconds\":"<<ordinary[32]<<",\"waitable_timer_min_microseconds\":"<<precise.front()<<",\"waitable_timer_median_microseconds\":"<<precise[32]<<",\"waitable_timer_p95_microseconds\":"<<precise[60]<<",\"game_fps_verified\":false}\n";if(!out)throw std::runtime_error("Cannot write timer report");}
    std::cout<<"PASS: native relative timer, no early release, bounded waits; high_resolution="<<timer.high_resolution()<<" ordinary 1ms wait median="<<ordinary[32]<<"us timer median="<<precise[32]<<"us; no game FPS claim\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
