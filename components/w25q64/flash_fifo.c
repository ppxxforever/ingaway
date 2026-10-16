#include "flash_fifo.h"
#include <string.h>
#include <limits.h>

/* Little-endian v1, 40-byte header: magic32, version32, id64, length32,
 * payload CRC32, header CRC32, commit32, ack32, reserved32.
 * A validated header reserves its ID before any payload is programmed. */
#define MAGIC 0x31465157u
#define FREE 0
#define RECLAIM 1
#define LIVE 2
#define QUARANTINE 3
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i=0; i<4; i++) p[i]=(uint8_t)(v>>(8*i));
}
static uint64_t getid(const uint8_t *h)
{
    return get32(h+8) | (uint64_t)get32(h+12)<<32;
}
/* IEEE CRC32, reflected polynomial, initial/final XOR 0xffffffff. */
static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n)
{
    while (n--) {
        c ^= *p++;
        for (unsigned b=0; b<8; b++) c=(c>>1)^((0u-(c&1u))&0xedb88320u);
    }
    return c;
}
static uint32_t crc(const void *p, size_t n)
{
    return ~crc_step(UINT32_MAX,p,n);
}
static int failed(flash_fifo_t *f, int rc)
{
    f->ready=false;
    return rc;
}
static bool valid_header(const uint8_t *h)
{
    return get32(h)==MAGIC && get32(h+4)==1 && getid(h)!=0 &&
           get32(h+16)!=0 && get32(h+16)<=FF_MAX_PAYLOAD &&
           crc(h,24)==get32(h+24);
}
static int payload_crc(flash_fifo_t *f, unsigned slot, const uint8_t *h)
{
    uint8_t buf[128];
    uint32_t n=get32(h+16), c=UINT32_MAX, off=slot*FF_SECTOR+40;
    while (n) {
        size_t chunk=n>sizeof(buf)?sizeof(buf):n;
        if (f->io.read(f->io.ctx,off,buf,chunk)) return FF_IO;
        c=crc_step(c,buf,chunk);
        off+=chunk;
        n-=chunk;
    }
    return ~c==get32(h+20)?FF_OK:FF_CORRUPT;
}
int ff_open(flash_fifo_t *f, const ff_io_t *io)
{
    if (!f) return FF_ARGUMENT;
    /* Snapshot permits ff_open(f, &f->io) for recovery. */
    ff_io_t copy;
    if (io) copy=*io;
    f->ready=false;
    if (!io || !copy.read || !copy.write || !copy.erase ||
        copy.size%FF_SECTOR || copy.size<2*FF_SECTOR ||
        copy.size>FF_MAX_SLOTS*FF_SECTOR) return FF_ARGUMENT;
    memset(f,0,sizeof(*f));
    f->io=copy;
    f->slots=copy.size/FF_SECTOR;
    for (unsigned s=0; s<f->slots; s++) {
        uint8_t h[40];
        if (f->io.read(f->io.ctx,s*FF_SECTOR,h,sizeof(h))) return FF_IO;
        if (!valid_header(h)) {
            /* A damaged committed, unacked header is a hard error, not a
             * silently dropped record. Unknown/torn sectors are quarantined. */
            if (get32(h+28)==0 && get32(h+32)!=0) return FF_CORRUPT;
            uint8_t buf[128];
            bool blank=true;
            for (unsigned off=0; off<FF_SECTOR; off+=sizeof(buf)) {
                if (f->io.read(f->io.ctx,s*FF_SECTOR+off,buf,sizeof(buf))) return FF_IO;
                for (unsigned j=0; j<sizeof(buf); j++)
                    if (buf[j]!=255) blank=false;
            }
            f->states[s]=blank?FREE:QUARANTINE;
            continue;
        }
        uint64_t id=getid(h);
        for (unsigned j=0; j<s; j++) if (f->ids[j]==id) return FF_CORRUPT;
        f->ids[s]=id;
        if (id>f->highest) {
            f->highest=id;
            f->next=(s+1)%f->slots;
        }
        f->states[s]=RECLAIM;
        if (get32(h+28)==0 && get32(h+32)!=0) {
            int rc=payload_crc(f,s,h);
            if (rc) return rc;
            f->states[s]=LIVE;
            ++f->pending;
        }
    }
    f->ready=true;
    return FF_OK;
}
static unsigned oldest(const flash_fifo_t *f)
{
    unsigned best=f->slots;
    for (unsigned s=0; s<f->slots; s++)
        if (f->states[s]==LIVE && (best==f->slots || f->ids[s]<f->ids[best])) best=s;
    return best;
}
int ff_append(flash_fifo_t *f, const void *data, size_t len, uint64_t *id)
{
    if (!f || !f->ready || !data || !id || !len || len>FF_MAX_PAYLOAD) return FF_ARGUMENT;
    if (f->highest==UINT64_MAX) return FF_EXHAUSTED;
    unsigned s=f->slots;
    for (unsigned n=0; n<f->slots; n++) {
        unsigned candidate=(f->next+n)%f->slots;
        /* Keep the newest reservation as a durable ID high-water mark.
         * Only erase an older acked/aborted sector, never a live record. */
        if (f->states[candidate]==FREE || (f->states[candidate]==RECLAIM &&
            f->ids[candidate]!=f->highest)) {
            s=candidate;
            break;
        }
    }
    if (s==f->slots) return FF_FULL;
    uint32_t off=s*FF_SECTOR;
    if (f->states[s]!=FREE && f->io.erase(f->io.ctx,off,FF_SECTOR)) return failed(f,FF_IO);
    uint8_t h[40];
    memset(h,255,sizeof(h));
    put32(h,MAGIC);
    put32(h+4,1);
    uint64_t seq=f->highest+1;
    put32(h+8,(uint32_t)seq);
    put32(h+12,(uint32_t)(seq>>32));
    put32(h+16,(uint32_t)len);
    put32(h+20,crc(data,len));
    put32(h+24,crc(h,24));
    uint8_t verify[28];
    const uint8_t zero[4]={0};
    if (f->io.write(f->io.ctx,off,h,28) ||
        f->io.read(f->io.ctx,off,verify,sizeof(verify)) ||
        memcmp(h,verify,sizeof(verify)) ||
        f->io.write(f->io.ctx,off+40,data,len)) return failed(f,FF_IO);
    int rc=payload_crc(f,s,h);
    if (rc) return failed(f,rc);
    if (f->io.write(f->io.ctx,off+28,zero,sizeof(zero))) return failed(f,FF_IO);
    f->highest=seq;
    f->ids[s]=seq;
    f->states[s]=LIVE;
    ++f->pending;
    f->next=(s+1)%f->slots;
    *id=seq;
    return FF_OK;
}
static int read_slot(flash_fifo_t *f, unsigned s, void *data, size_t capacity,
                     size_t *len, uint64_t *id)
{
    uint8_t h[40];
    if (f->io.read(f->io.ctx,s*FF_SECTOR,h,sizeof(h))) return failed(f,FF_IO);
    if (!valid_header(h) || getid(h)!=f->ids[s] || get32(h+28)!=0 ||
        get32(h+32)==0) return failed(f,FF_CORRUPT);
    size_t n=get32(h+16);
    if (n>capacity) return FF_ARGUMENT;
    if (f->io.read(f->io.ctx,s*FF_SECTOR+40,data,n)) return failed(f,FF_IO);
    if (crc(data,n)!=get32(h+20)) return failed(f,FF_CORRUPT);
    *len=n;
    *id=f->ids[s];
    return FF_OK;
}
int ff_peek(flash_fifo_t *f, void *data, size_t capacity, size_t *len, uint64_t *id)
{
    if (!f || !f->ready || !data || !len || !id) return FF_ARGUMENT;
    unsigned s=oldest(f);
    if (s==f->slots) return FF_EMPTY;
    return read_slot(f,s,data,capacity,len,id);
}
int ff_read_after(flash_fifo_t *f, uint64_t after, void *data, size_t capacity,
                  size_t *len, uint64_t *id)
{
    if (!f || !f->ready || !data || !len || !id) return FF_ARGUMENT;
    unsigned best=f->slots;
    for (unsigned s=0; s<f->slots; ++s) {
        if (f->states[s]==LIVE && f->ids[s]>after &&
            (best==f->slots || f->ids[s]<f->ids[best])) best=s;
    }
    if (best==f->slots) return FF_EMPTY;
    return read_slot(f,best,data,capacity,len,id);
}
int ff_ack(flash_fifo_t *f, uint64_t id)
{
    if (!f || !f->ready) return FF_ARGUMENT;
    unsigned s=oldest(f);
    if (s==f->slots) return FF_EMPTY;
    if (f->ids[s]!=id) return FF_ARGUMENT;
    const uint8_t zero[4]={0};
    if (f->io.write(f->io.ctx,s*FF_SECTOR+32,zero,sizeof(zero))) return failed(f,FF_IO);
    f->states[s]=RECLAIM;
    --f->pending;
    return FF_OK;
}
