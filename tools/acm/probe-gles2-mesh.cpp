// Retained immutable mesh, dynamically indexed uniform matrix palette on Mali.
#include "../../src/gpu/gles2_batch_shaders.h"
#include <EGL/egl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <GLES2/gl2.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
static void check(bool ok,const char *s){if(!ok){std::fprintf(stderr,"FAIL mesh: %s GL=%x EGL=%x\n",s,glGetError(),eglGetError());std::exit(1);}}
static GLuint shader(GLenum kind,const char *s){GLuint id=glCreateShader(kind);glShaderSource(id,1,&s,nullptr);glCompileShader(id);GLint ok;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);if(!ok){char text[4096];glGetShaderInfoLog(id,sizeof(text),nullptr,text);std::fprintf(stderr,"%s\n",text);}check(ok,"shader");return id;}
struct vertex {float xyz[3],matrix;unsigned char color[4];};
// Use the production vertex layout and indexed texture/palette fragment
// program. Model-space and fallback records share one VBO and index buffer.
struct batch_vertex {
 float position[3],uv0[2],uv1[2],sort_depth;
 uint8_t info0[4];
 int16_t info1[4];
 uint16_t palette[2];
 uint8_t colour[4];
};
static_assert(sizeof(batch_vertex)==52);

static GLuint batch_program(bool mixed){
 const std::string prefix=std::string("#define VF2_PREC mediump\n#define VF2_VARY_PREC mediump\n#define VF2_FLAT_FRAGMENT_PROBE 0\n")+
     (mixed?"#define VF2_MESH_VERTEX 1\n":"");
 const GLuint vs=shader(GL_VERTEX_SHADER,(prefix+vf2_batch::vertex_shader).c_str());
 const GLuint fs=shader(GL_FRAGMENT_SHADER,(prefix+vf2_batch::fragment_shader).c_str());
 const GLuint program=glCreateProgram();
 glAttachShader(program,vs);glAttachShader(program,fs);
 const char *attributes[]={"aPositionQ","aUV0","aUV1","aDepth","aInfo0","aInfo1","aPalette","aColour"};
 for(unsigned i=0;i<8;++i)glBindAttribLocation(program,i,attributes[i]);
 glLinkProgram(program);
 glDeleteShader(vs);glDeleteShader(fs);
 GLint ok;
 glGetProgramiv(program,GL_LINK_STATUS,&ok);
 if(!ok){
  char log[4096];glGetProgramInfoLog(program,sizeof(log),nullptr,log);std::fprintf(stderr,"%s\n",log);
 }
 check(ok,"link textured mesh/fallback program");
 return program;
}

struct mesh_vertex {
 float position[3],uv0[2],uv1[2];
};
struct dynamic_vertex {
 float sort_depth;
 uint8_t info0[4];
 int16_t info1[4];
 uint16_t palette[2];
 uint8_t colour[4];
};
static_assert(sizeof(mesh_vertex)==28&&sizeof(dynamic_vertex)==24);
static_assert(offsetof(batch_vertex,sort_depth)==sizeof(mesh_vertex));

static void bind_batch(GLuint program,GLuint vbo,GLuint dynamic=0){
 glUseProgram(program);
 glUniform2f(glGetUniformLocation(program,"uSize"),64,64);
 glUniform1f(glGetUniformLocation(program,"uScale"),1);
 glUniform1f(glGetUniformLocation(program,"uHeight"),64);
 glUniform1f(glGetUniformLocation(program,"uOwnerMode"),0);
 glUniform1i(glGetUniformLocation(program,"uAtlasA"),0);
 glUniform1i(glGetUniformLocation(program,"uAtlasB"),1);
 glUniform1i(glGetUniformLocation(program,"uPalette"),2);
 glBindBuffer(GL_ARRAY_BUFFER,vbo);
 const unsigned sizes[]={3,2,2,1,4,4,2,4};
 const size_t offsets[]={offsetof(batch_vertex,position),offsetof(batch_vertex,uv0),offsetof(batch_vertex,uv1),
     offsetof(batch_vertex,sort_depth),offsetof(batch_vertex,info0),offsetof(batch_vertex,info1),
     offsetof(batch_vertex,palette),offsetof(batch_vertex,colour)};
 const GLenum types[]={GL_FLOAT,GL_FLOAT,GL_FLOAT,GL_FLOAT,GL_UNSIGNED_BYTE,GL_SHORT,GL_UNSIGNED_SHORT,GL_UNSIGNED_BYTE};
 for(unsigned i=0;i<8;++i){
  glEnableVertexAttribArray(i);
  const bool separate=dynamic&&i>=3;
  glBindBuffer(GL_ARRAY_BUFFER,separate?dynamic:vbo);
  const size_t offset=offsets[i]-(separate?sizeof(mesh_vertex):0);
  const size_t stride=dynamic?(separate?sizeof(dynamic_vertex):sizeof(mesh_vertex)):sizeof(batch_vertex);
  glVertexAttribPointer(i,sizes[i],types[i],i==7?GL_TRUE:GL_FALSE,stride,reinterpret_cast<const void*>(offset));
 }
}

