#include "native_savedata.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
void check(bool yes,const char* error){if(!yes)throw std::runtime_error(error);}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Project root required");
    const std::filesystem::path root=argv[1];kon::ContentMount content(root,"ja");
    const auto unique=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    kon::NativeFiles files(content,root/"work"/("savedata_fixture_"+std::to_string(unique)));
    psprecomp::GuestMemory memory;kon::NativeSavedata service(memory,files);
    const auto request=0x08810000u;
    memory.store32(request,1536);memory.store32(request+28,0x12345678u);
    const auto field=[&](unsigned at,const char* value){for(unsigned n=0;value[n];++n)memory.store8(request+at+n,value[n]);};
    field(60,"TESTGAME");field(76,"EMPTY");field(100,"DATA.BIN");
    check(service.get_status(0)==service.wrong_type && service.update(0)==service.wrong_type,"Unstarted dialog accepted");
    check(service.init(request,100)==0 && service.init(request,101)==service.invalid_status,"Busy request overwritten");
    check(service.get_status(200099)==1 && service.update(200099)==service.invalid_status,"Initialization timing lost");
    check(service.get_status(200100)==2 && service.shutdown(200100)==service.invalid_status,"Dialog skipped running state");
    check(service.update(200100)==0 && memory.load32(request+28)==service.no_data && service.get_status(200101)==3,"Absent slot did not report actual absence");
    check(service.shutdown(200200)==0 && service.get_status(202199)==4 && service.get_status(202200)==0,"Shutdown did not complete");
    std::filesystem::create_directories(service.path().parent_path());
    check(service.init(request,300000)==0 && service.update(500000)==0 && memory.load32(request+28)==service.file_missing,"Missing file in existing slot conflated with missing slot");
    service.shutdown(500100);service.get_status(502100);
    std::ofstream(service.path(),std::ios::binary)<<"original fixture bytes";
    check(service.init(request,600000)==0,"Existing slot initialization failed");
    bool stopped=false;try{service.update(800000);}catch(const std::runtime_error&){stopped=true;}
    check(stopped && service.get_status(800001)==2 && memory.load32(request+28)==service.file_missing,"Existing save was disguised as absent or successful");
    // A new service instance must reload bytes from storage, not an in-memory
    // shortcut. Test in its isolated fixture slot; original PSP bytes stay put.
    field(76,"NATIVE");memory.store32(request+48,1);memory.store32(request+56,1);
    constexpr unsigned data=0x08820000u,icon=0x08830000u;
    memory.store32(request+116,data);memory.store32(request+120,64);memory.store32(request+124,37);
    memory.store32(request+1412,icon);memory.store32(request+1416,8);memory.store32(request+1420,8);
    for(unsigned n=0;n<37;++n)memory.store8(data+n,static_cast<std::uint8_t>(n*7u));
    for(unsigned n=0;n<8;++n)memory.store8(icon+n,static_cast<std::uint8_t>(n+90));
    for(unsigned n=0;n<16;++n)memory.store8(request+1500+n,static_cast<std::uint8_t>(n+1));
    field(128,"Native fixture title");memory.store32(request+1516,2);
    kon::NativeSavedata writer(memory,files);
    check(writer.init(request,1000000)==0 && writer.update(1200000)==0 && writer.result()==0,"Autosave did not finish actual storage");
    check(std::filesystem::exists(writer.native_path()) && !std::filesystem::exists(writer.path()),"Native container was confused with encrypted PSP data");
    writer.shutdown(1200100);writer.get_status(1202100);
    for(unsigned n=0;n<64;++n)memory.store8(data+n,0xCC);
    for(unsigned n=0;n<8;++n)memory.store8(icon+n,0xDD);
    for(unsigned n=0;n<1284;++n)memory.store8(request+128+n,0);
    memory.store32(request+48,0);memory.store32(request+124,0);
    kon::NativeSavedata reader(memory,files);
    check(reader.init(request,2000000)==0 && reader.update(2200000)==0 && reader.result()==0,"New service did not reload saved bytes");
    check(memory.load32(request+124)==37 && memory.load8(request+128)=='N',"Reload lost length or metadata");
    for(unsigned n=0;n<37;++n)check(memory.load8(data+n)==static_cast<std::uint8_t>(n*7u),"Round-trip changed original game state");
    for(unsigned n=0;n<8;++n)check(memory.load8(icon+n)==n+90,"Round-trip lost save icon");
    check(memory.load8(data+37)==0xCC,"Reload wrote beyond the saved data length");
    reader.shutdown(2200100);reader.get_status(2202100);
    memory.store32(request+120,12);memory.store32(request+28,0x12345678u);memory.store8(data,0xCC);
    kon::NativeSavedata short_buffer(memory,files);short_buffer.init(request,3000000);stopped=false;try{short_buffer.update(3200000);}catch(const std::runtime_error&){stopped=true;}
    check(stopped && memory.load8(data)==0xCC && memory.load32(request+28)==0x12345678u,"Too-small destination caused partial reload or fabricated success");
    memory.store32(request+120,64);
    {std::fstream corrupt(writer.native_path(),std::ios::binary|std::ios::in|std::ios::out);corrupt.seekp(-1,std::ios::end);corrupt.put('!');}
    kon::NativeSavedata damaged(memory,files);damaged.init(request,4000000);stopped=false;try{damaged.update(4200000);}catch(const std::runtime_error&){stopped=true;}
    check(stopped && memory.load8(data)==0xCC && memory.load32(request+28)==0x12345678u,"Damaged native save was reported as successful or absent");
    std::cout<<"PASS: timed save lifecycle, real absence, original PSP preservation, atomic PC autosave, fresh-service reload, metadata/icons and corrupt/short-buffer rejection\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
