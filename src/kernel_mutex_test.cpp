#include "kernel_mutex.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    kon::KernelMutexes m;
    check(m.create(5,"file system",0x100,0,2)==0,"Create priority mutex");
    check(!m.acquire(5,2,1).blocked,"First owner lock");
    check(m.acquire(5,3,1).blocked,"Competing thread must wait");
    check(m.release(5,3,1)==0x800201C5u,"Wrong thread unlock");
    check(m.acquire(5,2,1).error==0x800201C8u,"Recursive lock rejected");
    check(m.release(5,2,1)==0,"Owner unlock");
    check(m.acquire(5,3,1).error==0 && m.objects.at(5).owner==3,"Ownership transfer");
    check(m.create(6,"recursive",0x200,1,2)==0,"Recursive creation");
    check(m.acquire(6,2,3).error==0,"Recursive count");
    check(m.release(6,2,5)==0x800201C7u,"Count underflow rejected");
    check(m.release(6,2,4)==0 && m.objects.at(6).owner==-1,"Recursive final release");
    check(m.create(7,"overflow",0x200,std::numeric_limits<std::int32_t>::max(),2)==0,"Max count");
    check(m.acquire(7,2,1).error==0x800201C6u,"Overflow rejected");
    check(m.acquire(8,2,1).error==0x800201C3u,"Unknown mutex");
    check(m.create(8,"invalid",0,2,2)==0x800201BDu,"Nonrecursive initial count");
    std::cout<<"PASS: ownership, contention, recursion and count errors\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
