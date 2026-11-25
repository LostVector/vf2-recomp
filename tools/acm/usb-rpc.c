// Local USB transport for the RAM development environment. No network listener.
// FunctionFS v1 descriptors support the Mini's Linux 3.4 kernel.
#define _GNU_SOURCE
#include <linux/usb/functionfs.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct __attribute__((packed)) descriptors {
    struct usb_functionfs_descs_head header;
    struct __attribute__((packed)) speed {
        struct usb_interface_descriptor interface;
        struct usb_endpoint_descriptor_no_audio out, in;
    } fs, hs;
};
struct __attribute__((packed)) strings {
    struct usb_functionfs_strings_head header;
    uint16_t lang;
    char text[12];
};
static int control, input, output;
static int transfer(int fd, void *ptr, size_t size, int writing) {
    char *p=ptr;
    while (size) {
        size_t chunk=size>16384?16384:size;
        ssize_t n=writing?write(fd,p,chunk):read(fd,p,chunk);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        p+=n; size-=n;
    }
    return 0;
}
static int send_result(int32_t status, const void *data, uint32_t length) {
    uint32_t h[2]={htole32(status),htole32(length)};
    return transfer(output,h,sizeof(h),1) || (length && transfer(output,(void*)data,length,1));
}
static void *events(void *unused) {
    (void)unused;
    for (;;) {
        struct usb_functionfs_event e[8];
        ssize_t n=read(control,e,sizeof(e));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { usleep(50000); continue; }
        for (unsigned i=0;i<n/sizeof(*e);i++) {
            fprintf(stderr,"FunctionFS event %u\n",e[i].type);
            if (e[i].type==FUNCTIONFS_SETUP) {
                // Unsupported vendor control request: stall the data stage.
                char byte;
                if (e[i].u.setup.bRequestType&USB_DIR_IN) read(control,&byte,0);
                else write(control,&byte,0);
            }
        }
    }
    return NULL;
}
static int command(uint32_t op,char *name,uint32_t length) {
    if (op==1) { // Execute a command and return its combined output/status.
        if (length) return send_result(-EINVAL,NULL,0);
        char *cmd=NULL; if (asprintf(&cmd,"%s 2>&1",name)<0) return -1;
        FILE *pipe=popen(cmd,"r"); free(cmd);
        if (!pipe) return send_result(-errno,NULL,0);
        size_t cap=16384,used=0; char *data=malloc(cap);
        if (!data) { pclose(pipe); return -1; }
        for (;;) {
            if (used==cap) {
                if (cap>=16*1024*1024) break;
                cap*=2; char *next=realloc(data,cap);
                if (!next) { free(data); pclose(pipe); return -1; } data=next;
            }
            size_t n=fread(data+used,1,cap-used,pipe); used+=n;
            if (!n) break;
        }
        int status=pclose(pipe);
        int rc=send_result(WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status),data,used);
        free(data); return rc;
    }
    // File transfer is limited to the RAM development filesystem.
    if (strncmp(name,"/tmp/",5) && strncmp(name,"/run/",5)) return send_result(-EPERM,NULL,0);
    if (strstr(name,"..")) return send_result(-EINVAL,NULL,0);
    if (op==4) return send_result(chmod(name,length&0777)?-errno:0,NULL,0);
    if (op==2) {
        FILE *file=fopen(name,"wb");
        if (!file) return send_result(-errno,NULL,0);
        char data[16384]; uint32_t left=length; int rc=0;
        while (left) {
            uint32_t n=left>sizeof(data)?sizeof(data):left;
            if (transfer(input,data,n,0)) { rc=-EIO; break; }
            if (fwrite(data,1,n,file)!=n) rc=-EIO;
            left-=n;
        }
        if (fclose(file)) rc=-EIO;
        return send_result(rc,NULL,0);
    }
    if (op==3) {
        FILE *file=fopen(name,"rb");
        if (!file) return send_result(-errno,NULL,0);
        struct stat st; fstat(fileno(file),&st);
        if (st.st_size>64*1024*1024) { fclose(file); return send_result(-EFBIG,NULL,0); }
        uint32_t h[2]={0,htole32(st.st_size)};
        int rc=transfer(output,h,sizeof(h),1);
        char data[16384]; size_t n;
        while (!rc && (n=fread(data,1,sizeof(data),file))) rc=transfer(output,data,n,1);
        fclose(file); return rc;
    }
    return send_result(-EINVAL,NULL,0);
}
int main(int argc,char **argv) {
    const char *base=argc>1?argv[1]:"/dev/usb-ffs/acm";
    char path[256]; snprintf(path,sizeof(path),"%s/ep0",base);
    control=open(path,O_RDWR); if(control<0) { perror(path); return 1; }
    struct descriptors d={0};
    d.header.magic=htole32(FUNCTIONFS_DESCRIPTORS_MAGIC);
    d.header.length=htole32(sizeof(d));d.header.fs_count=htole32(3);d.header.hs_count=htole32(3);
    d.fs.interface=(struct usb_interface_descriptor){.bLength=USB_DT_INTERFACE_SIZE,.bDescriptorType=USB_DT_INTERFACE,.bNumEndpoints=2,.bInterfaceClass=0xff,.iInterface=1};
    d.fs.out=(struct usb_endpoint_descriptor_no_audio){.bLength=USB_DT_ENDPOINT_SIZE,.bDescriptorType=USB_DT_ENDPOINT,.bEndpointAddress=1,.bmAttributes=USB_ENDPOINT_XFER_BULK,.wMaxPacketSize=htole16(64)};
    d.fs.in=d.fs.out;d.fs.in.bEndpointAddress=0x82;
    d.hs=d.fs; d.hs.out.wMaxPacketSize=d.hs.in.wMaxPacketSize=htole16(512);
    struct strings s={.header={htole32(FUNCTIONFS_STRINGS_MAGIC),htole32(sizeof(s)),htole32(1),htole32(1)},.lang=htole16(0x0409),.text="ACM VF2 RPC"};
    if (write(control,&d,sizeof(d))!=sizeof(d) || write(control,&s,sizeof(s))!=sizeof(s)) {perror("FunctionFS descriptors");return 1;}
    pthread_t thread;pthread_create(&thread,NULL,events,NULL);
    snprintf(path,sizeof(path),"%s/ep1",base);input=open(path,O_RDONLY);
    snprintf(path,sizeof(path),"%s/ep2",base);output=open(path,O_WRONLY);
    if(input<0||output<0) {perror("FunctionFS bulk endpoints");return 1;}
    fprintf(stderr,"ACM USB RPC ready\n");
    for (;;) {
        uint32_t h[4];
        if (transfer(input,h,sizeof(h),0)) {usleep(50000);continue;}
        uint32_t magic=le32toh(h[0]),op=le32toh(h[1]),namesize=le32toh(h[2]),length=le32toh(h[3]);
        if (magic!=0x314d4341 || namesize>4096 || length>64*1024*1024) {fprintf(stderr,"Invalid RPC header\n");return 1;}
        char name[4097];
        if (transfer(input,name,namesize,0)) continue;
        name[namesize]=0;
        if (command(op,name,length)) fprintf(stderr,"RPC transfer interrupted\n");
    }
}