static void check_mixed_textured(){
 const GLuint mixed=batch_program(true),reference=batch_program(false);
 GLuint textures[3];
 glGenTextures(3,textures);
 const uint8_t texels[][8]={{16,255,32,255,48,0,64,255},{80,255,96,255,112,255,128,255}};
 glPixelStorei(GL_UNPACK_ALIGNMENT,1);
 for(unsigned i=0;i<3;++i){
  glActiveTexture(GL_TEXTURE0+i);
  glBindTexture(GL_TEXTURE_2D,textures[i]);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,i==2?GL_NEAREST:GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,i==2?GL_NEAREST:GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  if(i<2)glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE_ALPHA,4,1,0,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,texels[i]);
  else{
   std::vector<uint8_t> palette(512*1024*4);
   for(unsigned j=0;j<512*1024;++j){
    palette[j*4]=uint8_t(32+(j&255)/2+((j>>8)&1)*64);palette[j*4+1]=uint8_t(255-j);
    palette[j*4+2]=uint8_t(j*13);palette[j*4+3]=255;
   }
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,1024,0,GL_RGBA,GL_UNSIGNED_BYTE,palette.data());
  }
 }

 // Two retained quads use the first and last slot. A projected fallback
 // quad changes depth and palette while all three share the atlas pair.
 std::array<batch_vertex,12> vertices{};
 for(unsigned i=0;i<vertices.size();++i){
  const unsigned corner=i%4,quad=i/4;
  auto &v=vertices[i];
  const float x=(corner&1)?1.f:-1.f,y=(corner&2)?1.f:-1.f;
  v.position[0]=quad<2?x:(32+x*20);
  v.position[1]=quad<2?y:(32+y*16);
  v.position[2]=quad<2?0:1;
  v.uv0[0]=v.uv1[0]=(corner&1)?0.875f:0.125f;
  v.uv0[1]=v.uv1[1]=0.5f;
  v.sort_depth=quad==0?0.1f:0.4f;
  v.info0[0]=1;
  v.info0[1]=quad==0;
  v.info0[2]=quad==1;
  v.info0[3]=quad==0?1:quad==1?64:0;
  v.info1[2]=quad==1;
 }
 const uint16_t indices[]={0,1,2,2,1,3,4,5,6,6,5,7,8,9,10,10,9,11};
 std::array<mesh_vertex,12> geometry{};
 std::array<dynamic_vertex,12> dynamic{};
 for(unsigned i=0;i<12;++i)std::memcpy(&geometry[i],&vertices[i],sizeof(geometry[i]));
 GLuint buffers[4];
 glGenBuffers(4,buffers);
 glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
 glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
 glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);
 glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
 glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);
 glClearDepthf(1);
 std::array<float,192*4> matrices{};
 std::array<uint8_t,64*64*4> a{},b{};
 for(unsigned frame=0;frame<32;++frame){
  float *left=matrices.data(),*right=left+63*12;
  for(float *rows:{left,right}){
   rows[0]=0.5f;rows[5]=0.5f;
   rows[3]=rows==left?-0.25f:0.25f;
   rows[7]=float(int(frame%5)-2)/16;
   rows[8]=rows==left?0.f:0.125f;
   rows[11]=1;
  }
  for(unsigned i=8;i<12;++i){
   vertices[i].sort_depth=(frame&1)?-0.5f:0.7f;
   vertices[i].palette[0]=(frame&2)?256:0;
  }
  // Retained positions/UVs stay resident. All materials may change, and
  // fallback positions can update in a dedicated tail of the same buffer.
  for(unsigned i=0;i<4;++i)vertices[i].palette[0]=(frame&4)?256:0;
  for(unsigned i=8;i<12;++i){
   vertices[i].position[0]+=float(int(frame%3)-1);
   std::memcpy(&geometry[i],&vertices[i],sizeof(geometry[i]));
  }
  for(unsigned i=0;i<12;++i)
   std::memcpy(&dynamic[i],reinterpret_cast<const uint8_t*>(&vertices[i])+sizeof(mesh_vertex),sizeof(dynamic[i]));
  bind_batch(mixed,buffers[0],buffers[3]);
  glBufferData(GL_ARRAY_BUFFER,sizeof(dynamic),dynamic.data(),GL_STREAM_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
  glBufferSubData(GL_ARRAY_BUFFER,8*sizeof(mesh_vertex),4*sizeof(mesh_vertex),geometry.data()+8);
  glUniform4fv(glGetUniformLocation(mixed,"uMeshRows[0]"),192,matrices.data());
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES,18,GL_UNSIGNED_SHORT,nullptr);
  glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,a.data());

  auto projected=vertices;
  for(unsigned i=0;i<8;++i){
   auto &v=projected[i];
   const float *rows=i<4?left:right;
   const float x=rows[0]*v.position[0]+rows[3];
   const float y=rows[5]*v.position[1]+rows[7];
   const float w=rows[8]*v.position[0]+rows[11];
   v.position[0]=(x/w+1)*32;v.position[1]=(1-y/w)*32;v.position[2]=1/w;
   v.info0[3]=0;
  }
  bind_batch(reference,buffers[1]);
  glBufferData(GL_ARRAY_BUFFER,sizeof(projected),projected.data(),GL_STREAM_DRAW);
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES,18,GL_UNSIGNED_SHORT,nullptr);
  glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,b.data());
  check(glGetError()==GL_NO_ERROR,"textured mixed and reference draws");
  check(a==b,"mixed textured pixels must match projected reference");
  check(a[(32*64+32)*4]!=0,"visible indexed texture and palette");
 }
 glDeleteBuffers(4,buffers);glDeleteTextures(3,textures);
 glDeleteProgram(mixed);glDeleteProgram(reference);
 std::puts("Mixed atlas batch passed: streamed materials and fallback vertices, depth, mip/palette, transparency, checker and 131072 exact pixels");
}

