#pragma once
#include "native_gpu_raster.hpp"
#include "native_memory_view.hpp"
#include "native_render_timeline.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
namespace kon {
struct NativeFrameClear {
 unsigned left=0,top=0,right=0,bottom=0,color=0,mask=0,z=0;
};
struct NativeFrameCommand {
 bool clear=false;
 std::array<std::uint32_t,32> parameters{};
 std::vector<NativeFrameClear> rectangles;
 std::shared_ptr<const std::vector<std::uint32_t>> texture;
 std::vector<NativeGpuRasterTriangle> triangles;
 std::uint32_t source_pc=0;
};
struct NativeRenderFrame {
 std::vector<std::uint8_t> initial_vram;
 std::vector<NativeFrameCommand> commands;
 std::uint32_t address=0,stride=0,format=0;
 std::uint64_t timestamp_us=0;
 bool complete=true;
 std::string rejection;
};
// Own every byte used by later rendering. No guest pointers cross a render
// boundary. At most four original destinations and 64 MiB per frame are held.
class NativeFrameStream {
public:
 explicit NativeFrameStream(psprecomp::GuestMemory& memory):memory_(memory){}
 void source(std::uint32_t pc){pc_=pc;}
 void clear(std::uint32_t address,std::uint32_t depth,unsigned stride,unsigned zstride,unsigned format,bool zwrite,std::span<const NativeFrameClear> rectangles){
  auto& frame=get(address);NativeFrameCommand command;command.clear=true;command.source_pc=pc_;
  command.parameters[7]=stride;command.parameters[8]=zstride;command.parameters[9]=format;command.parameters[11]=zwrite?16:0;command.parameters[28]=address;command.parameters[29]=depth;
  command.rectangles.assign(rectangles.begin(),rectangles.end());append(frame,std::move(command));
 }
 void raster(const std::array<std::uint32_t,32>& p,std::span<const std::uint32_t> texture,std::span<const NativeGpuRasterTriangle> jobs){
  auto& frame=get(p[28]);NativeFrameCommand command;command.parameters=p;command.source_pc=pc_;
  command.texture=intern_texture(frame,texture);command.triangles.assign(jobs.begin(),jobs.end());append(frame,std::move(command));
 }
 void reject(std::uint32_t address,const char* reason){auto& frame=get(address);frame.complete=false;frame.rejection=reason;}
 std::shared_ptr<const NativeRenderFrame> finish(std::uint32_t address,unsigned stride,unsigned format,std::uint64_t now){
  auto it=frames_.find(address);if(it==frames_.end())return {};
  auto frame=std::make_shared<NativeRenderFrame>(std::move(it->second));frames_.erase(it);bytes_.erase(address);textures_.erase(address);
  frame->address=address;frame->stride=stride;frame->format=format;frame->timestamp_us=now;return frame;
 }
private:
 psprecomp::GuestMemory& memory_;std::uint32_t pc_=0;
 struct TextureRecord {std::uintptr_t source;std::shared_ptr<const std::vector<std::uint32_t>> pixels;};
 std::map<std::uint32_t,std::vector<TextureRecord>> textures_;
 std::shared_ptr<const std::vector<std::uint32_t>> intern_texture(NativeRenderFrame& frame,std::span<const std::uint32_t> pixels){
  if(pixels.empty() || !frame.complete)return {};
  auto& pool=textures_[frame.address];const auto address=reinterpret_cast<std::uintptr_t>(pixels.data());
  const auto same=[&](const TextureRecord& entry){return entry.pixels->size()==pixels.size() && bytes_equal(entry.pixels->data(),pixels.data(),pixels.size_bytes());};
  for(const auto& entry:pool)if(entry.source==address && same(entry))return entry.pixels;
  for(const auto& entry:pool)if(same(entry))return entry.pixels;
  auto& bytes=bytes_.at(frame.address);if(bytes+pixels.size_bytes()>64u*1024u*1024u){frame.complete=false;frame.rejection="Immutable textures exceed capture budget";return {};}
  auto owned=std::make_shared<const std::vector<std::uint32_t>>(pixels.begin(),pixels.end());pool.push_back({address,owned});bytes+=pixels.size_bytes();return owned;
 }
 std::map<std::uint32_t,NativeRenderFrame> frames_;std::map<std::uint32_t,std::size_t> bytes_;
 NativeRenderFrame& get(std::uint32_t address){
  if(!frames_.contains(address)){
   if(frames_.size()>=4)throw std::runtime_error("Too many immutable render destinations");
   auto& frame=frames_[address];frame.address=address;const auto& vram=memory_.vram_bytes();frame.initial_vram.assign(vram.begin(),vram.end());bytes_[address]=vram.size();
  }
  return frames_.at(address);
 }
 void append(NativeRenderFrame& frame,NativeFrameCommand command){
  if(!frame.complete)return;auto& bytes=bytes_.at(frame.address);
  const auto next=bytes+command.triangles.size()*sizeof(NativeGpuRasterTriangle)+command.rectangles.size()*sizeof(NativeFrameClear)+sizeof(command);
  if(next>64u*1024u*1024u || frame.commands.size()>=4096){frame.complete=false;frame.rejection="Immutable frame exceeds capture budget";return;}
  bytes=next;frame.commands.push_back(std::move(command));
 }
};
// Replays projected geometry into privately owned VRAM. Original simulation,
// tasks, audio, controllers and scoring are not invoked or modified.
inline std::vector<std::uint8_t> replay_native_frame(const NativeRenderFrame& frame,NativeGpuRaster& gpu){
 if(!frame.complete || frame.initial_vram.size()!=psprecomp::GuestMemory::kVramSize)throw std::runtime_error("Cannot replay an incomplete native frame");
 // Replays touch VRAM only: reuse one private memory per thread instead of
 // allocating and zeroing a full guest address space for every frame.
 thread_local psprecomp::GuestMemory memory;memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,frame.initial_vram);
 bool batch=false;std::array<unsigned,5> identity{};std::vector<std::uint32_t> colors,depths;
 std::size_t known_colors=0,known_depths=0; // words the open batch already owns
 const auto commit=[&]{if(!batch)return;batch=false;known_colors=known_depths=0;const auto result=gpu.end_batch();if(!result.draws)return;const auto& p=result.parameters;
  NativeMemoryView color(memory,p[28],result.color_span().size()*(p[9]==3?4u:2u),true),depth(memory,p[29],result.depth_span().size()*2,true);color.write_words(result.color_span(),p[9]==3?4u:2u);depth.write_words(result.depth_span(),2);
 };
 for(const auto& command:frame.commands){const auto& p=command.parameters;const auto bytes=p[9]==3?4u:2u;
  if(command.clear){commit();
   // One validated view per rectangle instead of checked per-pixel accesses.
   for(const auto& r:command.rectangles){if(r.right<=r.left || r.bottom<=r.top)continue;const auto rows=std::size_t(r.bottom-r.top);
    if(r.mask){NativeMemoryView color(memory,p[28]+r.top*p[7]*bytes,((rows-1)*p[7]+r.right)*bytes,true);
     for(std::size_t y=0;y<rows;++y)for(unsigned x=r.left;x<r.right;++x){const auto at=(y*p[7]+x)*bytes;
      if(bytes==4)color.store32(at,(color.load32(at)&~r.mask)|(r.color&r.mask));else color.store16(at,static_cast<std::uint16_t>((color.load16(at)&~r.mask)|(r.color&r.mask)));}}
    if(p[11]&16){NativeMemoryView depth(memory,p[29]+r.top*p[8]*2,((rows-1)*p[8]+r.right)*2,true);
     for(std::size_t y=0;y<rows;++y)for(unsigned x=r.left;x<r.right;++x)depth.store16((y*p[8]+x)*2,static_cast<std::uint16_t>(r.z));}
   }continue;}
  if(command.triangles.empty())continue;
  const std::array<unsigned,5> next{p[7],p[8],p[9],p[28],p[29]};if(batch && identity!=next)commit();if(!batch){gpu.begin_batch();batch=true;identity=next;}
  const auto count=std::size_t(p[24]-1)*p[7]+p[23],zcount=(p[11]&8)?std::size_t(p[24]-1)*p[8]+p[23]:0;
  NativeMemoryView color(memory,p[28],count*bytes),depth(memory,p[29],zcount*2);color.read_words(colors,bytes,known_colors);depth.read_words(depths,2,known_depths);
  const auto texels=command.texture?std::span<const std::uint32_t>(*command.texture):std::span<const std::uint32_t>{};
  NativeGpuRasterStats result;
  try{result=gpu.raster(colors,depths,texels,command.triangles,p);}
  catch(const NativeGpuBatchFull&){commit();gpu.begin_batch();batch=true;identity=next;color.read_words(colors,bytes);depth.read_words(depths,2);result=gpu.raster(colors,depths,texels,command.triangles,p);}
  if(!result.deferred){color.write_words(colors,bytes);depth.write_words(depths,2);}
  else{known_colors=std::max(known_colors,colors.size());known_depths=std::max(known_depths,depths.size());}
 }
 commit();const auto output=memory.vram_bytes();return {output.begin(),output.end()};
}
// Binary snapshot of a complete frame plus the original VRAM after it, for
// offline replay benchmarks and exactness checks. Textures are deduplicated.
inline void save_native_frame(const std::filesystem::path& path,const NativeRenderFrame& frame,std::span<const std::uint8_t> expected_vram){
 std::ofstream out(path,std::ios::binary);const auto u32=[&](std::uint32_t v){out.write(reinterpret_cast<const char*>(&v),4);};
 const auto bytes=[&](const void* p,std::size_t n){out.write(static_cast<const char*>(p),static_cast<std::streamsize>(n));};
 bytes("KONFRAM1",8);u32(frame.address);u32(frame.stride);u32(frame.format);bytes(&frame.timestamp_us,8);
 u32(std::uint32_t(frame.initial_vram.size()));bytes(frame.initial_vram.data(),frame.initial_vram.size());u32(std::uint32_t(expected_vram.size()));bytes(expected_vram.data(),expected_vram.size());
 std::vector<const std::vector<std::uint32_t>*> textures;for(const auto& c:frame.commands)if(c.texture && std::find(textures.begin(),textures.end(),c.texture.get())==textures.end())textures.push_back(c.texture.get());
 u32(std::uint32_t(textures.size()));for(const auto* t:textures){u32(std::uint32_t(t->size()));bytes(t->data(),t->size()*4);}
 u32(std::uint32_t(frame.commands.size()));
 for(const auto& c:frame.commands){u32(c.clear);bytes(c.parameters.data(),128);u32(std::uint32_t(c.rectangles.size()));bytes(c.rectangles.data(),c.rectangles.size()*sizeof(NativeFrameClear));
  u32(c.texture?std::uint32_t(std::find(textures.begin(),textures.end(),c.texture.get())-textures.begin()):~0u);
  u32(std::uint32_t(c.triangles.size()));bytes(c.triangles.data(),c.triangles.size()*sizeof(NativeGpuRasterTriangle));u32(c.source_pc);}
 if(!out)throw std::runtime_error("Cannot save native frame snapshot");
}
inline NativeRenderFrame load_native_frame(const std::filesystem::path& path,std::vector<std::uint8_t>& expected_vram){
 std::ifstream in(path,std::ios::binary);const auto bytes=[&](void* p,std::size_t n){in.read(static_cast<char*>(p),static_cast<std::streamsize>(n));if(!in)throw std::runtime_error("Truncated native frame snapshot");};
 const auto u32=[&]{std::uint32_t v;bytes(&v,4);return v;};char magic[8];bytes(magic,8);if(std::memcmp(magic,"KONFRAM1",8))throw std::runtime_error("Not a native frame snapshot");
 NativeRenderFrame frame;frame.address=u32();frame.stride=u32();frame.format=u32();bytes(&frame.timestamp_us,8);
 frame.initial_vram.resize(u32());bytes(frame.initial_vram.data(),frame.initial_vram.size());expected_vram.resize(u32());bytes(expected_vram.data(),expected_vram.size());
 std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> textures(u32());
 for(auto& t:textures){std::vector<std::uint32_t> pixels(u32());bytes(pixels.data(),pixels.size()*4);t=std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels));}
 frame.commands.resize(u32());
 for(auto& c:frame.commands){c.clear=u32()!=0;bytes(c.parameters.data(),128);c.rectangles.resize(u32());bytes(c.rectangles.data(),c.rectangles.size()*sizeof(NativeFrameClear));
  const auto texture=u32();if(texture!=~0u)c.texture=textures.at(texture);c.triangles.resize(u32());bytes(c.triangles.data(),c.triangles.size()*sizeof(NativeGpuRasterTriangle));c.source_pc=u32();}
 return frame;
}
// Interpolate only packets whose command, resources and topology still match.
// Scene/resource transitions retain current geometry instead of pairing guesses.
inline NativeRenderFrame interpolate_native_frame(const NativeRenderFrame& previous,const NativeRenderFrame& current,double alpha){
 auto frame=current;if(previous.commands.size()!=current.commands.size())return frame;
 for(std::size_t n=0;n<frame.commands.size();++n){const auto& a=previous.commands[n];auto& b=frame.commands[n];
  if(a.clear || b.clear || a.source_pc!=b.source_pc || (a.texture!=b.texture && (!a.texture || !b.texture || *a.texture!=*b.texture)) || a.triangles.size()!=b.triangles.size())continue;
  bool compatible=true;for(unsigned tag=7;tag<28;++tag)if(tag<21 || tag>24)compatible=compatible && a.parameters[tag]==b.parameters[tag];
  for(std::size_t k=0;k<b.triangles.size() && compatible;++k)compatible=(a.triangles[k].state[4]&7u)==(b.triangles[k].state[4]&7u) && a.triangles[k].state[7]==b.triangles[k].state[7];
  if(!compatible)continue;
  unsigned left=~0u,top=~0u,right=0,bottom=0;
  for(std::size_t k=0;k<b.triangles.size();++k){b.triangles[k]=interpolate_native_triangle(a.triangles[k],b.triangles[k],alpha);const auto& s=b.triangles[k].state;left=std::min(left,s[0]);top=std::min(top,s[1]);right=std::max(right,s[2]);bottom=std::max(bottom,s[3]);}
  if(!b.triangles.empty()){b.parameters[21]=left;b.parameters[22]=top;b.parameters[23]=right;b.parameters[24]=bottom;}
 }
 return frame;
}
}
