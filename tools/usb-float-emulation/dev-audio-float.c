/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic UAC2 HS sink for usbfs integration tests. No hardware provenance. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "desc.h"
#include "qom/object.h"
#include "qemu/bswap.h"

#define TYPE_FLOAT_SINK "usb-audio-float-test"
OBJECT_DECLARE_SIMPLE_TYPE(FloatSink, FLOAT_SINK)
struct FloatSink {
    USBDevice dev;
    char *capture;
    FILE *out;
    uint32_t rate;
};
#define U16(x) ((x)&255), (((x)>>8)&255)
#define U32(x) U16(x), U16((x)>>16)
static const USBDescStrings strings = {
    [1] = "Synthetic test fixture", [2] = "UAC2 PCM32 and IEEE_FLOAT32 sink",
    [3] = "SOFTWARE-ONLY-001"
};
static USBDescEndpoint endpoints[] = {
    { .bEndpointAddress=1, .bmAttributes=5, .wMaxPacketSize=104,
      .bInterval=1, .extra=(uint8_t[]){8,0x25,1,0,0,0,0,0} },
    { .bEndpointAddress=0x81, .bmAttributes=0x11,
      .wMaxPacketSize=4, .bInterval=4 },
};
#define STREAM_ALT(alt, format) { \
    .bInterfaceNumber=1, .bAlternateSetting=alt, .bNumEndpoints=2, \
    .bInterfaceClass=1, .bInterfaceSubClass=2, .bInterfaceProtocol=0x20, \
    .ndesc=2, .descs=(USBDescOther[]) { \
        {.data=(uint8_t[]){16,0x24,1,1,0,1,U32(format),2,U32(3),0}}, \
        {.data=(uint8_t[]){6,0x24,2,1,4,32}} }, .eps=endpoints }
static const USBDescIface interfaces[] = {
    { .bInterfaceNumber=0, .bInterfaceClass=1, .bInterfaceSubClass=1,
      .bInterfaceProtocol=0x20, .ndesc=4, .descs=(USBDescOther[]) {
        {.data=(uint8_t[]){9,0x24,1,U16(0x200),8,U16(46),0}},
        {.data=(uint8_t[]){8,0x24,0x0a,0x10,3,7,0,0}},
        {.data=(uint8_t[]){17,0x24,2,1,U16(0x101),0,0x10,2,U32(3),0,U16(0),0}},
        {.data=(uint8_t[]){12,0x24,3,2,U16(0x301),0,1,0x10,U16(0),0}},
      } },
    { .bInterfaceNumber=1, .bInterfaceClass=1, .bInterfaceSubClass=2,
      .bInterfaceProtocol=0x20 },
    STREAM_ALT(1, 1), /* signed PCM32 */
    STREAM_ALT(2, 4), /* IEEE_FLOAT32 */
};
static const USBDescIfaceAssoc association = {
    .bFirstInterface=0, .bInterfaceCount=2, .bFunctionClass=1,
    .bFunctionSubClass=0, .bFunctionProtocol=0x20,
    .nif=ARRAY_SIZE(interfaces), .ifs=interfaces,
};
static const USBDescDevice high = {
    .bcdUSB=0x200, .bDeviceClass=0xef, .bDeviceSubClass=2,
    .bDeviceProtocol=1, .bMaxPacketSize0=64, .bNumConfigurations=1,
    .confs=(USBDescConfig[]){{ .bNumInterfaces=2, .bConfigurationValue=1,
        .bmAttributes=0x80, .bMaxPower=50,
        .nif_groups=1, .if_groups=&association }},
};
static const USBDesc descriptor = {
    .id={.idVendor=0x46f4,.idProduct=0xf032,.bcdDevice=0x100,
         .iManufacturer=1,.iProduct=2,.iSerialNumber=3},
    .high=&high, .str=strings,
};
static void control(USBDevice *dev, USBPacket *p, int request,
                    int value, int index, int length, uint8_t *data)
{
    FloatSink *s=FLOAT_SINK(dev);
    if (usb_desc_handle_control(dev,p,request,value,index,length,data)>=0) return;
    if (index==0x1000 && value==0x100) {
        if (request==0x2101 && length==4) {
            uint32_t rate=ldl_le_p(data);
            if (rate!=44100 && rate!=48000 && rate!=96000) goto stall;
            s->rate=rate;
            return;
        }
        if (request==0xa101 && length>=4) {
            stl_le_p(data,s->rate); p->actual_length=4; return;
        }
        if (request==0xa102) {
            uint8_t ranges[]={U16(3),U32(44100),U32(44100),U32(0),
                U32(48000),U32(48000),U32(0),U32(96000),U32(96000),U32(0)};
            int n=MIN(length,sizeof(ranges));
            memcpy(data,ranges,n); p->actual_length=n; return;
        }
    }
    if (index==0x1000 && value==0x200 && request==0xa101 && length>=1) {
        data[0]=1; p->actual_length=1; return;
    }
 stall:
    p->status=USB_RET_STALL;
}
static void data_packet(USBDevice *dev, USBPacket *p)
{
    FloatSink *s=FLOAT_SINK(dev);
    if (!dev->altsetting[1]) { p->status=USB_RET_STALL; return; }
    if (p->pid==USB_TOKEN_OUT && p->ep->nr==1) {
        uint8_t bytes[104], header[12];
        size_t n=p->iov.size;
        if (n>sizeof(bytes) || n%8) { p->status=USB_RET_STALL; return; }
        usb_packet_copy(p,bytes,n);
        stl_le_p(header,s->rate); stl_le_p(header+4,dev->altsetting[1]);
        stl_le_p(header+8,n);
        if (fwrite(header,1,12,s->out)!=12 || fwrite(bytes,1,n,s->out)!=n || fflush(s->out)) {
            error_report("float-test: capture write failed"); abort();
        }
        return;
    }
    if (p->pid==USB_TOKEN_IN && p->ep->nr==1) {
        uint8_t bytes[4];
        stl_le_p(bytes,((uint64_t)s->rate*65536+4000)/8000);
        usb_packet_copy(p,bytes,MIN(p->iov.size,4)); return;
    }
    p->status=USB_RET_STALL;
}
static void realize(USBDevice *dev, Error **errp)
{
    FloatSink *s=FLOAT_SINK(dev);
    if (!s->capture || !(s->out=fopen(s->capture,"wb"))) {
        error_setg(errp,"capture must name a writable output file"); return;
    }
    s->rate=48000; dev->usb_desc=&descriptor;
    usb_desc_init(dev);
}
static void unrealize(USBDevice *dev) { fclose(FLOAT_SINK(dev)->out); }
static const Property properties[]={DEFINE_PROP_STRING("capture",FloatSink,capture)};
static void class_init(ObjectClass *klass,const void *data)
{
    DeviceClass *dc=DEVICE_CLASS(klass);
    USBDeviceClass *uc=USB_DEVICE_CLASS(klass);
    device_class_set_props(dc,properties);
    uc->product_desc="Synthetic IEEE_FLOAT32 USB sink";
    uc->realize=realize; uc->unrealize=unrealize;
    uc->handle_attach=usb_desc_attach;
    uc->handle_control=control; uc->handle_data=data_packet;
}
static const TypeInfo info={.name=TYPE_FLOAT_SINK,.parent=TYPE_USB_DEVICE,
    .instance_size=sizeof(FloatSink),.class_init=class_init};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
