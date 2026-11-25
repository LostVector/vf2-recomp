// EGL without SDL/desktop dependencies: fbdev on the ACM, pbuffer in tests.
#pragma once
#include <EGL/egl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef __linux__
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#endif
class vf2_egl_context {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    struct { uint16_t width,height; } native_window{480,800};
public:
    int width=0,height=0;
    vf2_egl_context()=default;
    vf2_egl_context(const vf2_egl_context&)=delete;
    ~vf2_egl_context(){
        if(display!=EGL_NO_DISPLAY){
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);
            if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
            eglTerminate(display);
        }
    }
    bool open(bool headless=false){
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major,minor;
        if(!eglInitialize(display,&major,&minor)||!eglBindAPI(EGL_OPENGL_ES_API))return false;
        EGLint attributes[]={EGL_SURFACE_TYPE,headless?EGL_PBUFFER_BIT:EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config;EGLint count;
        if(!eglChooseConfig(display,attributes,&config,1,&count)||!count)return false;
        EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);if(context==EGL_NO_CONTEXT)return false;
        if(headless){
            EGLint size[]={EGL_WIDTH,496,EGL_HEIGHT,384,EGL_NONE};surface=eglCreatePbufferSurface(display,config,size);
        }else{
#ifdef __linux__
            int fb=open_fb();
            if(fb>=0){fb_var_screeninfo v{};if(ioctl(fb,FBIOGET_VSCREENINFO,&v)==0){native_window.width=v.xres;native_window.height=v.yres;}close(fb);}
#endif
            surface=eglCreateWindowSurface(display,config,reinterpret_cast<EGLNativeWindowType>(&native_window),nullptr);
        }
        if(surface==EGL_NO_SURFACE||!eglMakeCurrent(display,surface,surface,context))return false;
        eglQuerySurface(display,surface,EGL_WIDTH,&width);eglQuerySurface(display,surface,EGL_HEIGHT,&height);
        int interval=headless?0:1;
        const char *requested=std::getenv("VF2_SWAP_INTERVAL");
        if(requested)interval=std::atoi(requested)==0?0:1;
        if(!eglSwapInterval(display,interval))return false;
        if(requested){
            EGLint minimum=0,maximum=0;
            eglGetConfigAttrib(display,config,EGL_MIN_SWAP_INTERVAL,&minimum);
            eglGetConfigAttrib(display,config,EGL_MAX_SWAP_INTERVAL,&maximum);
            std::fprintf(stderr,"EGL swap interval requested %d, config range %d..%d\n",interval,minimum,maximum);
        }
        return true;
    }
    bool swap(){return eglSwapBuffers(display,surface);}
private:
#ifdef __linux__
    static int open_fb(){return ::open("/dev/fb0",O_RDWR);}
#endif
};
