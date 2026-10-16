#include "storage_archive.h"
#include "w25q64.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RECORD_MAX 1024
#define QUEUE_LENGTH 24
#define RECOVERY_US 5000000LL
static const char *TAG="archive";
typedef struct {
    uint64_t event_seq;
    int64_t mono_us, utc_us, sync_age_us;
    uint32_t time_epoch;
    bool utc_valid;
} stamp_t;
typedef struct {
    stamp_t stamp;
    union {
        storage_sample_t sample;
        struct {
            storage_level_t level;
            int32_t error;
            uint64_t record_id;
            char source[20], event[40], detail[96];
        } log;
    } data;
} queued_record_t;
typedef struct {
    flash_fifo_t fifo;
    const esp_partition_t *part;
    uint32_t evicted, write_errors;
    int64_t next_recovery;
    bool ready;
} archive_t;
static archive_t history, logs;
static QueueHandle_t history_queue, log_queue;
static SemaphoreHandle_t archive_mutex;
static portMUX_TYPE stamp_lock=portMUX_INITIALIZER_UNLOCKED;
static char boot_id[40];
static uint64_t next_event_seq=1;
static bool utc_valid, started;
static int64_t anchor_mono, anchor_utc;
static uint32_t time_epoch, history_dropped, logs_dropped;

static int read_part(void *ctx,uint32_t off,void *buf,size_t n)
{
    /* Large recovery scans must let the idle task/watchdog run. */
    if (off%(32*FF_SECTOR)==0) vTaskDelay(1);
    return esp_partition_read(ctx,off,buf,n)!=ESP_OK;
}
static int write_part(void *ctx,uint32_t off,const void *buf,size_t n)
{ return esp_partition_write(ctx,off,buf,n)!=ESP_OK; }
static int erase_part(void *ctx,uint32_t off,size_t n)
{ return esp_partition_erase_range(ctx,off,n)!=ESP_OK; }

