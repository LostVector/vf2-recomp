// Needed because the tiny vendor installer busybox has no insmod applet.
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
int main(int argc,char **argv){
    if(argc<2){fprintf(stderr,"usage: load-module module.ko [parameters]\n");return 2;}
    FILE *f=fopen(argv[1],"rb");if(!f){perror(argv[1]);return 1;}
    struct stat st;if(fstat(fileno(f),&st)||st.st_size<=0||st.st_size>16*1024*1024){fclose(f);return 1;}
    void *data=malloc(st.st_size);if(!data){fclose(f);return 1;}
    if(fread(data,1,st.st_size,f)!=(size_t)st.st_size){free(data);fclose(f);return 1;}fclose(f);
    int rc=syscall(__NR_init_module,data,st.st_size,argc>2?argv[2]:"");free(data);
    if(rc){if(errno==EEXIST){puts("Module already loaded");return 0;}perror("init_module");return 1;}
    puts("Module loaded");return 0;
}
