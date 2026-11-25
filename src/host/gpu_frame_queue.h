// A single GL context may prepare the next frame while the previous one runs.
// Waiting on its fence before admitting another keeps at most two frames'
// command storage live. All entry points run on the context-owning thread.
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <cstdio>
#include <cstring>

class gpu_frame_queue {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLSyncKHR previous=EGL_NO_SYNC_KHR;
    PFNEGLCREATESYNCKHRPROC create=nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy=nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC wait=nullptr;
    PFNEGLGETSYNCATTRIBKHRPROC status=nullptr;
    double wait_ms=0;
    unsigned long long completed=0;
    static bool extension(const char *list,const char *name){
        if(!list)return false;
        const size_t n=std::strlen(name);
        for(const char *at=list;(at=std::strstr(at,name));at+=n)
            if((at==list||at[-1]==' ')&&(at[n]==0||at[n]==' '))return true;
        return false;
    }
public:
    gpu_frame_queue()=default;
    gpu_frame_queue(const gpu_frame_queue&)=delete;
    ~gpu_frame_queue(){if(previous!=EGL_NO_SYNC_KHR)destroy(display,previous);}
    bool open(){
        display=eglGetCurrentDisplay();
        if(display==EGL_NO_DISPLAY||!extension(eglQueryString(display,EGL_EXTENSIONS),"EGL_KHR_fence_sync")||
           !extension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),"GL_OES_EGL_sync"))return false;
        create=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroy=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        wait=reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
        status=reinterpret_cast<PFNEGLGETSYNCATTRIBKHRPROC>(eglGetProcAddress("eglGetSyncAttribKHR"));
        return create&&destroy&&wait;
    }
    int pending_status()const{
        if(previous==EGL_NO_SYNC_KHR||!status)return -1;
        EGLint value=0;if(!status(display,previous,EGL_SYNC_STATUS_KHR,&value))return -1;
        return value==EGL_SIGNALED_KHR?1:0;
    }
    bool drain(){
        if(previous==EGL_NO_SYNC_KHR)return true;
        const auto start=std::chrono::steady_clock::now();
        const EGLint result=wait(display,previous,EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,2000000000ull);
        wait_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(result!=EGL_CONDITION_SATISFIED_KHR){
            std::fprintf(stderr,"GPU fence wait failed: result %x EGL %x\n",result,eglGetError());return false;
        }
        if(!destroy(display,previous))return false;
        previous=EGL_NO_SYNC_KHR;++completed;return true;
    }
    bool end_frame(){
        if(!drain())return false;
        previous=create(display,EGL_SYNC_FENCE_KHR,nullptr);
        if(previous==EGL_NO_SYNC_KHR){std::fprintf(stderr,"GPU fence creation failed: EGL %x\n",eglGetError());return false;}
        glFlush();return true;
    }
    void report()const{
        std::fprintf(stderr,"GPU queue: %llu frames completed, %.2f ms/frame fence wait, at most two frames in flight\n",completed,completed?wait_ms/completed:0);
    }
};