static stamp_t timestamp(void)
{
    stamp_t s;
    portENTER_CRITICAL(&stamp_lock);
    s.mono_us=esp_timer_get_time();
    s.event_seq=next_event_seq==UINT64_MAX?0:next_event_seq++;
    s.utc_valid=utc_valid;
    s.time_epoch=time_epoch;
    s.sync_age_us=utc_valid?s.mono_us-anchor_mono:0;
    s.utc_us=utc_valid?anchor_utc+s.sync_age_us:0;
    portEXIT_CRITICAL(&stamp_lock);
    return s;
}
static void count_drop(bool is_history)
{
    portENTER_CRITICAL(&stamp_lock);
    if (is_history) ++history_dropped; else ++logs_dropped;
    portEXIT_CRITICAL(&stamp_lock);
}
esp_err_t storage_history_submit(const storage_sample_t *sample)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    if (!sample) return ESP_ERR_INVALID_ARG;
    queued_record_t q={.stamp=timestamp(),.data.sample=*sample};
    if (!q.stamp.event_seq) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(history_queue,&q,0)!=pdTRUE) {
        count_drop(true);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t storage_log(storage_level_t level,const char *source,const char *event,
                      int32_t error,uint64_t record_id,const char *detail)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    if (level<STORAGE_DEBUG || level>STORAGE_ERROR || !source || !event) return ESP_ERR_INVALID_ARG;
    if (level<CONFIG_STORAGE_LOG_MIN_LEVEL) return ESP_OK;
    queued_record_t q={.stamp=timestamp()};
    if (!q.stamp.event_seq) return ESP_ERR_INVALID_STATE;
    q.data.log.level=level;
    q.data.log.error=error;
    q.data.log.record_id=record_id;
    snprintf(q.data.log.source,sizeof(q.data.log.source),"%s",source);
    snprintf(q.data.log.event,sizeof(q.data.log.event),"%s",event);
    snprintf(q.data.log.detail,sizeof(q.data.log.detail),"%s",detail?detail:"");
    if (xQueueSend(log_queue,&q,0)!=pdTRUE) {
        count_drop(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void storage_clock_synced(struct timeval *tv)
{
    if (!tv || tv->tv_sec<1704067200LL || tv->tv_sec>4102444800LL ||
        tv->tv_usec<0 || tv->tv_usec>=1000000) return;
    portENTER_CRITICAL(&stamp_lock);
    anchor_mono=esp_timer_get_time();
    anchor_utc=(int64_t)tv->tv_sec*1000000+tv->tv_usec;
    utc_valid=true;
    ++time_epoch;
    uint32_t epoch=time_epoch;
    portEXIT_CRITICAL(&stamp_lock);
    storage_log(STORAGE_INFO,"time","sntp_sync",0,epoch,"new UTC anchor");
}
/* Strings are UTF-8 bytes, with JSON control characters escaped. */
static void escape(char *out,size_t cap,const char *in)
{
    static const char hex[]="0123456789abcdef";
    size_t n=0;
    for (const unsigned char *p=(const unsigned char *)in;*p;++p) {
        unsigned char c=*p;
        size_t need=c<32?6:(c=='"'||c=='\\')?2:1;
        if (n+need>=cap) break;
        if (c<32) {
            out[n++]='\\';out[n++]='u';out[n++]='0';out[n++]='0';
            out[n++]=hex[c>>4];out[n++]=hex[c&15];
        } else {
            if (need==2) out[n++]='\\';
            out[n++]=(char)c;
        }
    }
    out[n]=0;
}
static int format_record(char *out,size_t cap,const queued_record_t *q,bool is_history)
{
    char utc[32]="null",age[32]="null";
    if (q->stamp.utc_valid) {
        snprintf(utc,sizeof(utc),"%" PRId64,q->stamp.utc_us);
        snprintf(age,sizeof(age),"%" PRId64,q->stamp.sync_age_us);
    }
    int n=snprintf(out,cap,"{\"version\":1,\"boot_id\":\"%s\",\"event_seq\":%" PRIu64
        ",\"mono_us\":%" PRId64 ",\"utc_us\":%s,\"utc_valid\":%s,\"time_epoch\":%" PRIu32
        ",\"sync_age_us\":%s,",boot_id,q->stamp.event_seq,q->stamp.mono_us,utc,
        q->stamp.utc_valid?"true":"false",q->stamp.time_epoch,age);
    if (n<0 || (size_t)n>=cap) return -1;
    int tail;
    if (is_history) {
        const storage_sample_t *s=&q->data.sample;
        char value[16]="null",last[32]="null";
        if (s->has_value) {
            snprintf(value,sizeof(value),"%u",s->value);
            snprintf(last,sizeof(last),"%" PRId64,s->last_success_ms);
        }
        tail=snprintf(out+n,cap-(size_t)n,"\"kind\":\"modbus\",\"temperature_raw\":%s,"
            "\"type\":%u,\"has_value\":%s,\"stale\":%s,\"sample_mono_ms\":%s,"
            "\"state\":%" PRId32 ",\"error\":%" PRId32 "}",value,s->type,
            s->has_value?"true":"false",s->stale?"true":"false",last,s->state,s->error);
    } else {
        char source[120],event[240],detail[576];
        escape(source,sizeof(source),q->data.log.source);
        escape(event,sizeof(event),q->data.log.event);
        escape(detail,sizeof(detail),q->data.log.detail);
        tail=snprintf(out+n,cap-(size_t)n,"\"kind\":\"log\",\"level\":%d,\"source\":\"%s\","
            "\"event\":\"%s\",\"error\":%" PRId32 ",\"record_id\":\"%" PRIu64 "\",\"detail\":\"%s\"}",
            q->data.log.level,source,event,q->data.log.error,q->data.log.record_id,detail);
    }
    return tail<0 || (size_t)tail>=cap-(size_t)n?-1:n+tail;
}
static void recover(archive_t *a)
{
    if (a->ready || !a->part || esp_timer_get_time()<a->next_recovery) return;
    a->next_recovery=esp_timer_get_time()+RECOVERY_US;
    ff_io_t io={.ctx=(void *)a->part,.read=read_part,.write=write_part,
                .erase=erase_part,.size=a->part->size};
    int rc=ff_open(&a->fifo,&io);
    a->ready=rc==FF_OK;
    if (rc!=FF_OK) {
        ESP_LOGW(TAG,"%s recovery failed rc=%d; no format",a->part->label,rc);
        storage_log(STORAGE_ERROR,"archive","recovery_failed",rc,0,a->part->label);
    }
}
/* Archival retention differs from delivery cache: evict oldest when full.
 * PUBACK never touches these rings. Partial/unknown sectors remain protected. */
static int append_archive(archive_t *a,const char *data,size_t len)
{
    if (!a->ready) return FF_IO;
    uint64_t id;
    int rc=ff_append(&a->fifo,data,len,&id);
    if (rc==FF_FULL) {
        char oldest[RECORD_MAX];size_t old_len;
        rc=ff_peek(&a->fifo,oldest,sizeof(oldest),&old_len,&id);
        if (rc==FF_OK) {
            rc=ff_ack(&a->fifo,id);
            if (rc==FF_OK) {
                ++a->evicted;
                rc=ff_append(&a->fifo,data,len,&id);
            }
        }
    }
    if (rc!=FF_OK) {
        ++a->write_errors;
        a->ready=false;
        a->next_recovery=esp_timer_get_time()+RECOVERY_US;
        ESP_LOGW(TAG,"%s append failed rc=%d",a->part->label,rc);
        if (a==&history) storage_log(STORAGE_ERROR,"archive","history_write_failed",rc,0,"");
    }
    return rc;
}
static void archive_task(void *arg)
{
    (void)arg;
    queued_record_t pending_history,pending_log;
    bool have_history=false,have_log=false;
    uint32_t reported_history=0,reported_logs=0;
    int64_t next_report=0;
    char json[RECORD_MAX];
    for (;;) {
        if (!have_history) have_history=xQueueReceive(history_queue,&pending_history,0)==pdTRUE;
        if (!have_log) have_log=xQueueReceive(log_queue,&pending_log,0)==pdTRUE;
        xSemaphoreTake(archive_mutex,portMAX_DELAY);
        recover(&history);recover(&logs);
        if (have_history && history.ready) {
            int n=format_record(json,sizeof(json),&pending_history,true);
            if (n<0) {count_drop(true);have_history=false;}
            else if (append_archive(&history,json,(size_t)n)==FF_OK) have_history=false;
        }
        if (have_log && logs.ready) {
            int n=format_record(json,sizeof(json),&pending_log,false);
            if (n<0) {count_drop(false);have_log=false;}
            else if (append_archive(&logs,json,(size_t)n)==FF_OK) have_log=false;
        }
        xSemaphoreGive(archive_mutex);
        if (esp_timer_get_time()>=next_report) {
            storage_archive_stats_t stats;storage_archive_get_stats(&stats);
            if (stats.history_dropped!=reported_history || stats.logs_dropped!=reported_logs) {
                ESP_LOGW(TAG,"queue drops history=%" PRIu32 " logs=%" PRIu32,
                         stats.history_dropped,stats.logs_dropped);
                char detail[96];
                snprintf(detail,sizeof(detail),"history_dropped=%" PRIu32 " logs_dropped=%" PRIu32,
                         stats.history_dropped,stats.logs_dropped);
                storage_log(STORAGE_WARN,"archive","queue_overflow",ESP_ERR_NO_MEM,0,detail);
                reported_history=stats.history_dropped;reported_logs=stats.logs_dropped;
            }
            next_report=esp_timer_get_time()+10000000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static esp_err_t create_boot_id(void)
{
    uint8_t mac[6];esp_err_t rc=esp_read_mac(mac,ESP_MAC_WIFI_STA);
    if (rc!=ESP_OK) return rc;
    nvs_handle_t nvs;rc=nvs_open("archive_meta",NVS_READWRITE,&nvs);
    if (rc!=ESP_OK) return rc;
    uint64_t boot=0;
    rc=nvs_get_u64(nvs,"boot_seq",&boot);
    if (rc==ESP_ERR_NVS_NOT_FOUND) rc=ESP_OK;
    if (rc==ESP_OK && boot==UINT64_MAX) rc=ESP_ERR_INVALID_STATE;
    if (rc==ESP_OK) rc=nvs_set_u64(nvs,"boot_seq",++boot);
    if (rc==ESP_OK) rc=nvs_commit(nvs);
    nvs_close(nvs);
    if (rc==ESP_OK) snprintf(boot_id,sizeof(boot_id),"%02x%02x%02x%02x%02x%02x-%016" PRIx64,
                             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],boot);
    return rc;
}
esp_err_t storage_archive_start(void)
{
    if (started) return ESP_OK;
    history.part=w25q64_history_partition();logs.part=w25q64_logs_partition();
    if (!history.part || !logs.part) return ESP_ERR_INVALID_STATE;
    esp_err_t rc=create_boot_id();if (rc!=ESP_OK) return rc;
    archive_mutex=xSemaphoreCreateMutex();
    history_queue=xQueueCreate(QUEUE_LENGTH,sizeof(queued_record_t));
    log_queue=xQueueCreate(QUEUE_LENGTH,sizeof(queued_record_t));
    if (!archive_mutex || !history_queue || !log_queue) goto no_memory;
    /* Application startup is the single creator; tasks only submit afterward. */
    started=true;
    if (xTaskCreate(archive_task,"archive",8192,NULL,3,NULL)!=pdPASS) {
        started=false;goto no_memory;
    }
    storage_log(STORAGE_INFO,"system","boot",0,0,boot_id);
    return ESP_OK;
no_memory:
    if (archive_mutex) vSemaphoreDelete(archive_mutex);
    if (history_queue) vQueueDelete(history_queue);
    if (log_queue) vQueueDelete(log_queue);
    archive_mutex=NULL;history_queue=NULL;log_queue=NULL;
    return ESP_ERR_NO_MEM;
}
int storage_archive_read(storage_archive_kind_t kind,uint64_t after,
                         void *data,size_t capacity,size_t *len,uint64_t *id)
{
    if (!started || (kind!=STORAGE_HISTORY && kind!=STORAGE_LOGS)) return FF_ARGUMENT;
    archive_t *a=kind==STORAGE_HISTORY?&history:&logs;
    xSemaphoreTake(archive_mutex,portMAX_DELAY);
    int rc=a->ready?ff_read_after(&a->fifo,after,data,capacity,len,id):FF_IO;
    if (a->ready && !a->fifo.ready) {
        a->ready=false;a->next_recovery=esp_timer_get_time()+RECOVERY_US;
    }
    xSemaphoreGive(archive_mutex);
    return rc;
}
void storage_archive_get_stats(storage_archive_stats_t *s)
{
    if (!s) return;
    memset(s,0,sizeof(*s));
    if (started) {
        xSemaphoreTake(archive_mutex,portMAX_DELAY);
        s->history_evicted=history.evicted;s->logs_evicted=logs.evicted;
        s->history_write_errors=history.write_errors;s->logs_write_errors=logs.write_errors;
        s->history_ready=history.ready;s->logs_ready=logs.ready;
        xSemaphoreGive(archive_mutex);
    }
    portENTER_CRITICAL(&stamp_lock);
    s->history_dropped=history_dropped;s->logs_dropped=logs_dropped;
    portEXIT_CRITICAL(&stamp_lock);
}
