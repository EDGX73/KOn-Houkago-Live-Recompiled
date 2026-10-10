#include "native_vram_ownership.hpp"
#include <array>
#include <thread>
#include <iostream>
#include <stdexcept>
using psprecomp::GuestMemory;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Ownership {
 GuestMemory* memory;std::uint32_t first=0x100;std::size_t bytes=16,calls=0;bool pending=true,fail=false;
 static bool access(void* argument,std::uint32_t address,std::size_t length)noexcept{
  auto& self=*static_cast<Ownership*>(argument);const auto offset=address&(GuestMemory::kVramSize-1);
  if(!self.pending || (!length) || (length<=GuestMemory::kVramSize-offset && !(offset<self.first+self.bytes && self.first<offset+length)))return true;
  ++self.calls;if(self.fail)return false;
  self.pending=false;try{kon::NativeVramInternalScope internal;for(std::size_t n=0;n<self.bytes;++n)self.memory->store8(GuestMemory::kVramPhysicalBase+self.first+static_cast<std::uint32_t>(n),0xA5);}catch(...){return false;}
  return true;
 }
 void arm(){pending=true;fail=false;kon::NativeVramInternalScope internal;memory->zero(GuestMemory::kVramPhysicalBase,GuestMemory::kVramSize);}
};
int main(){try{
 GuestMemory memory,private_memory;Ownership owner{&memory};psprecomp::set_native_vram_access_hook(&memory,Ownership::access,&owner);
 const auto base=GuestMemory::kVramPhysicalBase;
 // Original AOT view, cached/uncached aliases and mirrored VRAM all synchronize.
 auto aot=memory.aot_fast_view();
 for(auto alias:{base,base+GuestMemory::kVramSize,base|0x40000000u,base|0x80000000u}){
  owner.arm();require(aot.aot_load8(alias+0x100)==0xA5,"Original AOT byte read stale");
  owner.arm();require(aot.aot_load16(alias+0x100)==0xA5A5,"Original AOT halfword read stale");
  owner.arm();require(aot.aot_load32(alias+0x100)==0xA5A5A5A5,"Original AOT word read stale");
  owner.arm();aot.aot_store8(alias+0x100,0x12);require(memory.load16(base+0x100)==0xA512,"GPU overwrote original byte store");
  owner.arm();aot.aot_store16(alias+0x100,0x3412);require(memory.load32(base+0x100)==0xA5A53412,"GPU overwrote original halfword store");
  owner.arm();aot.aot_store32(alias+0x100,0x78563412);require(memory.load32(base+0x100)==0x78563412,"GPU overwrote original word store");
 }
 std::array<std::uint8_t,32> block{};
 owner.arm();memory.copy_out(base+0xF0,block);require(!owner.pending && block[16]==0xA5,"Bulk read starting before pending range was stale");
 owner.arm();block.fill(0x77);memory.copy_in(base+0xF0,block);require(!owner.pending && memory.load8(base+0x100)==0x77,"Bulk write lost to pending GPU");
 owner.arm();memory.zero(base+0xF0,32);require(!owner.pending && memory.load8(base+0x100)==0,"Bulk zero lost to pending GPU");
 owner.arm();memory.copy_out(base+GuestMemory::kVramSize-16,std::span(block));require(!owner.pending,"Wrapped mirrored access failed conservative synchronization");
 owner.arm();memory.copy_out(base+GuestMemory::kVramSize+0xF0,block);require(!owner.pending && block[16]==0xA5,"Mirrored bulk read returned stale bytes");
 owner.arm();const auto* pointer=memory.raw_pointer(base+0xF0,32);require(pointer && !owner.pending && pointer[16]==0xA5,"Direct reader bypassed synchronization");
 owner.arm();memory.aot_copy_lz_match(GuestMemory::kPhysicalBase,base+0xF0,32);require(!owner.pending && memory.load8(GuestMemory::kPhysicalBase+16)==0xA5,"AOT bulk read was stale");
 owner.arm();(void)private_memory.load8(base+0x100);aot.aot_store32(GuestMemory::kPhysicalBase,123);require(owner.pending,"Private memory or main RAM invoked GPU ownership");
 {kon::NativeVramInternalScope outer;{kon::NativeVramInternalScope inner;require(memory.load8(base+0x100)==0,"Internal ownership not suppressed");}require(memory.load8(base+0x100)==0,"Nested suppression lost outer scope");}
 require(memory.load8(base+0x100)==0xA5,"Suppression escaped renderer scope");
 owner.arm();bool worker_ok=false;std::thread worker([&]{worker_ok=memory.load8(base+0x100)==0;});worker.join();require(worker_ok && owner.pending,"Hook was not isolated to owner thread");
 owner.arm();owner.fail=true;require(memory.raw_pointer(base+0x100,4)==nullptr,"Failed noexcept access exposed stale VRAM");
 bool threw=false;try{(void)aot.aot_load32(base+0x100);}catch(const std::exception&){threw=true;}require(threw && memory.vram_bytes()[0x100]==0,"Failed original access continued");
 psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);
 std::cout<<"PASS: original AOT aliases, stores, full bulk ranges, direct readers, private memory, thread ownership and failure-before-access\n";return 0;
}catch(const std::exception& error){psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);std::cerr<<error.what()<<'\n';return 1;}}
