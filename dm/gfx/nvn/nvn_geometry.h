// Primitive assembly for geometry compute. Values are capture-record indices;
// restart separates runs before strips/fans are assembled. No game-specific state.
#pragma once
#include <stdint.h>
#include <stddef.h>

static int nvn_geo_topology(int prim) {
    switch (prim) {
        case 0: return 0;
        case 1: case 2: case 3: return 1;
        case 10: case 11: return 2;
        case 4: case 5: case 6: case 7: return 3;
        case 12: return 4;
        default: return -1;
    }
}
static int nvn_geo_width(int topology) { static const int width[] = {1, 2, 4, 3, 6}; return topology >= 0 && topology < 5 ? width[topology] : 0; }

// Returns words required/written. SIZE_MAX means unsupported topology or overflow.
// dst == NULL counts only. A short destination is never partially written.
static size_t nvn_geo_assemble(int prim, const uint32_t *src, size_t count, int restart, uint32_t restart_index, uint32_t *dst, size_t capacity) {
    int width = nvn_geo_width(nvn_geo_topology(prim));
    if (!width) return SIZE_MAX;
    if (dst) {
        size_t needed = nvn_geo_assemble(prim, src, count, restart, restart_index, NULL, 0);
        if (needed == SIZE_MAX || needed > capacity) return SIZE_MAX;
    }
    size_t words = 0;
    for (size_t begin = 0; begin < count;) {
        size_t end = begin;
        while (end < count && !(restart && src[end] == restart_index)) end++;
        size_t n = end - begin, np = 0;
        switch (prim) {
            case 0: np = n; break;
            case 1: np = n / 2; break;
            case 2: np = n > 1 ? n : 0; break;
            case 3: np = n > 1 ? n - 1 : 0; break;
            case 4: np = n / 3; break;
            case 5: case 6: np = n > 2 ? n - 2 : 0; break;
            case 7: np = n / 4 * 2; break;
            case 10: np = n / 4; break;
            case 11: np = n > 3 ? n - 3 : 0; break;
            case 12: np = n / 6; break;
        }
        if (np > (SIZE_MAX - words) / (size_t)width) return SIZE_MAX;
        if (dst && words + np * width > capacity) return SIZE_MAX;
        for (size_t i = 0; dst && i < np; i++) {
            size_t k[6] = {0};
            switch (prim) {
                case 0: k[0] = i; break;
                case 1: k[0] = 2*i; k[1] = 2*i+1; break;
                case 2: k[0] = i; k[1] = (i+1)%n; break;
                case 3: k[0] = i; k[1] = i+1; break;
                case 4: for (int c=0;c<3;c++) k[c]=3*i+c; break;
                case 5: k[0]=i+(i&1); k[1]=i+1-(i&1); k[2]=i+2; break;
                case 6: k[0]=0; k[1]=i+1; k[2]=i+2; break;
                case 7: k[0]=4*(i/2); k[1]=k[0]+1+(i&1); k[2]=k[1]+1; break;
                case 10: for (int c=0;c<4;c++) k[c]=4*i+c; break;
                case 11: for (int c=0;c<4;c++) k[c]=i+c; break;
                case 12: for (int c=0;c<6;c++) k[c]=6*i+c; break;
            }
            for (int c=0;c<width;c++) dst[words+i*width+c]=src[begin+k[c]];
        }
        words += np * width;
        begin = end < count ? end + 1 : end;
    }
    return words;
}

