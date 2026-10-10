#pragma once
#include "ge_queue.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <span>
#include <vector>
namespace kon {
// Own column-major transform and polygon clipping. These are GE coordinates,
// not an OpenGL frustum: XY uses the GE 4096-unit range; the far depth clamps
// when depth clipping is enabled. Near clipping precedes perspective division,
// including vertices behind the eye, so discarded endpoints cannot reject XY.
struct GeometryVertex {
    std::array<double,4> clip{},color{};
    double u=0,v=0,q=1,z=0;
    int x=0,y=0;
};
// Clipping one triangle against one plane produces at most four vertices.
// Keep the complete result on the stack without allocating per primitive.
struct ClippedTriangle {
    std::array<GeometryVertex,4> vertices{};unsigned count=0;
    void push_back(const GeometryVertex& vertex){if(count==vertices.size())throw std::runtime_error("Near clipping exceeded four vertices");vertices[count++]=vertex;}
    unsigned size()const{return count;}bool empty()const{return !count;}
    GeometryVertex& front(){return vertices[0];}const GeometryVertex& front()const{return vertices[0];}
    GeometryVertex& operator[](unsigned n){return vertices[n];}
    const GeometryVertex& operator[](unsigned n)const{return vertices[n];}
    auto begin(){return vertices.begin();}auto end(){return vertices.begin()+count;}
    auto begin()const{return vertices.begin();}auto end()const{return vertices.begin()+count;}
};
class NativeGeometry {
public:
    static std::array<double,3> affine(const std::array<float,12>& m,const std::array<double,3>& p){
        std::array<double,3> result{};
        for(unsigned row=0;row<3;++row)result[row]=m[row]*p[0]+m[row+3]*p[1]+m[row+6]*p[2]+m[row+9];
        return result;
    }
    static std::array<double,4> transform(const GeState& state,const std::array<double,3>& p){
        const auto world=affine(state.world,p),eye=affine(state.view,world);
        std::array<double,4> clip{};
        for(unsigned row=0;row<4;++row)clip[row]=state.projection[row]*eye[0]+state.projection[row+4]*eye[1]+state.projection[row+8]*eye[2]+state.projection[row+12];
        // A nonfinite matrix or position (the game itself sent one) is not an
        // error on the PSP: its primitives are dropped (see finite()).
        return clip;
    }
    static bool finite(const std::array<double,4>& clip){return std::isfinite(clip[0]) && std::isfinite(clip[1]) && std::isfinite(clip[2]) && std::isfinite(clip[3]);}
    static std::array<double,3> skin(const GeState& state,const std::array<double,3>& position,std::span<const double> weights){
        if(weights.size()>8)throw std::runtime_error("Too many GE bone weights");
        std::array<double,3> result{};
        for(unsigned n=0;n<weights.size();++n){
            if(!std::isfinite(weights[n]))throw std::runtime_error("Nonfinite GE skin weight");
            std::array<float,12> bone;std::copy_n(state.bones.begin()+n*12u,12,bone.begin());const auto transformed=affine(bone,position);
            for(unsigned axis=0;axis<3;++axis)result[axis]+=transformed[axis]*weights[n];
        }
        return result;
    }
    static GeometryVertex interpolate(const GeometryVertex& a,const GeometryVertex& b,double t){
        GeometryVertex out;
        for(unsigned i=0;i<4;++i){out.clip[i]=a.clip[i]+(b.clip[i]-a.clip[i])*t;out.color[i]=a.color[i]+(b.color[i]-a.color[i])*t;}
        out.u=a.u+(b.u-a.u)*t;out.v=a.v+(b.v-a.v)*t;return out;
    }
    static ClippedTriangle clip_triangle(const GeState& state,const GeometryVertex& a,const GeometryVertex& b,const GeometryVertex& c){
        const std::array<GeometryVertex,3> input{a,b,c};
        unsigned outside_near=0,outside_far=0,behind=0;
        for(const auto& vertex:input){outside_near+=vertex.clip[2]<-vertex.clip[3];outside_far+=vertex.clip[2]>vertex.clip[3];behind+=vertex.clip[3]<=0;}
        if(behind==3 || outside_near==3 || outside_far==3)return {};
        // Without clipping, partial near/far crossings are allowed; viewport Z
        // bounds decide whether the primitive survives. W<=0 cannot project.
        if(!(state.registers[0x1C]&1u)){
            if(behind)return {};ClippedTriangle unchanged;for(const auto& v:input)unchanged.push_back(v);return unchanged;
        }
        // Most triangles lie entirely in front of the near plane. Their
        // output is already identical to input; avoid building it again.
        if(!outside_near){ClippedTriangle unchanged;for(const auto& v:input)unchanged.push_back(v);return unchanged;}
        // Sutherland-Hodgman against z+w>=0, carrying the unprojected UV/color.
        ClippedTriangle output;auto previous=input.back();double previous_distance=previous.clip[2]+previous.clip[3];
        for(const auto& current:input){
            const auto distance=current.clip[2]+current.clip[3];
            if((distance>=0)!=(previous_distance>=0)){
                auto intersection=interpolate(previous,current,previous_distance/(previous_distance-distance));
                intersection.clip[2]=-intersection.clip[3];
                output.push_back(intersection);
            }
            if(distance>=0)output.push_back(current);
            previous=current;previous_distance=distance;
        }
        return output;
    }
    static bool project(const GeState& state,GeometryVertex& vertex){
        // A zero-W point remaining exactly on the plane is not projectable.
        // Reject its primitive rather than aborting the entire game renderer.
        if(vertex.clip[3]<=0)return false;
        vertex.q=1.0/vertex.clip[3];const auto& r=state.registers;
        const auto axis=[&](unsigned n){return vertex.clip[n]*vertex.q*GeState::float24(r[0x42+n])+GeState::float24(r[0x45+n]);};
        const double x=axis(0),y=axis(1),z=axis(2);
        if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))throw std::runtime_error("Nonfinite GE viewport");
        if(x<0 || y<0 || x>=4095.96875 || y>=4095.96875)return false;
        if(!(r[0x1C]&1u) && (z<0 || z>=65536))return false;
        vertex.x=static_cast<int>(x*16.0+0.375-static_cast<double>(r[0x4C]&65535u));
        vertex.y=static_cast<int>(y*16.0+0.375-static_cast<double>(r[0x4D]&65535u));
        vertex.z=static_cast<std::uint16_t>(std::clamp(z,0.0,65535.0));return true;
    }
};
}
