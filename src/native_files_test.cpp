#include "native_files.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int main(int argc,char** argv){try {
    if(argc!=2)throw std::runtime_error("Project root required");
    const std::filesystem::path root=argv[1];
    for(const auto language:{"ja","en"}) {
        kon::ContentMount mount(root,language);kon::NativeFiles files(mount,root/"work/io_fixture");
        const auto fd=files.open("disc0:/PSP_GAME/USRDIR/std.cpk",0x4001u);
        require(fd==3,"First descriptor should be 3");std::array<std::uint8_t,16> bytes{};
        require(files.read(fd,bytes)==16 && bytes[0]=='C' && bytes[1]=='P' && bytes[2]=='K',"Real language archive header not read");
        const auto size=files.size(fd);require(size>0x100000,"Wrong archive size");
        require(files.seek(fd,-4,2)==size-4,"64-bit end-relative seek failed");
        require(files.read(fd,bytes)==4 && files.read(fd,bytes)==0,"EOF partial read incorrect");
        require(files.seek(fd,0,0)==0 && files.read(fd,bytes)==16,"EOF recovery failed");
        require(files.seek(fd,-1,0)<0,"Negative seek should fail");
        require(files.write(fd,bytes)==kon::NativeFiles::bad_fd,"Disc file writable");
        require(files.close(fd)==0 && files.close(fd)==kon::NativeFiles::bad_fd,"Descriptor close errors incorrect");
        require(files.open("disc0:/PSP_GAME/USRDIR/absent.native-test",1)==0x80010002u,"Missing file did not return error");
        bool denied=false;try{files.open("ms0:/../../escape",1);}catch(const std::invalid_argument&){denied=true;}
        require(denied,"Storage traversal permitted");
        std::filesystem::create_directories(files.storage());
        const auto saved=files.open("ms0:/port_io_test.bin",0x603u);require(saved==3,"Cannot create port fixture");
        const std::array<std::uint8_t,5> payload{0,1,128,254,255};
        require(files.write(saved,payload)==5 && files.sync(saved)==0,"Persisted write failed");
        require(files.close(saved)==0,"Write close failed");
        const auto reopened=files.open("ms0:/port_io_test.bin",1);std::array<std::uint8_t,5> readback{};
        require(files.read(reopened,readback)==5 && readback==payload,"Saved bytes changed on reopen");
        files.close(reopened);
    }
    std::cout<<"PASS: real JA/EN CPK reads, 64-bit seeks, EOF, safe paths and persisted host-file bytes\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
