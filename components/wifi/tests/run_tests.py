#!/usr/bin/env python3
"""Host tests of actual boot decision/reprovision functions (RTOS/NVS mocked)."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "wifi.c").read_text()


def function(signature):
    start = source.index(signature + "\n{")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_NVS_NO_FREE_PAGES 2
#define ESP_ERR_NVS_NEW_VERSION_FOUND 3
#define ESP_ERR_NO_MEM 4
#define NVS_READWRITE 1
#define pdFALSE 0
#define pdPASS 1
#define CONNECTED_BIT 1
#define BLE_SHUTDOWN_REQUESTED_BIT 2
#define SAVED_WIFI_TIMEOUT_MS 30000
#define pdMS_TO_TICKS(x) (x)
#define BLUFI_INFO(...) ((void)0)
#define BLUFI_ERROR(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef unsigned EventBits_t;
static bool boot_with_saved_wifi, force_provision;
static bool saved_ssid;
static uint8_t forced_flag;
static unsigned events, waits, blufi_starts, task_deletes, commits, restarts, tasks;
static int set_error;
static void *wifi_event_group;
static void wifi_recovery_task(void *arg) {(void)arg;}
static void blufi_boot_task(void *arg);
static int nvs_flash_init(void) {return ESP_OK;}
static int nvs_flash_erase(void) {assert(!"Unexpected NVS erase"); return ESP_OK;}
static int nvs_open(const char *name, int mode, int *handle) {
    assert(!strcmp(name,"wifi_boot") && mode == NVS_READWRITE); *handle=1; return ESP_OK;
}
static int nvs_get_u8(int h, const char *key, uint8_t *value) {
    (void)h; assert(!strcmp(key,"force_blufi"));
    *value=forced_flag; return forced_flag ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_set_u8(int h, const char *key, uint8_t value) {
    (void)h; assert(!strcmp(key,"force_blufi"));
    if (set_error) return set_error;
    forced_flag=value; return ESP_OK;
}
static int nvs_erase_key(int h, const char *key) {
    (void)h; assert(!strcmp(key,"force_blufi")); forced_flag=0; return ESP_OK;
}
static int nvs_commit(int h) {(void)h; ++commits; return ESP_OK;}
static void nvs_close(int h) {(void)h;}
static void esp_restart(void) {++restarts;}
static void initialise_wifi(void) {boot_with_saved_wifi = saved_ssid && !force_provision;}
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg,
                       int priority, void *handle) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    ++tasks; return pdPASS;
}
static unsigned xEventGroupWaitBits(void *group, unsigned bits, int clear, int all, int timeout) {
    (void)group;
    assert(bits == (CONNECTED_BIT | BLE_SHUTDOWN_REQUESTED_BIT));
    assert(!clear && !all && timeout == 30000); ++waits; return events;
}
static void start_blufi(void) {++blufi_starts;}
static void vTaskDelete(void *arg) {(void)arg; ++task_deletes;}
'''
checks = r'''
static void boot(bool saved, unsigned bits) {
    saved_ssid=saved; events=bits; waits=blufi_starts=task_deletes=tasks=0;
    wifi_init(); blufi_boot_task(NULL);
    assert(tasks==2 && task_deletes==1);
}
int main(void) {
    boot(false,0); assert(blufi_starts==1 && waits==0);
    boot(true,CONNECTED_BIT); assert(blufi_starts==0 && waits==1);
    boot(true,0); assert(blufi_starts==1 && waits==1);
    /* Connection succeeded and dropped before the boot task was scheduled. */
    boot(true,BLE_SHUTDOWN_REQUESTED_BIT); assert(blufi_starts==0);
    wifi_reprovision(); assert(forced_flag==1 && restarts==1 && commits==1);
    boot(true,0); assert(force_provision && !forced_flag && blufi_starts==1 && waits==0);
    boot(true,CONNECTED_BIT); assert(!force_provision && blufi_starts==0);
    set_error=9; wifi_reprovision(); assert(restarts==1 && forced_flag==0);
    puts("PASS: no credentials, saved success, timeout, success latch, one-shot reprovision, NVS failure");
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-boot-tests-") as tmp:
    c = Path(tmp) / "boot.c"
    c.write_text(preamble + "\n" + function("void wifi_reprovision(void)") + "\n" +
                 function("void wifi_init(void)") + "\n" +
                 function("static void blufi_boot_task(void *arg)") + "\n" + checks)
    binary = Path(tmp) / "boot"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
