#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_lv_present.h"

enum {W=64,H=96,LINES=4,BYTES=W*LINES*2};
static uint16_t pool[2][W*LINES], canvas[W*H];
static struct {bool busy; esp_display_present_area_t area; uint8_t snapshot[BYTES]; size_t bytes;} jobs[2];
static unsigned next_slot, overwrites, submissions;
static bool asynchronous;
static unsigned buffer_count;
static void complete(unsigned slot)
{
    if (!jobs[slot].busy) return;
    if (memcmp(pool[slot],jobs[slot].snapshot,jobs[slot].bytes)) ++overwrites;
    esp_display_present_area_t a=jobs[slot].area;
    size_t width=a.x2-a.x1+1;
    for (int y=a.y1;y<=a.y2;++y)
        memcpy(&canvas[y*W+a.x1],(uint8_t*)pool[slot]+(y-a.y1)*width*2,width*2);
    jobs[slot].busy=false;
}
esp_err_t esp_display_presenter_get_caps(const esp_display_presenter_t *p,esp_display_presenter_caps_t *c)
{
    (void)p; *c=(esp_display_presenter_caps_t){.contract=ESP_DISPLAY_PRESENT_CONTRACT_PARTITION,
        .width=W,.height=H,.pixel_format=ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,.drawbuf_bytes=BYTES};return ESP_OK;
}
esp_err_t esp_display_presenter_rebind_producer(esp_display_presenter_t *p){(void)p;return ESP_OK;}
esp_err_t esp_display_presenter_get_drawbuf(const esp_display_presenter_t *p,esp_display_presenter_drawbuf_t *b)
{
    (void)p;*b=(esp_display_presenter_drawbuf_t){.buffers={pool[0],pool[1]},.count=buffer_count,.lines=LINES,.bytes=BYTES};return ESP_OK;
}
esp_err_t esp_display_presenter_begin_next_frame(esp_display_presenter_t *p,
    const esp_display_present_surface_request_t *r,esp_display_present_area_t *a,size_t cap,size_t *n,bool *full)
{
    (void)p;(void)a;(void)cap;*n=0;*full=r->dirty_area_count==1&&r->dirty_areas[0].y1==0&&r->dirty_areas[0].y2==H-1;
    return ESP_OK;
}
static esp_err_t resolve(void *ctx,uint32_t lease,size_t stride,size_t requested,size_t *rows)
{(void)ctx;(void)lease;*rows=BYTES/stride;if(*rows>requested)*rows=requested;return ESP_OK;}
esp_err_t esp_display_presenter_acquire_buffer(esp_display_presenter_t *p,esp_display_presenter_buffer_t *b)
{
    (void)p;unsigned slot=next_slot;next_slot=(next_slot+1)%buffer_count;
    /* Match the production TE endpoint: wait for the pool slot at acquire. */
    complete(slot);
    *b=(esp_display_presenter_buffer_t){.surface={.pixels=pool[slot]},.capacity_bytes=BYTES,.lease_id=slot+1,.resolve_rows=resolve};
    return ESP_OK;
}
esp_err_t esp_display_presenter_submit_buffer(esp_display_presenter_t *p,
    const esp_display_presenter_buffer_t *b,const esp_display_present_area_t *a,size_t stride)
{
    (void)p;unsigned slot=b->lease_id-1;assert(!jobs[slot].busy);
    jobs[slot].busy=true;jobs[slot].area=*a;jobs[slot].bytes=stride*(a->y2-a->y1+1);
    memcpy(jobs[slot].snapshot,pool[slot],jobs[slot].bytes);++submissions;
    if(!asynchronous)complete(slot);
    return ESP_OK;
}
esp_err_t esp_display_presenter_commit_frame(esp_display_presenter_t *p,const esp_display_presenter_submit_t *s)
{(void)p;(void)s;complete(0);complete(1);return ESP_OK;}
void esp_display_presenter_cancel_frame(esp_display_presenter_t *p){(void)p;complete(0);complete(1);}
int main(int argc,char **argv)
{
    assert(argc==2);buffer_count=(unsigned)atoi(argv[1]);
    assert(buffer_count==1 || buffer_count==2);
    asynchronous=buffer_count>=2;
    lv_display_t *d=NULL;
    assert(esp_lv_present_start((esp_display_presenter_t*)1,&d)==ESP_OK);
    lv_obj_t *screen=lv_display_get_screen_active(d);
    lv_obj_set_style_pad_all(screen,0,0);
    lv_obj_set_style_bg_color(screen,lv_color_black(),0);
    for(int i=0;i<H/LINES;++i){
        lv_obj_t *o=lv_obj_create(screen);lv_obj_remove_style_all(o);
        lv_obj_set_pos(o,0,i*LINES);lv_obj_set_size(o,W,LINES);
        lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(o,lv_color_hex(0x050301*(i+1)),0);
    }
    lv_obj_update_layout(screen);lv_obj_invalidate(screen);lv_refr_now(d);
    printf("mode=%s tiles=%u overwritten-before-copy=%u header-pixel=%04x bottom-pixel=%04x\n",
        argv[1],submissions,overwrites,canvas[0],canvas[(H-1)*W]);
    unsigned bad_pixels=0;
    for(int y=0;y<H;++y) for(int x=0;x<W;++x)
        if(canvas[y*W+x]!=lv_color_to_u16(lv_color_hex(0x050301*(y/LINES+1))))++bad_pixels;
    printf("incorrect pixels=%u\n",bad_pixels);
    if(buffer_count==1) assert(overwrites==0 && bad_pixels==0);
    else assert(overwrites>0 && bad_pixels>0);
    assert(submissions>2);assert(esp_lv_present_stop()==ESP_OK);
}
