#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "lvgl.h"
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FOUND 0x105
typedef int uvc_format_t;
typedef struct { void *buf; size_t len; } uvc_fb_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
#define portMAX_DELAY 0x7fffffff
static unsigned host_notifications, host_ppa_calls;
static int host_ppa_error;
static bool host_display_available = true, host_mailbox_available = true;
static void (*host_ppa_hook)(void);
static jmp_buf host_task_done;
static bool host_orientation_running, host_touch_pressed, host_display_locked;
static unsigned host_task_steps, host_panel_rotation, host_rotation_calls;
static int host_rotation_error, host_imu_error, host_init_error;
static float host_ax, host_ay = 1, host_az;
static esp_err_t bsp_imu_init(void) { return host_init_error; }
static esp_err_t bsp_imu_start(const void *p) { return 0; }
static esp_err_t bsp_imu_deinit(void) { return 0; }
static esp_err_t bsp_imu_get_accel(float *x, float *y, float *z) {
 *x=host_ax; *y=host_ay; *z=host_az; return host_imu_error;
}
typedef unsigned bsp_display_rotation_t;
static unsigned bsp_display_get_rotation(void) { return host_panel_rotation; }
static int bsp_display_set_rotation(unsigned angle) {
 assert(!host_display_locked); ++host_rotation_calls;
 if (!host_rotation_error) host_panel_rotation=angle;
 return host_rotation_error;
}
static int host_touch_state(lv_indev_t *p) { return host_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
#define lv_indev_get_state host_touch_state
static unsigned host_touch_resets, host_touch_waits;
static void host_touch_reset(lv_indev_t *p, lv_obj_t *o) { assert(host_display_locked);++host_touch_resets; }
static void host_touch_wait(lv_indev_t *p) { assert(host_display_locked);++host_touch_waits; }
#define lv_indev_reset host_touch_reset
#define lv_indev_wait_release host_touch_wait

static void xTaskNotifyGive(TaskHandle_t task) { ++host_notifications; }
static unsigned ulTaskNotifyTake(int clear, int timeout) { return 0; }
typedef void *ppa_client_handle_t;
typedef struct { int oper_type, max_pending_trans_num; } ppa_client_config_t;
typedef struct {
 struct { const void *buffer; int pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,srm_cm,yuv_range,yuv_std; } in;
 struct { void *buffer; size_t buffer_size; int pic_w,pic_h,block_offset_x,block_offset_y,srm_cm; } out;
 int rotation_angle;
 float scale_x,scale_y;
 bool mirror_x, mirror_y;
 int mode;
} ppa_srm_oper_config_t;
typedef struct { bool enable_touch, enable_ppa_accel; } bsp_display_config_t;
#define BSP_DISPLAY_DEFAULT_CONFIG() {0}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
typedef enum { PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
 PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270 } ppa_srm_rotation_angle_t;
#define PPA_OPERATION_SRM 0
#define PPA_SRM_COLOR_MODE_RGB565 0
#define PPA_TRANS_MODE_BLOCKING 0
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 2
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 4
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_RETURN_ON_FALSE(ok,err,...) do { if (!(ok)) return (err); } while(0)
#define ESP_RETURN_ON_ERROR(expr,...) do { int rc=(expr); if(rc) return rc; } while(0)
#define ESP_LOGW(...) ((void)0)
static int64_t host_time = 1000000;
static int64_t esp_timer_get_time(void) { return host_time; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static void (*host_mailbox_wait_hook)(int);
static int xSemaphoreTake(SemaphoreHandle_t s,int t) {
 if (!host_mailbox_available && host_mailbox_wait_hook) host_mailbox_wait_hook(t);
 if (!host_mailbox_available) return 0;
 host_mailbox_available=false; return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { assert(!host_mailbox_available); host_mailbox_available=true; }
static void vTaskDelay(int t) {
 if(host_orientation_running) {
  host_time+=(int64_t)t*1000;
  if(++host_task_steps>12) longjmp(host_task_done,1);
 }
}
static void vTaskDelete(void *p) { assert(host_orientation_running); longjmp(host_task_done,1); }
static int xTaskCreate(void (*f)(void *),const char*n,int stack,void*a,int p,void*t) { return 1; }
/* Capture runs synchronously; hardware workers are not scheduled on the host. */
static int xTaskCreatePinnedToCore(void (*f)(void *),const char*n,int stack,void*a,int p,void*t,int c) { return 1; }
static void *heap_caps_aligned_calloc(size_t a,size_t n,size_t size,int caps) { return calloc(n,size); }
static int ppa_register_client(const ppa_client_config_t*c,ppa_client_handle_t*p) { *p=(void *)1; return 0; }
static int ppa_do_scale_rotate_mirror(ppa_client_handle_t p,const ppa_srm_oper_config_t*c) {
 ++host_ppa_calls;
 if(host_ppa_hook) host_ppa_hook();
 if(host_ppa_error) return host_ppa_error;
 const uint16_t *src=c->in.buffer; uint16_t *dst=c->out.buffer;
 int w=c->in.block_w, h=c->in.block_h;
 int rw=c->rotation_angle%2?h:w, rh=c->rotation_angle%2?w:h;
 int ow=(int)(rw*c->scale_x), oh=(int)(rh*c->scale_y);
 assert(ow+c->out.block_offset_x<=c->out.pic_w && oh+c->out.block_offset_y<=c->out.pic_h);
 for(int y=0;y<oh;++y) for(int x=0;x<ow;++x) {
  int u=(int)(x/c->scale_x), v=(int)(y/c->scale_y), sx=0, sy=0;
  if(c->mirror_x) u=rw-1-u;
  if(c->mirror_y) v=rh-1-v;
  switch(c->rotation_angle) {
   case 0:sx=u;sy=v;break;
   case 1:sx=w-1-v;sy=u;break;
   case 2:sx=w-1-u;sy=h-1-v;break;
   case 3:sx=v;sy=h-1-u;break;
  }
  dst[(y+c->out.block_offset_y)*c->out.pic_w+x+c->out.block_offset_x]=src[(sy+c->in.block_offset_y)*c->in.pic_w+sx+c->in.block_offset_x];
 }
 return 0;
}
static int esp_cache_msync(void*p,size_t n,int flags) { return 0; }
static bool bsp_display_lock(int timeout) { if(!host_display_available) return false; assert(!host_display_locked);host_display_locked=true; return true; }
static void bsp_display_unlock(void) { assert(host_display_locked);host_display_locked=false; }
static int bsp_display_brightness_set(int level) { return 0; }
static lv_indev_t *bsp_display_get_input_dev(void) { return (lv_indev_t *)1; }
static lv_display_t *bsp_display_start_with_config(const bsp_display_config_t*c) { return lv_display_get_default(); }
static int webcam_display_start(void) { return lv_display_get_default() ? ESP_OK : ESP_FAIL; }
static bool webcam_display_lock(int timeout) { return bsp_display_lock(timeout); }
static void webcam_display_unlock(void) { bsp_display_unlock(); }
static lv_indev_t *webcam_display_get_input_dev(void) { return bsp_display_get_input_dev(); }
static unsigned webcam_display_get_rotation(void) { return bsp_display_get_rotation(); }
static int webcam_display_set_rotation(unsigned angle) { return bsp_display_set_rotation(angle); }