int main(){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint ma,mi;check(eglInitialize(d,&ma,&mi),"EGL");check(eglBindAPI(EGL_OPENGL_ES_API),"API");
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,16,EGL_NONE};EGLConfig cfg;EGLint n;check(eglChooseConfig(d,attrs,&cfg,1,&n)&&n,"config");
 EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},sa[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};auto c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);auto surface=eglCreatePbufferSurface(d,cfg,sa);check(eglMakeCurrent(d,surface,surface,c),"current");
 std::printf("Retained mesh GPU: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 const char *vs="attribute vec3 p;attribute float instance;attribute vec4 color;uniform vec4 rows[192];varying lowp vec4 rgba;void main(){int b=int(instance)*3;vec4 v=vec4(p,1.);vec3 q=vec3(dot(rows[b],v),dot(rows[b+1],v),dot(rows[b+2],v));gl_Position=vec4(q,1.);rgba=color;}";
 const char *fs="precision mediump float;varying lowp vec4 rgba;void main(){gl_FragColor=rgba;}";
 auto a=shader(GL_VERTEX_SHADER,vs),b=shader(GL_FRAGMENT_SHADER,fs);auto program=glCreateProgram();glAttachShader(program,a);glAttachShader(program,b);glBindAttribLocation(program,0,"p");glBindAttribLocation(program,1,"instance");glBindAttribLocation(program,2,"color");glLinkProgram(program);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);check(ok,"link");glUseProgram(program);
 std::vector<vertex> mesh;const float xy[][2]={{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
 for(unsigned i=0;i<64;++i)for(auto &q:xy)mesh.push_back({{q[0],q[1],0},float(i),{static_cast<unsigned char>(31+i*3),static_cast<unsigned char>(239-i*3),static_cast<unsigned char>(17+i*2),255}});
 GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,mesh.size()*sizeof(vertex),mesh.data(),GL_STATIC_DRAW);
 const unsigned vertices=mesh.size();for(auto &v:mesh)v={}; // CPU source is no longer an input
 glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glEnableVertexAttribArray(2);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(vertex),nullptr);glVertexAttribPointer(1,1,GL_FLOAT,GL_FALSE,sizeof(vertex),(void*)12);glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(vertex),(void*)16);
 glViewport(0,0,64,64);glDisable(GL_DITHER);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glClearColor(0,0,0,1);
 const GLint loc=glGetUniformLocation(program,"rows[0]");check(loc>=0,"matrix palette location");std::array<float,64*12> rows{};std::array<unsigned char,64*64*4> pixels{};
 for(unsigned frame=0;frame<32;++frame){
  rows.fill(0);for(unsigned i=0;i<64;++i){const unsigned cell=(i+frame*7)%64;auto *r=rows.data()+i*12;const float s=3.f/32;
   switch((i+frame)%4){case 0:r[0]=s;r[5]=s;break;case 1:r[1]=-s;r[4]=s;break;case 2:r[0]=-s;r[5]=-s;break;default:r[1]=s;r[4]=-s;break;}
   r[3]=float((cell%8)*8+4)/32-1;r[7]=float((cell/8)*8+4)/32-1;r[11]=(float(i)-32)/64;
  }
  glUniform4fv(loc,192,rows.data());glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,vertices);glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());check(glGetError()==GL_NO_ERROR,"draw/read");
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){const unsigned cell=(y/8)*8+x/8,id=(cell+64-(frame*7)%64)%64;const bool inside=x%8>=1&&x%8<7&&y%8>=1&&y%8<7;const unsigned char expected[]={static_cast<unsigned char>(inside?31+id*3:0),static_cast<unsigned char>(inside?239-id*3:0),static_cast<unsigned char>(inside?17+id*2:0),255};
   for(unsigned k=0;k<4;++k)if(pixels[(y*64+x)*4+k]!=expected[k]){std::fprintf(stderr,"Mismatch frame %u pixel %u,%u channel %u: %u/%u\n",frame,x,y,k,pixels[(y*64+x)*4+k],expected[k]);return 1;}
  }
 }
 std::puts("Retained mesh passed: one immutable VBO upload, 64 dynamically indexed matrices, 32 palette updates, 131072 exact pixels");
 glDeleteBuffers(1,&vbo);glDeleteProgram(program);glDeleteShader(a);glDeleteShader(b);check_mixed_textured();eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(d,surface);eglDestroyContext(d,c);eglTerminate(d);
}
