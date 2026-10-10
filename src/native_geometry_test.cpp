#include "native_renderer.hpp"
#include <iostream>
#include <stdexcept>
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void close(double actual,double expected,const char* message){check(std::abs(actual-expected)<0.001,message);}
int main(){try{
    psprecomp::Runtime rt;auto& mem=rt.memory();kon::NativeRenderer renderer(mem);kon::GeState s;
    constexpr unsigned vertices=0x08810000,frame=0x04000000,depth=0x04100000,texture=0x08820000;
    s.world=s.view={1,0,0,0,1,0,0,0,1,0,0,0};
    s.projection={1,0,0,0,0,1,0,0,0,0,-1.5f,-1,0,0,-2.5f,0};
    const auto floating=[&](unsigned command,float value){s.registers[command]=std::bit_cast<unsigned>(value)>>8;};
    floating(0x42,2);floating(0x43,-2);floating(0x44,-32767.5f);
    floating(0x45,2048);floating(0x46,2048);floating(0x47,32767.5f);
    floating(0x48,1);floating(0x49,1);s.registers[0x4C]=s.registers[0x4D]=2046*16;
    s.registers[0x1C]=1;s.registers[0x50]=1;s.registers[0x9D]=512;s.registers[0x9E]=0x100000;s.registers[0x9F]=512;
    s.registers[0xD2]=3;s.registers[0x16]=s.registers[0xD5]=(3u<<10)|3u;
    s.registers[0x23]=1;s.registers[0xDE]=1;s.registers[0x12]=0x19F;
    // Perspective triangle with the second vertex three times farther away.
    const std::array<std::array<float,3>,3> positions{{{-1,1,-1},{3,3,-3},{-1,-1,-1}}};
    const unsigned colors[]={0xFF0000FF,0xFF00FF00,0xFFFF0000};
    for(unsigned i=0;i<3;++i){const auto p=vertices+i*24;mem.store32(p,std::bit_cast<unsigned>(i==1?1.f:0.f));mem.store32(p+4,0);mem.store32(p+8,colors[i]);for(unsigned n=0;n<3;++n)mem.store32(p+12+n*4,std::bit_cast<unsigned>(positions[i][n]));}
    auto clip=kon::NativeGeometry::transform(s,{positions[1][0],positions[1][1],positions[1][2]});
    close(clip[0],3,"Projection matrix order incorrect");close(clip[2],2,"Projection depth incorrect");close(clip[3],3,"Projection W incorrect");
    kon::GeometryVertex projected;projected.clip=clip;check(kon::NativeGeometry::project(s,projected),"Visible point rejected");
    check(projected.x==64 && projected.y==0 && projected.z==10922,"Viewport, offsets or depth conversion incorrect");
    s.vertex_address=vertices;check(renderer.draw(s,3,3),"Native float XYZ triangle rejected");
    check(mem.load32(frame+(1*512+1)*4)==0x005F5F3Fu,"Gouraud color must interpolate in screen space");
    check(mem.load16(depth+(1*512+1)*2)==45055,"Depth must interpolate in screen space with the reversed GE range");
    s.vertex_address=vertices;s.registers[0x50]=0;check(renderer.draw(s,3,3),"Flat projected triangle rejected");
    check(mem.load32(frame+(1*512+1)*4)==0x00FF0000,"Flat color must come from provoking vertex");
    // With mip count zero, a mip filter samples only the existing base level.
    s.registers[0x1E]=1;s.registers[0xA0]=texture&0xFFFFFF;s.registers[0xA8]=0x080004;
    s.registers[0xB8]=2;s.registers[0xC3]=3;s.registers[0xC6]=4;s.registers[0xC9]=0x103;
    for(unsigned i=0;i<4;++i)mem.store32(texture+i*4,i==0?0xFF0000FF:i==1?0xFF00FF00:i==2?0xFFFF0000:0xFFFFFFFF);
    s.vertex_address=vertices;check(renderer.draw(s,3,3),"Zero-level mip filter rejected");
    check(mem.load32(frame+(1*512+1)*4)==0x000000FF,"UV must use reciprocal W; affine UV would sample green");
    s.registers[0x1E]=0;s.registers[0x17]=1;s.registers[0x53]=1;s.registers[0x5C]=0x7F7F7F;s.registers[0x5D]=255;
    s.vertex_address=vertices;check(renderer.draw(s,3,3),"Ambient-only lighting rejected");check(mem.load32(frame+(1*512+1)*4)==0x007F0000,"Ambient color product incorrect");
    const auto written=renderer.draw_pixels;s.vertex_address=vertices;s.registers[0x18]=1;
    check(!renderer.draw(s,3,3) && renderer.draw_pixels==written && s.vertex_address==vertices,"Active lighting fabricated success");
    s.registers[0x18]=0;
    // Near-plane intersections retain linear attributes before perspective division.
    kon::GeometryVertex a,b,c;a.clip={0,0,-2,1};b.clip={1,0,0,1};c.clip={0,1,0,1};a.u=0;b.u=2;c.u=4;
    auto polygon=kon::NativeGeometry::clip_triangle(s,a,b,c);check(polygon.size()==4,"Near clipping did not produce a quad");
    for(const auto& vertex:polygon)check(vertex.clip[2]+vertex.clip[3]>=-0.00001,"Near-plane clipping leaked");
    close(polygon.front().u,2,"Clip interpolation lost UV");
    s.registers[0x1C]=0;polygon=kon::NativeGeometry::clip_triangle(s,a,b,c);
    check(polygon.size()==3,"Disabled clipping incorrectly discarded a partial depth crossing before viewport");
    check(!kon::NativeGeometry::project(s,polygon.front()),"Disabled depth clamp allowed a viewport Z outside its range");
    s.registers[0x1C]=1;a.clip[2]=b.clip[2]=c.clip[2]=2;check(kon::NativeGeometry::clip_triangle(s,a,b,c).empty(),"All-far triangle was not rejected");
    // A perspective near plane at distance 1 clips away an endpoint behind
    // the camera before its infinite/negative perspective divisor is used.
    a.clip={0,0,-4,-1};b.clip={1,0,0.5,2};c.clip={0,1,0.5,2};
    a.u=0;b.u=2;c.u=4;a.color={0,0,0,0};b.color=c.color={255,255,255,255};
    polygon=kon::NativeGeometry::clip_triangle(s,a,b,c);
    check(polygon.size()==4,"One behind-camera endpoint must become two visible triangles");
    for(auto vertex:polygon){check(vertex.clip[3]>0 && vertex.clip[2]+vertex.clip[3]>=0,"Behind-camera clipping leaked");check(kon::NativeGeometry::project(s,vertex),"Clipped camera crossing did not project");}
    close(polygon.front().clip[3],1,"Near intersection perspective W incorrect");
    close(polygon.front().u,8.0/3,"Behind-camera intersection lost UV");
    close(polygon.front().color[0],170,"Behind-camera intersection lost color");
    s.registers[0x1C]=0;check(kon::NativeGeometry::clip_triangle(s,a,b,c).empty(),"Disabled clipping accepted a behind-camera endpoint");
    s.registers[0x1C]=1;b.clip={1,0,-4,-1};
    polygon=kon::NativeGeometry::clip_triangle(s,a,b,c);
    check(polygon.size()==3,"Two behind-camera endpoints must leave one triangle");
    for(auto vertex:polygon)check(kon::NativeGeometry::project(s,vertex),"Two-endpoint camera crossing did not project");
    // An outside zero-W endpoint gets removed before screen range validation.
    a.clip={1000000,0,-2,0};b.clip={0,0,0,2};c.clip={0,1,0,2};
    polygon=kon::NativeGeometry::clip_triangle(s,a,b,c);
    check(polygon.size()==4,"Zero-W outside endpoint was not clipped");
    kon::GeometryVertex zero;zero.clip={0,0,0,0};check(!kon::NativeGeometry::project(s,zero),"Unprojectable zero-W point fabricated screen coordinates");
    for(unsigned n=0;n<2;++n){s.bones[n*12]=s.bones[n*12+4]=s.bones[n*12+8]=1;}s.bones[21]=2;
    const std::array<double,2> weights{0.25,0.75};const auto skinned=kon::NativeGeometry::skin(s,{1,0,-2},weights);
    close(skinned[0],2.5,"Bone translations/weights incorrect");close(skinned[2],-2,"Bone blend lost depth");
    const std::array<double,2> partial_weights{0.25,0.25};close(kon::NativeGeometry::skin(s,{1,0,-2},partial_weights)[2],-1,"Skin weights were incorrectly renormalized");
    // Exercise actual packed vertex layouts: weights precede aligned UV/XYZ.
    s.registers[0x1E]=1;s.registers[0x55]=0xFFFFFF;s.registers[0x58]=255;
    for(unsigned weight_type=1;weight_type<=3;++weight_type){
        const unsigned uv_offset=weight_type==3?8u:4u,position_offset=uv_offset+8u,stride=position_offset+12u;
        for(unsigned n=0;n<3;++n){const auto p=vertices+n*stride;
            if(weight_type==1){mem.store8(p,64);mem.store8(p+1,64);}else if(weight_type==2){mem.store16(p,16384);mem.store16(p+2,16384);}else{mem.store32(p,std::bit_cast<unsigned>(0.5f));mem.store32(p+4,std::bit_cast<unsigned>(0.5f));}
            mem.store32(p+uv_offset,std::bit_cast<unsigned>(n==1?1.f:0.f));mem.store32(p+uv_offset+4,0);
            for(unsigned axis=0;axis<3;++axis)mem.store32(p+position_offset+axis*4,std::bit_cast<unsigned>(positions[n][axis]-(axis==0?1.f:0.f)));
        }
        s.vertex_address=vertices;s.registers[0x12]=0x4183u|(weight_type<<9u);
        check(renderer.draw(s,3,3) && s.vertex_address==vertices+stride*3u,"Weighted packed vertex stride incorrect");
        check(mem.load32(frame+(1*512+1)*4)==0x000000FF,"Weighted UV offset or bone projection incorrect");
    }
    s.world[9]=2;s.view[9]=3;clip=kon::NativeGeometry::transform(s,{1,0,-3});close(clip[0],6,"World/view affine translations not composed");
    std::cout<<"PASS: column-major transforms, weighted 8/16/float bone layouts, viewport/subpixels, interpolated depth/color, reciprocal-W UV, ambient, zero-level mip filtering and near clipping\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
