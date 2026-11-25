#pragma once
namespace vf2_tiles {
inline constexpr const char *vertex_shader=R"(
attribute vec2 aPosition;
attribute vec3 aQ;
attribute vec4 aTransformA,aTransformB;
uniform vec2 uSize;
uniform float uScale;
uniform highp float uSortDepth;
uniform highp vec4 uTexture;
varying VF2_VARY_PREC vec2 vUV0,vUV1;
varying mediump float vDepth;
void main(){
    highp float depth=1.0/max(aQ.x,0.000000000001);
    highp vec2 xy=aPosition*uScale/uSize;
    highp vec2 uv=aQ.yz/(aQ.x*uTexture.xy);
    gl_Position=vec4(vec2(xy.x*2.0-1.0,1.0-xy.y*2.0)*depth,uSortDepth*depth,depth);
    vUV0=uv*aTransformA.xy+aTransformA.zw;
    vUV1=uv*aTransformB.xy+aTransformB.zw;
    vDepth=depth;
}
)";
inline constexpr const char *fragment_shader=R"(
precision VF2_PREC float;
varying VF2_VARY_PREC vec2 vUV0,vUV1;
varying mediump float vDepth;
uniform VF2_PREC sampler2D uTile0,uTile1,uPalette;
uniform vec4 uColour,uOwner;
uniform vec2 uPaletteOffset;
uniform float uTextured,uTransparent,uChecker,uOwnerMode,uHeight;
uniform float uLOD,uBand,uBlendMode,uMicroMinLOD;
void main(){
    if(uChecker>0.5 && mod(floor(gl_FragCoord.x)+uHeight-1.0-floor(gl_FragCoord.y),2.0)<0.5)discard;
    vec4 colour=uColour;
    if(uTextured>0.5){
        // Keep the varying operands unmodified: Mali-400 uses its higher
        // precision interpolator-to-texture path for these two instructions.
        vec4 a=texture2D(uTile0,vUV0);
        vec4 b=texture2D(uTile1,vUV1);
        vec2 ta=vec2(floor(a.r*255.0+0.01),floor(a.a*128.0+0.01));
        vec2 tb=vec2(floor(b.r*255.0+0.01),floor(b.a*128.0+0.01));
        float lod=log2(max(vDepth,0.000001))*2.0-uLOD/128.0;
        float blend=0.0;
        if(uBlendMode>1.5)blend=clamp(-lod/(2.0*exp2(uMicroMinLOD)),0.0,127.0/256.0);
        else if(uBlendMode>0.5)blend=clamp(lod-uBand,0.0,1.0);
        vec2 tx=floor(mix(ta,tb,blend));
        if(uTransparent>0.5 && tx.y<64.0)discard;
        colour=texture2D(uPalette,vec2((tx.x+0.5)/VF2_PALETTE_X_RANGE+uPaletteOffset.x,uPaletteOffset.y));
    }
    gl_FragColor=uOwnerMode>0.5?uOwner:colour;
}
)";
}
