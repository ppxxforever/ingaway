#include "w25q64_cache.h"
#include "w25q64.h"
static flash_fifo_t fifo;
static int read_part(void *ctx, uint32_t off, void *buf, size_t n)
{ return esp_partition_read(ctx,off,buf,n)!=ESP_OK; }
static int write_part(void *ctx, uint32_t off, const void *buf, size_t n)
{ return esp_partition_write(ctx,off,buf,n)!=ESP_OK; }
static int erase_part(void *ctx, uint32_t off, size_t n)
{ return esp_partition_erase_range(ctx,off,n)!=ESP_OK; }
esp_err_t w25q64_cache_init(void)
{
    const esp_partition_t *part=w25q64_cache_partition();
    if (!part) return ESP_ERR_INVALID_STATE;
    ff_io_t io={.ctx=(void *)part,.read=read_part,.write=write_part,.erase=erase_part,.size=part->size};
    int rc=ff_open(&fifo,&io);
    return rc==FF_OK?ESP_OK:rc==FF_CORRUPT?ESP_ERR_INVALID_CRC:ESP_FAIL;
}
int w25q64_cache_append(const void *data, size_t len, uint64_t *id)
{ return ff_append(&fifo,data,len,id); }
int w25q64_cache_peek(void *data, size_t capacity, size_t *len, uint64_t *id)
{ return ff_peek(&fifo,data,capacity,len,id); }
int w25q64_cache_ack(uint64_t id) { return ff_ack(&fifo,id); }
