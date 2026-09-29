// SPDX-License-Identifier: GPL-2.0-or-later
// Runs production USB submission against Linux usbfs and the QEMU xHCI device.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <dirent.h>
#include <string>
#include "usb-audio-output.cpp"
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#expr,errno); exit(1); } } while (0)
static int ctl(int fd,int type,int req,int value,int index,void *p,int len) {
    usbdevfs_ctrltransfer c={}; c.bRequestType=type;c.bRequest=req;
    c.wValue=value;c.wIndex=index;c.wLength=len;c.data=p;c.timeout=1000;
    return ioctl(fd,USBDEVFS_CONTROL,&c);
}
static int open_device() {
    for(int bus=1;bus<8;bus++) for(int dev=1;dev<32;dev++) {
        char path[64];snprintf(path,sizeof(path),"/dev/bus/usb/%03d/%03d",bus,dev);
        int fd=open(path,O_RDWR);if(fd<0)continue;
        unsigned char desc[18]={};
        if(ctl(fd,0x80,6,0x100,0,desc,18)==18 && desc[8]==0xf4 && desc[9]==0x46 && desc[10]==0x32 && desc[11]==0xf0) {
            printf("DEVICE %s speed=%d\n",path,ioctl(fd,USBDEVFS_GET_SPEED,0));
            CHECK(ioctl(fd,USBDEVFS_GET_SPEED,0)==3);
            unsigned char config[512]={};int n=ctl(fd,0x80,6,0x200,0,config,sizeof(config));
            CHECK(n>0);printf("DESCRIPTOR ");for(int i=0;i<n;i++)printf("%02x",config[i]);puts("");
            return fd;
        } close(fd);
    } return -1;
}
int main() {
    setbuf(stdout,nullptr);setbuf(stderr,nullptr);
    int fd=open_device();CHECK(fd>=0);
    unsigned control=0, ifnum=1;
    CHECK(ioctl(fd,USBDEVFS_CLAIMINTERFACE,&control)==0);
    CHECK(ioctl(fd,USBDEVFS_CLAIMINTERFACE,&ifnum)==0);
    const int rates[]={44100,48000,96000};
    const float pattern[]={-1.0f,-0.5f,-0.125f,0.0f,0.125f,0.5f,1.0f,1.25f,-1.25f,-0.0f};
    const int chunks[]={1,7,13,257,509};
    for(int alt: {1,2}) for(int rate: rates) {
        usbdevfs_setinterface reset={1,0};CHECK(ioctl(fd,USBDEVFS_SETINTERFACE,&reset)==0);
        unsigned char r[4]={(unsigned char)rate,(unsigned char)(rate>>8),(unsigned char)(rate>>16),(unsigned char)(rate>>24)};
        CHECK(ctl(fd,0x21,1,0x100,0x1000,r,4)==4);
        unsigned char current[4]={};CHECK(ctl(fd,0xa1,1,0x100,0x1000,current,4)==4);CHECK(!memcmp(r,current,4));
        unsigned char ranges[38]={};CHECK(ctl(fd,0xa1,2,0x100,0x1000,ranges,38)==38);CHECK(ranges[0]==3);
        jlong h=Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioCreate(nullptr,nullptr,fd,1,1,0x81,rate,2,32,104,8000,4,rate,1,alt==2?1:0);
        CHECK(h);auto *ctx=reinterpret_cast<UsbAudioContext*>(h);
        CHECK(Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioSetAltSetting(nullptr,nullptr,h,alt));
        CHECK(Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStart(nullptr,nullptr,h));
        CHECK(ctx->feedbackShift==0);CHECK(ctx->feedbackInFlight);
        const int frames=rate/5+37;
        std::vector<float> samples(frames*2);
        for(int i=0;i<frames*2;i++)samples[i]=i<10?pattern[i]:((i*37)%65521-32760)/32768.0f;
        int offset=0,chunk=0;
        while(offset<frames) {
            int n=std::min(chunks[chunk++%5],frames-offset);
            submitFloatPcm(ctx,samples.data()+offset*2,n);
            CHECK(ctx->running.load());offset+=n;
        }
        CHECK(Java_com_decent_usbaudio_UsbAudioStream_nativeFinish(nullptr,nullptr,h));
        CHECK(ctx->residualBytes==0);CHECK(ctx->urbsInFlight==0);
        CHECK(ctx->framesWritten==frames);
        printf("PASS alt=%d rate=%d frames=%d feedbackShift=%d packetsPerUrb=%d\n",alt,rate,frames,ctx->feedbackShift,ctx->packetsPerUrb);
        Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioDestroy(nullptr,nullptr,h);
    }
    CHECK(ioctl(fd,USBDEVFS_RELEASEINTERFACE,&ifnum)==0);
    CHECK(ioctl(fd,USBDEVFS_RELEASEINTERFACE,&control)==0);close(fd);
    puts("USB_FLOAT_GUEST_PASS");return 0;
}
