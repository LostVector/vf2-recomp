#pragma once
namespace vf2_batch {
inline constexpr const char *vertex_shader=R"(
attribute vec3 aPositionQ;
attribute vec2 aUV0,aUV1;
attribute float aDepth;
attribute vec4 aInfo0,aInfo1,aColour;
attribute vec2 aPalette;
uniform vec2 uSize;
uniform vec2 uScale;
#ifdef VF2_MESH_VERTEX
// Three rows produce homogeneous clip X, Y and W. Slot zero denotes
// a conventional screen-space packet with reciprocal depth.
uniform highp vec4 uMeshRows[192];
#endif
varying VF2_VARY_PREC vec2 vUV0,vUV1;
varying mediump float vDepth;
varying mediump vec3 vInfo0;
varying mediump vec4 vInfo1,vColour;
varying mediump vec2 vPalette;
void main(){
    highp float depth=1.0/max(aPositionQ.z,0.000000000001);
    highp vec2 xy=aPositionQ.xy*uScale/uSize;
    highp vec2 clip=vec2(xy.x*2.0-1.0,1.0-xy.y*2.0)*depth;
#ifdef VF2_MESH_VERTEX
    if(aInfo0.w>0.5){
        int at=(int(aInfo0.w)-1)*3;
        highp vec4 p=vec4(aPositionQ,1.0);
        clip=vec2(dot(uMeshRows[at],p),dot(uMeshRows[at+1],p));
        depth=dot(uMeshRows[at+2],p);
    }
#endif
    gl_Position=vec4(clip,aDepth*depth,depth);
    vUV0=aUV0;vUV1=aUV1;
    vDepth=depth;vColour=aColour;
#ifdef VF2_PROBE_COMPACT_META
    highp float flags=floor(aPalette.x/4096.0);
    highp float mip=aInfo0.x,palette=mod(aPalette.x,4096.0);
    vInfo0=vec3(mod(flags,2.0),mod(floor(flags/2.0),2.0),floor(flags/4.0));
    vInfo1=vec4(aInfo1.x,mod(mip,16.0)-1.0,mod(floor(mip/16.0),4.0),floor(mip/64.0));
    vPalette=vec2(floor(palette/1024.0)*256.0/VF2_PALETTE_X_RANGE,(mod(palette,1024.0)+0.5)/1024.0);
#else
    vInfo0=aInfo0.xyz;vInfo1=aInfo1;
    vPalette=vec2(aPalette.x/VF2_PALETTE_X_RANGE,(aPalette.y+0.5)/1024.0);
#endif
}
)";
inline constexpr const char *fragment_shader=R"(
precision VF2_PREC float;
varying VF2_VARY_PREC vec2 vUV0,vUV1;
varying mediump float vDepth;
varying mediump vec3 vInfo0;
varying mediump vec4 vInfo1,vColour;
varying mediump vec2 vPalette;
uniform VF2_PREC sampler2D uAtlasA,uAtlasB,uPalette;
uniform float uHeight,uOwnerMode;
void main(){
#if VF2_FLAT_FRAGMENT_PROBE
    // Diagnostic only: remove texture, palette and discard work while keeping
    // the CPU's packet construction, uploads and draw submission intact.
    gl_FragColor=vColour;
#else
#if VF2_BATCH_MODE == 0
    if(vInfo0.z>0.5&&mod(floor(gl_FragCoord.x)+uHeight-1.0-floor(gl_FragCoord.y),2.0)<0.5)discard;
    vec4 colour=vColour;
    if(vInfo0.x>0.5){
#endif
        // Direct interpolator-to-sampler operands preserve Mali-400 precision.
        vec4 a=texture2D(uAtlasA,vUV0);
#if VF2_BATCH_MODE == 2
        float index=floor(a.r*255.0+0.01);
#elif VF2_BATCH_MODE == 1
        float index=floor(a.r*255.0+0.01);
        if(vInfo1.z>0.5){
            float nextIndex=floor(texture2D(uAtlasB,vUV1).r*255.0+0.01);
            float lod=log2(max(vDepth,0.000001))*2.0-vInfo1.x/128.0;
            float blend=vInfo1.z>1.5?clamp(-lod/(2.0*exp2(vInfo1.w)),0.0,127.0/256.0):clamp(lod-vInfo1.y,0.0,1.0);
            index=floor(mix(index,nextIndex,blend));
        }
#else
        vec2 ta=vec2(floor(a.r*255.0+0.01),floor(a.a*128.0+0.01));
        vec2 tx=ta;
        if(vInfo1.z>0.5){
            vec4 b=texture2D(uAtlasB,vUV1);
            vec2 tb=vec2(floor(b.r*255.0+0.01),floor(b.a*128.0+0.01));
            float lod=log2(max(vDepth,0.000001))*2.0-vInfo1.x/128.0;
            float blend=vInfo1.z>1.5?clamp(-lod/(2.0*exp2(vInfo1.w)),0.0,127.0/256.0):clamp(lod-vInfo1.y,0.0,1.0);
            tx=floor(mix(ta,tb,blend));
        }
        if(vInfo0.y>0.5&&tx.y<64.0)discard;
        if(uOwnerMode<0.5)colour=texture2D(uPalette,vec2((tx.x+0.5)/VF2_PALETTE_X_RANGE+vPalette.x,vPalette.y));
    }
    gl_FragColor=colour;
#endif
#if VF2_BATCH_MODE != 0
    gl_FragColor=texture2D(uPalette,vec2((index+0.5)/VF2_PALETTE_X_RANGE+vPalette.x,vPalette.y));
#endif
#endif
}
)";
// Textured batches that may contain transparent materials need the alpha
// discard but not the untextured/checker branches of the general shader.
// Emitting this as its own program keeps the fetch chain identical while
// removing the per-fragment vInfo0.x test, which is what made the
// homogeneous modes cheaper. Pixel-exact by construction: it is the
// general shader's textured path verbatim.
inline constexpr const char *fragment_shader_textured=R"(
precision VF2_PREC float;
varying VF2_VARY_PREC vec2 vUV0,vUV1;
varying mediump float vDepth;
varying mediump vec3 vInfo0;
varying mediump vec4 vInfo1,vColour;
varying mediump vec2 vPalette;
uniform VF2_PREC sampler2D uAtlasA,uAtlasB,uPalette;
uniform float uHeight,uOwnerMode;
void main(){
    vec4 colour=vColour;
    vec4 a=texture2D(uAtlasA,vUV0);
    vec2 ta=vec2(floor(a.r*255.0+0.01),floor(a.a*128.0+0.01));
    vec2 tx=ta;
    if(vInfo1.z>0.5){
        vec4 b=texture2D(uAtlasB,vUV1);
        vec2 tb=vec2(floor(b.r*255.0+0.01),floor(b.a*128.0+0.01));
        float lod=log2(max(vDepth,0.000001))*2.0-vInfo1.x/128.0;
        float blend=vInfo1.z>1.5?clamp(-lod/(2.0*exp2(vInfo1.w)),0.0,127.0/256.0):clamp(lod-vInfo1.y,0.0,1.0);
        tx=floor(mix(ta,tb,blend));
    }
    if(vInfo0.y>0.5&&tx.y<64.0)discard;
    if(uOwnerMode<0.5)colour=texture2D(uPalette,vec2((tx.x+0.5)/VF2_PALETTE_X_RANGE+vPalette.x,vPalette.y));
    gl_FragColor=colour;
}
)";
}
