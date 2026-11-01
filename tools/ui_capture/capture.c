#include "host.h"
#include "webcam.h"
static webcam_status_t mock = {.privacy=true,.mounted=true,.epoch=1,
 .settings={.brightness=70,.preview=true,.mirror=false,.sleep_seconds=WEBCAM_DEFAULT_SLEEP_SECONDS,.rotation=270}};
void pipeline_snapshot(webcam_status_t *s) { *s=mock; }
void pipeline_privacy(bool on) { mock.privacy=on; ++mock.epoch; }
void pipeline_screen_off(bool off) { mock.screen_off=off; }
void pipeline_settings(const webcam_settings_t *s) { if (s->rotation != mock.settings.rotation) ++mock.epoch; mock.settings=*s; }
static const void *borrowed;
static unsigned releases, accepted, drops;
void pipeline_preview_result(bool rendered, uint32_t elapsed_us) { if(rendered) ++accepted; else ++drops; }
void pipeline_preview_release(const void *p) { assert(p==borrowed); borrowed=NULL; ++releases; }
/* Compile the production source directly, including its actual event handlers. */
#include "preview_ui.c"
static uint16_t pixels[480*480], draw[480*480];
static void flush(lv_display_t*d,const lv_area_t*a,uint8_t*p) {
 int w=a->x2-a->x1+1;
 for(int y=a->y1;y<=a->y2;y++) memcpy(&pixels[y*480+a->x1],p+(y-a->y1)*w*2,w*2);
 lv_display_flush_ready(d);
}
static void snap(const char *name) {
 refresh(NULL);
 lv_tick_inc(300);lv_timer_handler();
 lv_obj_update_layout(lv_screen_active());
 lv_obj_invalidate(lv_screen_active());
 lv_refr_now(NULL);
 char path[256]; snprintf(path,sizeof(path),"%s.ppm",name);
 FILE*f=fopen(path,"wb"); assert(f);
 fprintf(f,"P6\n480 480\n255\n");
 for(int i=0;i<480*480;i++) {
  uint16_t p=pixels[i]; uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
  fwrite(rgb,1,3,f);
 }
 fclose(f);
}


