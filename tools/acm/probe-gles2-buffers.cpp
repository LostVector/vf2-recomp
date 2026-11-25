// Finite GLES2 buffer capability and upload-cost probe.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <time.h>
static void check(bool ok,const char *s){if(!ok){std::fprintf(stderr,"FAIL: %s GL=%x EGL=%x\n",s,glGetError(),eglGetError());std::exit(1);}}
static double cpu_ms(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return double(t.tv_sec)*1000+double(t.tv_nsec)/1e6;}
static GLuint shader(GLenum type,const char *s){GLuint v=glCreateShader(type);glShaderSource(v,1,&s,nullptr);glCompileShader(v);GLint ok;glGetShaderiv(v,GL_COMPILE_STATUS,&ok);check(ok,"shader");return v;}
struct vertex{float x,y,r,g,b;};
int main(){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint ma,mi;check(eglInitialize(d,&ma,&mi),"EGL initialize");check(eglBindAPI(EGL_OPENGL_ES_API),"EGL API");
 EGLint a[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};EGLConfig cfg;EGLint n;check(eglChooseConfig(d,a,&cfg,1,&n)&&n,"config");
 EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},sa[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};auto c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);auto s=eglCreatePbufferSurface(d,cfg,sa);check(eglMakeCurrent(d,s,s,c),"context");
 const char *ext=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));auto map=reinterpret_cast<PFNGLMAPBUFFEROESPROC>(eglGetProcAddress("glMapBufferOES"));auto unmap=reinterpret_cast<PFNGLUNMAPBUFFEROESPROC>(eglGetProcAddress("glUnmapBufferOES"));bool supported=ext&&std::strstr(ext,"GL_OES_mapbuffer")&&map&&unmap;
 std::printf("GPU: %s / %s\nOES_mapbuffer: %d; EXT_map_buffer_range: %d\n",glGetString(GL_RENDERER),glGetString(GL_VERSION),supported,ext&&std::strstr(ext,"GL_EXT_map_buffer_range")!=nullptr);
 GLint uniform_vectors=0,vertex_samplers=0,attributes=0;glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS,&uniform_vectors);glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS,&vertex_samplers);glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,&attributes);
 std::printf("Vertex capabilities: %d uniform vec4s, %d texture samplers, %d attributes\n",uniform_vectors,vertex_samplers,attributes);
 GLuint vs=shader(GL_VERTEX_SHADER,"attribute vec2 xy;attribute vec3 rgb;varying mediump vec3 c;void main(){gl_Position=vec4(xy,0.,1.);c=rgb;}"),fs=shader(GL_FRAGMENT_SHADER,"precision mediump float;varying mediump vec3 c;void main(){gl_FragColor=vec4(c,1.);}");GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glBindAttribLocation(program,0,"xy");glBindAttribLocation(program,1,"rgb");glLinkProgram(program);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);check(ok,"link");glUseProgram(program);glDeleteShader(vs);glDeleteShader(fs);
 std::vector<vertex> data(18000,vertex{0,0,0,0,1});const float xy[3][2]={{-1,-1},{3,-1},{-1,3}};for(unsigned i=0;i<3;++i){data[i]={xy[i][0],xy[i][1],1,0,0};data[data.size()-3+i]={xy[i][0],xy[i][1],0,1,0};}size_t bytes=data.size()*sizeof(vertex);unsigned repeats=240;
 for(unsigned mode=0;mode<(supported?3u:1u);++mode){
  GLuint buffers[3];glGenBuffers(3,buffers);for(auto b:buffers){glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,bytes,nullptr,GL_STREAM_DRAW);}check(glGetError()==GL_NO_ERROR,"allocate");double start=cpu_ms();
  for(unsigned i=0;i<repeats;++i){glBindBuffer(GL_ARRAY_BUFFER,buffers[mode==2?i%3:0]);if(mode==0)glBufferData(GL_ARRAY_BUFFER,bytes,data.data(),GL_STREAM_DRAW);else{if(mode==1)glBufferData(GL_ARRAY_BUFFER,bytes,nullptr,GL_STREAM_DRAW);void *p=map(GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES);check(p,"map");std::memcpy(p,data.data(),bytes);check(unmap(GL_ARRAY_BUFFER),"unmap");}}
  double elapsed=cpu_ms()-start;check(glGetError()==GL_NO_ERROR,"upload");glViewport(0,0,32,32);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_DITHER);glEnable(GL_SCISSOR_TEST);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(vertex),nullptr);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(vertex),reinterpret_cast<void*>(8));glScissor(0,0,16,32);glDrawArrays(GL_TRIANGLES,0,3);glScissor(16,0,16,32);glDrawArrays(GL_TRIANGLES,data.size()-3,3);
  std::vector<unsigned char> pixels(32*32*4);glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());check(glGetError()==GL_NO_ERROR,"readback");for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){auto *p=pixels.data()+(y*32+x)*4;check(p[0]==(x<16?255:0)&&p[1]==(x<16?0:255)&&p[2]==0&&p[3]==255,"first and last vertex payload");}
  std::printf("%s: %.4f CPU ms/upload, %zu bytes, %u iterations; pixels exact\n",mode==0?"BufferData":mode==1?"orphan/map/copy":"three-buffer map/copy",elapsed/repeats,bytes,repeats);glDeleteBuffers(3,buffers);
 }
 std::puts("Upload-only timings exclude in-flight readers; game throughput must be measured separately.");glDeleteProgram(program);eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(d,s);eglDestroyContext(d,c);eglTerminate(d);
}
