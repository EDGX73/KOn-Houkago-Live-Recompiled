#include "native_atrac.hpp"
#include <fstream>
#include <iostream>
void check(bool yes,const char* reason){if(!yes)throw std::runtime_error(reason);}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Root required");const std::filesystem::path root=argv[1];
    std::ifstream input(root/"work/audio_fixture/original.at3",std::ios::binary);
    std::vector<std::uint8_t> encoded((std::istreambuf_iterator<char>(input)),{});check(!encoded.empty(),"Fixture absent");
    const auto wave=kon::AtracWave::parse(encoded);
    check(wave.block_bytes==744 && wave.data_offset==100 && wave.samples==267659 && wave.first_sample==2416,"Original RIFF length/delay metadata changed");
    kon::NativeMedia media(root);media.load_module(0x302);media.reinit_atrac(0,3);
    const auto reference=media.decode_audio_file(root/"work/audio_fixture/original.at3");
    const auto raw_id=media.bind_atrac3plus(2,44100,wave.block_bytes);std::vector<std::int16_t> raw;
    for(unsigned offset=wave.data_offset;offset<encoded.size();offset+=wave.block_bytes){
        const auto frame=media.decode_atrac_packet(raw_id,std::span(encoded).subspan(offset,wave.block_bytes));raw.insert(raw.end(),frame.begin(),frame.end());
    }
    check(raw==reference.samples,"Raw packet decoding differs from complete original reference PCM");media.release_atrac(raw_id);
    for(const auto capacity:{4096u,16384u,static_cast<unsigned>(encoded.size())}){
        psprecomp::GuestMemory memory;kon::NativeAtrac atrac(memory,media);const auto buffer=0x08810000u;
        for(unsigned n=0;n<capacity;++n)memory.store8(buffer+n,encoded[n]);
        const auto id=atrac.set_data(buffer,capacity);check(!(id&0x80000000u),"Stream initialization failed");
        std::vector<std::int16_t> output;unsigned wraps=0;
        for(unsigned calls=0;calls<1000;++calls){
            const auto refill=atrac.refill(id);
            check(refill.offset+std::uint64_t(refill.bytes)<=encoded.size() && refill.address+std::uint64_t(refill.bytes)<=buffer+capacity,"Refill escaped provided buffer/file");
            if(refill.bytes){
                if(refill.address==buffer)++wraps;
                check(atrac.add(id,refill.bytes+1)==0x80630018u,"Oversized refill accepted");
                for(unsigned n=0;n<refill.bytes;++n)memory.store8(refill.address+n,encoded[refill.offset+n]);
                check(atrac.add(id,refill.bytes)==0,"Refill failed");
            }
            const auto expected=atrac.next_samples(id);const auto decoded=atrac.decode(id);
            check(decoded.result==0 && decoded.frames==expected && decoded.frames<=2048,"Frame sample count or decoder failed");
            output.insert(output.end(),decoded.pcm.begin(),decoded.pcm.end());if(decoded.finish)break;
        }
        const std::vector<std::int16_t> trimmed(reference.samples.begin()+wave.first_sample*2,reference.samples.begin()+(wave.first_sample+wave.samples)*2);
        check(output==trimmed,"Partial/circular buffering changed original PCM or sample delay/end trim");
        check(capacity>=encoded.size() || wraps>5,"Partial stream did not exercise buffer wrapping");
        check(atrac.decode(id).result==0x80630024u && atrac.next_samples(id)==0,"End of stream was not reported");
        check(atrac.reset_info(id,wave.samples).result==0x80630015u,"Seek beyond end accepted");
        for(const auto sample:{0u,1000u,50000u,100000u,267000u}){
            const auto info=atrac.reset_info(id,sample);check(!info.result,"Seek metadata failed");
            check(info.address==buffer && info.minimum<=info.bytes && info.offset+std::uint64_t(info.bytes)<=encoded.size(),"Seek refill escaped buffer/file");
            check(atrac.reset(id,sample,info.bytes+1,0)==0x80630016u,"Oversized reset accepted");
            check(atrac.reset(id,sample,info.bytes,1)==0x80630017u,"Unexpected second buffer accepted");
            if(info.minimum)check(atrac.reset(id,sample,info.minimum-1,0)==0x80630016u,"Insufficient reset history accepted");
            for(unsigned n=0;n<info.bytes;++n)memory.store8(info.address+n,encoded[info.offset+n]);
            check(atrac.reset(id,sample,info.bytes,0)==0,"Seek/reset failed");
            std::vector<std::int16_t> sought;
            for(unsigned calls=0;calls<1000;++calls){
                const auto refill=atrac.refill(id);for(unsigned n=0;n<refill.bytes;++n)memory.store8(refill.address+n,encoded[refill.offset+n]);
                if(refill.bytes)check(atrac.add(id,refill.bytes)==0,"Seek refill failed");
                const auto decoded=atrac.decode(id);check(!decoded.result,"Seek decode failed");
                sought.insert(sought.end(),decoded.pcm.begin(),decoded.pcm.end());if(decoded.finish)break;
            }
            const std::vector<std::int16_t> expected(reference.samples.begin()+(wave.first_sample+sample)*2,reference.samples.begin()+(wave.first_sample+wave.samples)*2);
            if(sought!=expected)throw std::runtime_error("Seek changed original PCM at sample "+std::to_string(sample)+" capacity "+std::to_string(capacity));
        }
        check(atrac.release(id)==0 && atrac.release(id)==0x80630005u,"Stream release lost resource identity");
    }
    // Own loop metadata around the unchanged original compressed packets.
    // This keeps the source fixture intact and gives known PCM loop boundaries.
    for(const unsigned loop_start:{0u,1000u,267000u}){
        auto looped=encoded;std::vector<std::uint8_t> smpl(68,0);
        const auto word=[](std::vector<std::uint8_t>& out,unsigned at,unsigned value){for(unsigned n=0;n<4;++n)out[at+n]=static_cast<std::uint8_t>(value>>(n*8u));};
        word(smpl,0,0x6C706D73u);word(smpl,4,60);word(smpl,36,1);
        // This original fixture has an extended (12-byte) fact chunk, whose
        // loop coordinates include the additional 184-sample codec offset.
        word(smpl,52,wave.first_sample-184u+loop_start);word(smpl,56,wave.first_sample-184u+wave.samples-1u);
        looped.insert(looped.begin()+wave.data_offset-8u,smpl.begin(),smpl.end());word(looped,4,static_cast<unsigned>(looped.size())-8u);
        const auto info=kon::AtracWave::parse(looped);
        check(info.loop_start==static_cast<int>(loop_start) && info.loop_end==static_cast<int>(wave.samples-1u),"Loop sample coordinate adjustment changed");
        for(const auto capacity:{4096u,4301u,16384u,static_cast<unsigned>(looped.size())})for(const auto repeats:{1,2,-1}){
            psprecomp::GuestMemory memory;kon::NativeAtrac atrac(memory,media);const auto buffer=0x08810000u;
            memory.copy_in(buffer,std::span(looped).first(capacity));const auto id=atrac.set_data(buffer,capacity);
            check(!(id&0x80000000u) && !atrac.set_loops(id,repeats),"Loop initialization failed");
            const unsigned turns=repeats<0?3u:static_cast<unsigned>(repeats);
            const auto total=wave.samples+turns*(wave.samples-loop_start);
            std::vector<std::int16_t> output;unsigned backward_reads=0,last_offset=0,short_ends=0;
            for(unsigned calls=0;calls<2000 && output.size()<static_cast<std::size_t>(total)*2u;++calls){
                const auto fill=atrac.refill(id);
                check(fill.address>=buffer && fill.address+std::uint64_t(fill.bytes)<=buffer+capacity && fill.offset+std::uint64_t(fill.bytes)<=looped.size(),"Loop refill escaped buffer or file");
                if(fill.bytes){backward_reads+=fill.offset<last_offset;last_offset=fill.offset;
                    // Irregular additions force packet splits and odd ring pitches.
                    const auto bytes=std::min(fill.bytes,997u);memory.copy_in(fill.address,std::span(looped).subspan(fill.offset,bytes));check(!atrac.add(id,bytes),"Loop refill rejected");}
                const auto expected=atrac.next_samples(id);const auto decoded=atrac.decode(id);
                if(decoded.result==0x80630023u)continue;
                check(!decoded.result && decoded.frames==expected && decoded.frames>0,"Loop decode or next-sample count failed");
                short_ends+=decoded.frames<2048u;output.insert(output.end(),decoded.pcm.begin(),decoded.pcm.end());
                if(decoded.finish){check(repeats>=0,"Infinite loop reported end of audio");break;}
            }
            std::vector<std::int16_t> expected(reference.samples.begin()+wave.first_sample*2u,reference.samples.begin()+(wave.first_sample+wave.samples)*2u);
            for(unsigned n=0;n<turns;++n)expected.insert(expected.end(),reference.samples.begin()+(wave.first_sample+loop_start)*2u,reference.samples.begin()+(wave.first_sample+wave.samples)*2u);
            // Infinite decoding may finish a packet beyond this bounded check.
            if(repeats<0 && output.size()>expected.size())output.resize(expected.size());
            if(output!=expected)throw std::runtime_error("Loop changed reference PCM: start="+std::to_string(loop_start)+" capacity="+std::to_string(capacity)+" repeats="+std::to_string(repeats)+" output="+std::to_string(output.size()));
            check(short_ends>=turns && (capacity>=looped.size() || backward_reads>=turns),"Loop boundary/prefetch did not execute");
            check(atrac.streams.at(id).loops==(repeats<0?-1:0),"Loop count was not retained/decremented");
            if(repeats>=0)check(atrac.decode(id).result==0x80630024u,"Finite looping did not terminate");
            else check(!atrac.set_loops(id,0),"Loop cancellation failed");
            check(!atrac.release(id),"Loop decoder was not released");
        }
    }
    std::cout<<"PASS: bit-exact original PCM, delay/end trim, circular refill, seek history, finite/infinite loops, irregular prefetch and bounds\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