static void press(lv_obj_t*o) { lv_obj_send_event(o,LV_EVENT_CLICKED,NULL); }
static lv_obj_t *find_button(lv_obj_t *parent,const char *caption) {
 for(uint32_t i=0;i<lv_obj_get_child_count(parent);i++) {
  lv_obj_t *obj=lv_obj_get_child(parent,i);
  if(lv_obj_check_type(obj,&lv_button_class) && lv_obj_get_child_count(obj)>0 &&
     strcmp(lv_label_get_text(lv_obj_get_child(obj,0)),caption)==0) return obj;
 }
 assert(!"button not found"); return NULL;
}
static void select_language(unsigned selected) {
 lv_buttonmatrix_set_selected_button(language_buttons,selected);
 lv_obj_send_event(language_buttons,LV_EVENT_VALUE_CHANGED,NULL);
 assert(mock.settings.language==selected && language==selected);
 assert(lv_buttonmatrix_has_button_ctrl(language_buttons,selected,LV_BUTTONMATRIX_CTRL_CHECKED));
 assert(!lv_buttonmatrix_has_button_ctrl(language_buttons,1-selected,LV_BUTTONMATRIX_CTRL_CHECKED));
}
static lv_point_t touch_point;
static bool touch_pressed;
static void read_touch(lv_indev_t *device, lv_indev_data_t *data) {
 (void)device;data->point=touch_point;
 data->state=touch_pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void tap(lv_indev_t *device,int x,int y) {
 touch_point=(lv_point_t){x,y};touch_pressed=true;lv_indev_read(device);
 touch_pressed=false;lv_indev_read(device);
}
int main(int argc, char **argv) {
 (void)argv;
 if(argc>1) mock.settings.language=WEBCAM_LANGUAGE_EN;
 lv_init();lv_display_t*d=lv_display_create(480,480);
 lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(d,draw,NULL,sizeof(draw),LV_DISPLAY_RENDER_MODE_FULL);
 lv_display_set_flush_cb(d,flush);
 assert(preview_ui_init()==ESP_OK);
 if(argc>1) {assert(language==WEBCAM_LANGUAGE_EN);snap("boot-en");select_language(WEBCAM_LANGUAGE_ZH);}
 assert(language==WEBCAM_LANGUAGE_ZH && mock.settings.sleep_seconds==300);
 snap("home-zh");
 press(lv_obj_get_parent(settings_label));snap("settings-zh");
 assert(lv_dropdown_get_selected(sleep_dropdown)==3);
 lv_indev_t *touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
 lv_indev_set_read_cb(touch,read_touch);
 tap(touch,430,12);assert(language==WEBCAM_LANGUAGE_EN);
 tap(touch,360,59);assert(language==WEBCAM_LANGUAGE_ZH);
 tap(touch,430,59);assert(language==WEBCAM_LANGUAGE_EN);
 tap(touch,360,12);assert(language==WEBCAM_LANGUAGE_ZH);
 lv_indev_delete(touch);
 lv_area_t hit;lv_obj_get_click_area(language_buttons,&hit);assert(hit.y2-hit.y1+1>=48);
 webcam_settings_t before=mock.settings;unsigned epoch=mock.epoch;
 select_language(WEBCAM_LANGUAGE_EN);snap("settings-en");
 assert(strcmp(lv_label_get_text(settings_title),"Settings")==0);
 assert(strcmp(lv_label_get_text(preview_value),"On")==0);
 assert(mock.settings.sleep_seconds==before.sleep_seconds && mock.settings.rotation==before.rotation);
 assert(mock.settings.preview==before.preview && mock.settings.mirror==before.mirror);
 assert(mock.settings.brightness==before.brightness && mock.epoch==epoch);
 press(lv_obj_get_parent(back_label));snap("home-en");
 press(lv_obj_get_parent(settings_label));
 lv_dropdown_set_selected(sleep_dropdown,1);lv_obj_send_event(sleep_dropdown,LV_EVENT_VALUE_CHANGED,NULL);
 assert(mock.settings.sleep_seconds==30);
 select_language(WEBCAM_LANGUAGE_ZH);assert(lv_dropdown_get_selected(sleep_dropdown)==1);
 lv_dropdown_set_selected(sleep_dropdown,3);lv_obj_send_event(sleep_dropdown,LV_EVENT_VALUE_CHANGED,NULL);
 lv_dropdown_open(sleep_dropdown);snap("timeouts-zh");lv_dropdown_close(sleep_dropdown);
 lv_dropdown_open(rotation_dropdown);snap("rotation-zh");lv_dropdown_close(rotation_dropdown);
 lv_obj_remove_state(preview_switch,LV_STATE_CHECKED);lv_obj_send_event(preview_switch,LV_EVENT_VALUE_CHANGED,NULL);
 snap("preview-off-zh");
 press(lv_obj_get_parent(back_label));
 mock.privacy=false;mock.streaming=true;mock.error=ESP_ERR_NOT_FOUND;snap("error-zh");
 assert(strcmp(lv_label_get_text(state_label),"摄像头故障")==0);
 select_language(WEBCAM_LANGUAGE_EN);snap("error-en");
 assert(strcmp(lv_label_get_text(state_label),"Camera error")==0);
 mock.privacy=true;mock.error=ESP_OK;
 host_time=last_activity+299000000;refresh(NULL);assert(!mock.screen_off);
 host_time=last_activity+301000000;refresh(NULL);assert(mock.screen_off);
 press(wake_cover);assert(!mock.screen_off);
 puts("Language callbacks, immediate localization, preserved settings, dropdowns, 5-minute idle and wake checks passed.");
}