// One ordered compaction per draw. Output is a point/line/triangle list, so it
// does not depend on Metal primitive-restart behaviour. Preserve triangle-strip
// winding and the last vertex. Unwritten GS index slots are UINT_MAX.
// Large draws use three dispatches: geo_pack_count scans 64-group blocks,
// geo_pack_scan scans their totals in parallel and writes indirect arguments,
// then geo_pack_write adds local and block offsets. A draw of up to 1024 groups
// instead runs geo_pack_small, doing the ordered compaction in one workgroup.
// A single thread used to
// walk every group: with it, each frame that drew OCTOPATH TRAVELER II's 129,032-triangle cube shadow mesh
// (drawn in runs, nvn_geometry.inc geo_split) took about 300 ms; now only the first one is long.
static const char k_nvn_geo_pack_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct geo_pack { device const uint *src; device uint *dst; device uint *args; uint groups, stride, topology, capacity; device uint *cnt; };\n"
"static bool geo_room(constant geo_pack&j,uint o,uint k,uint n){return o<=j.capacity&&k<=j.capacity-o&&n<=j.capacity-o-k;}\n"
"static uint geo_group(constant geo_pack& j, uint g, bool w, uint o) {\n"
" uint a=0,b=0,n=0,k=0;\n"
" for(uint i=0;i<j.stride;i++) { uint v=j.src[g*j.stride+i];\n"
"  if(v==0xffffffffu) {n=0;continue;}\n"
"  if(j.topology==1) {if(w && geo_room(j,o,k,1)) j.dst[o+k]=v; k+=1;}\n"
"  else if(j.topology==6) {if(n) {if(w && geo_room(j,o,k,2)) {j.dst[o+k]=b;j.dst[o+k+1]=v;} k+=2;}}\n"
"  else if(n>=2) {if(w && geo_room(j,o,k,3)) {j.dst[o+k]=(n&1)?b:a;j.dst[o+k+1]=(n&1)?a:b;j.dst[o+k+2]=v;} k+=3;}\n"
"  a=b;b=v;n++;\n"
" }\n"
" return k;\n"
"}\n"
"kernel void geo_pack_count(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]], uint block [[threadgroup_position_in_grid]],\n"
" uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]], uint sw [[threads_per_simdgroup]]) {\n"
" threadgroup uint sums[64]; uint c=g<j.groups?geo_group(j,g,false,0):0u;\n"
" uint local=simd_prefix_exclusive_sum(c), total=simd_sum(c); if(sl==0) sums[si]=total;\n"
" threadgroup_barrier(mem_flags::mem_threadgroup); uint before=0; for(uint s=0;s<si;s++) before+=sums[s];\n"
" if(g<j.groups) j.cnt[g]=before+local;\n"
" if(li==0) { uint n=0; for(uint s=0;s<(64u+sw-1u)/sw;s++) n+=sums[s]; j.cnt[j.groups+block]=n; }\n"
"}\n"
// Block totals use wide arithmetic so malformed totals cannot wrap into writable output.
"kernel void geo_pack_scan_legacy(constant geo_pack& j [[buffer(0)]]) {\n"
" uint blocks=j.groups/64u+uint(j.groups%64u!=0u);ulong s=0;\n"
" for(uint b=0;b<blocks;b++){uint c=j.cnt[j.groups+b];j.cnt[j.groups+b]=uint(min(s,ulong(0xffffffffu)));s+=c;}\n"
" uint w=j.topology==1?1u:j.topology==6?2u:3u,n=s<=j.capacity?uint(s):j.capacity-j.capacity%w;\n"
" j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;\n"
"}\n"
"kernel void geo_pack_scan(constant geo_pack& j [[buffer(0)]], uint li [[thread_index_in_threadgroup]], uint3 nt [[threads_per_threadgroup]]) {\n"
" threadgroup ulong sums[256];uint blocks=j.groups/64u+uint(j.groups%64u!=0u),chunk=(blocks+nt.x-1u)/nt.x,lo=li*chunk,hi=min(lo+chunk,blocks);ulong s=0;\n"
" for(uint b=lo;b<hi;b++)s+=j.cnt[j.groups+b];sums[li]=s;threadgroup_barrier(mem_flags::mem_threadgroup);\n"
" for(uint step=1;step<nt.x;step<<=1){ulong add=li>=step?sums[li-step]:0;threadgroup_barrier(mem_flags::mem_threadgroup);sums[li]+=add;threadgroup_barrier(mem_flags::mem_threadgroup);}\n"
" s=li?sums[li-1]:0;for(uint b=lo;b<hi;b++){uint c=j.cnt[j.groups+b];j.cnt[j.groups+b]=uint(min(s,ulong(0xffffffffu)));s+=c;}\n"
" if(li==0){ulong total=sums[nt.x-1];uint w=j.topology==1?1u:j.topology==6?2u:3u,n=total<=j.capacity?uint(total):j.capacity-j.capacity%w;\n"
" j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;}\n"
"}\n"
// One workgroup can compact a small draw without global intermediate counts
// or two extra dispatches. Chunks are contiguous, preserving invocation order.
"kernel void geo_pack_small(constant geo_pack& j [[buffer(0)]], uint li [[thread_index_in_threadgroup]],\n"
" uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]], uint sw [[threads_per_simdgroup]], uint3 nt [[threads_per_threadgroup]]) {\n"
" threadgroup uint sums[128];uint chunk=(j.groups+nt.x-1u)/nt.x,lo=li*chunk,hi=min(lo+chunk,j.groups),s=0;\n"
" for(uint g=lo;g<hi;g++)s+=geo_group(j,g,false,0);uint before=simd_prefix_exclusive_sum(s),total=simd_sum(s);if(sl==0)sums[si]=total;\n"
" threadgroup_barrier(mem_flags::mem_threadgroup);for(uint b=0;b<si;b++)before+=sums[b];\n"
" for(uint g=lo;g<hi;g++)before+=geo_group(j,g,true,before);\n"
" if(li==0){uint n=0;for(uint b=0;b<(nt.x+sw-1u)/sw;b++)n+=sums[b];uint w=j.topology==1?1u:j.topology==6?2u:3u;n=n<=j.capacity?n:j.capacity-j.capacity%w;\n"
" j.args[0]=n;j.args[1]=1;j.args[2]=0;j.args[3]=0;j.args[4]=0;}\n"
"}\n"
"kernel void geo_pack_write(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]]) { if(g<j.groups){ulong o=ulong(j.cnt[g])+j.cnt[j.groups+g/64u];if(o<=0xffffffffu)geo_group(j,g,true,uint(o));} }\n";
// One word per group for local offsets, followed by one per 64-group block for block offsets.
typedef struct { uint64_t src, dst, args; uint32_t groups, stride, topology, capacity; uint64_t cnt; } nvn_geo_pack_job;
