// SPDX-License-Identifier: Apache-2.0
// USB hardware and scheduler substitutes for production transfer recovery checks.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CFG_TUD_VIDEO 1
#define CFG_TUD_VIDEO_STREAMING 1
#define UVC_CAM_NUM 1
#define VS_STATE_PROBING 0
#define XFER_RESULT_SUCCESS 0
#define TU_ASSERT(c, ...) do { if (!(c)) return false; } while (0)
#define TU_VERIFY(c, ...) TU_ASSERT(c)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define CONFIG_TINYUSB_RHPORT_HS 1
#define CONFIG_FORMAT_MJPEG_CAM1 1
#define CONFIG_UVC_TINYUSB_TASK_CORE 0
#define CONFIG_UVC_TINYUSB_TASK_PRIORITY 7
#define UVC_FORMAT_JPEG 1
#define USB_PHY_CTRL_OTG 1
#define USB_OTG_MODE_DEVICE 1
#define USB_PHY_TARGET_INT 1
#define USB_PHY_SPEED_HIGH 1
#define pdPASS 1
#define tskNO_AFFINITY -1
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef struct { int controller, otg_mode, target, otg_speed; } usb_phy_config_t;
typedef unsigned xfer_result_t;
typedef struct {
    struct { unsigned ep[1]; } desc;
    uint8_t *buffer;
    size_t bufsize, offset;
    unsigned state, index_vc, index_vs;
} videod_streaming_interface_t;
typedef struct { uint8_t buf[64]; } videod_streaming_epbuf_t;
typedef struct { uint8_t *beg; } videod_interface_t;
typedef struct { uint8_t FrameID, EndOfFrame; } tusb_video_payload_header_t;
typedef struct { uint8_t *buf; size_t len; } uvc_fb_t;
typedef struct {
    uint8_t *uvc_buffer;
    size_t uvc_buffer_size;
    void *cb_ctx;
    uvc_fb_t *(*fb_get_cb)(void *);
    void (*fb_return_cb)(uvc_fb_t *, void *);
} uvc_device_config_t;

static videod_streaming_interface_t _videod_streaming_itf[1] = {{.desc = {{1}}, .state = 2}};
#define stream _videod_streaming_itf[0]
static videod_streaming_epbuf_t _videod_streaming_epbuf[1];
static uint8_t descriptor[4], transfer[64], pixels[32];
static videod_interface_t _videod_itf[1] = {{descriptor}};
static uint32_t now_ms;
static unsigned driver_calls, fail_call, aborts, completed, acquired, returned;
static bool ready = true, streaming = true;
static uvc_fb_t frame = {pixels, sizeof(pixels)};
static videod_streaming_interface_t *_get_instance_streaming(unsigned c, unsigned s) {
    assert(c == 0 && s == 0); return &stream;
}
static uint8_t _desc_ep_addr(const uint8_t *p) { (void)p; return 0x81; }
static bool usbd_edpt_claim(unsigned p, unsigned e) { (void)p; (void)e; return true; }
static bool usbd_edpt_release(unsigned p, unsigned e) { (void)p; (void)e; return true; }
static void usbd_edpt_stall(unsigned p, unsigned e) { (void)p; (void)e; ++aborts; }
static void usbd_edpt_clear_stall(unsigned p, unsigned e) { (void)p; (void)e; }
static unsigned _prepare_in_payload(videod_streaming_interface_t *s, uint8_t *b) {
    (void)b;
    unsigned n = s->bufsize - s->offset < 16 ? s->bufsize - s->offset : 16;
    s->offset += n;
    return n + 2;
}
static bool usbd_edpt_xfer(unsigned p, unsigned e, uint8_t *b, unsigned n) {
    (void)p; (void)e; (void)b; (void)n;
    return ++driver_calls != fail_call;
}
static void tud_video_frame_xfer_complete_cb(unsigned c, unsigned s) {
    assert(c == 0 && s == 0); ++completed;
}
static uint32_t get_time_millis(void) { return now_ms; }
static bool tud_ready(void) { return ready; }
static bool tud_video_n_streaming(unsigned c, unsigned s) {
    assert(c == 0 && s == 0); return streaming;
}
static uvc_fb_t *get_frame(void *p) { (void)p; ++acquired; return &frame; }
static void return_frame(uvc_fb_t *f, void *p) {
    (void)p; assert(f == &frame); ++returned;
}
static struct {
    uint32_t interval_ms[1];
    uvc_device_config_t user_config[1];
    void *phy_hdl, *event_group;
    TaskHandle_t tusb_task_hdl;
    bool uvc_init[1];
    int format[1];
} s_uvc_device = {.interval_ms = {40},
    .user_config = {{transfer, sizeof(transfer), NULL, get_frame, return_frame}}};

static unsigned init_failure, event_allocations, phy_allocations, stack_starts, task_starts;
static bool event_alive, phy_alive, stack_alive;
static void *xEventGroupCreate(void) {
    ++event_allocations;
    if (init_failure == 1) return NULL;
    assert(!event_alive); event_alive = true; return &event_alive;
}
static void vEventGroupDelete(void *event) {
    assert(event == &event_alive && event_alive); event_alive = false;
}
static esp_err_t usb_new_phy(const usb_phy_config_t *config, void **out) {
    assert(config->controller == USB_PHY_CTRL_OTG && config->otg_speed == USB_PHY_SPEED_HIGH);
    ++phy_allocations;
    if (init_failure == 2) return -42;
    assert(!phy_alive); phy_alive = true; *out = &phy_alive; return ESP_OK;
}
static esp_err_t usb_del_phy(void *phy) {
    assert(phy == &phy_alive && phy_alive && !stack_alive);
    phy_alive = false; return ESP_OK;
}
static bool tusb_init(void) {
    assert(phy_alive); ++stack_starts;
    stack_alive = init_failure != 3; return stack_alive;
}
static bool tusb_teardown(void) { stack_alive = false; return true; }
static void tusb_device_task(void *arg) { (void)arg; }
static BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *out, BaseType_t core) {
    (void)name; (void)arg;
    assert(entry == tusb_device_task && stack == 4096 && priority == 7 && core == 0);
    assert(event_alive && phy_alive && stack_alive); ++task_starts;
    if (init_failure == 4) return 0;
    *out = &task_starts; return pdPASS;
}

/* PRODUCTION_FUNCTIONS */

static void check_initialization(void) {
    assert(uvc_device_init() == ESP_ERR_INVALID_STATE && event_allocations == 0);
    s_uvc_device.uvc_init[0] = true;
    const esp_err_t expected[] = {ESP_ERR_NO_MEM, -42, ESP_FAIL, ESP_ERR_NO_MEM};
    for (init_failure = 1; init_failure <= 4; ++init_failure) {
        unsigned phy_before = phy_allocations, stack_before = stack_starts, task_before = task_starts;
        assert(uvc_device_init() == expected[init_failure - 1]);
        assert(!event_alive && !phy_alive && !stack_alive);
        assert(!s_uvc_device.event_group && !s_uvc_device.phy_hdl && !s_uvc_device.tusb_task_hdl);
        assert(phy_allocations - phy_before == (init_failure > 1));
        assert(stack_starts - stack_before == (init_failure > 2));
        assert(task_starts - task_before == (init_failure > 3));
    }
    init_failure = 0;
    assert(uvc_device_init() == ESP_OK && s_uvc_device.tusb_task_hdl);
    unsigned allocations = event_allocations;
    assert(uvc_device_init() == ESP_ERR_INVALID_STATE && event_allocations == allocations);
    assert(event_alive && phy_alive && stack_alive);
    puts("PASS: initialization failures preserve errors, release resources and allow retry; duplicate initialization is rejected.");
}

static void advance(unsigned milliseconds) {
    now_ms += milliseconds;
    video_service(0);
}
static void finish_frame(void) {
    assert(uvc_transport_busy(0, 0));
    while (uvc_transport_busy(0, 0)) assert(videod_xfer_cb(0, 0x81, 0, 16));
}
int main(void) {
    check_initialization();
    memset(pixels, 0x12, sizeof(pixels));
    video_service(0); advance(40);
    assert(acquired == 1 && returned == 1 && uvc_transport_busy(0, 0));
    memset(pixels, 0x34, sizeof(pixels));
    advance(40);
    assert(acquired == 1 && transfer[0] == 0x12);
    finish_frame(); advance(1);
    assert(acquired == 2 && transfer[0] == 0x34);
    finish_frame();

    // A rejected first packet must permit a later driver submission.
    fail_call = driver_calls + 1;
    advance(40);
    assert(!uvc_transport_busy(0, 0));
    unsigned attempts = driver_calls;
    advance(40);
    assert(driver_calls == attempts + 1 && uvc_transport_busy(0, 0));
    finish_frame();

    // A rejected continuation must release the frame, not retain a busy buffer.
    advance(40); fail_call = driver_calls + 1;
    assert(!videod_xfer_cb(0, 0x81, 0, 16));
    assert(!uvc_transport_busy(0, 0));
    advance(40); finish_frame();

    // An unsuccessful packet terminates the frame instead of transmitting its tail.
    advance(40); attempts = driver_calls;
    assert(videod_xfer_cb(0, 0x81, 1, 0));
    assert(driver_calls == attempts && !uvc_transport_busy(0, 0));
    advance(40); finish_frame();

    // Missing completion is cancelled after one second; the next interval recovers.
    advance(40); unsigned cancelled = aborts;
    advance(999); assert(uvc_transport_busy(0, 0) && aborts == cancelled);
    advance(1); assert(!uvc_transport_busy(0, 0) && aborts == cancelled + 1);
    unsigned done = completed;
    assert(videod_xfer_cb(0, 0x81, 0, 16));
    assert(completed == done);
    advance(39); assert(!uvc_transport_busy(0, 0));
    advance(1); finish_frame();

    // Commit cancellation does not require the sender to observe a stopped stream.
    advance(40); uvc_transport_abort(0, 0);
    advance(40); finish_frame();

    // Interface reactivation clears the class buffer without an application notification.
    advance(40); stream.buffer = NULL; stream.bufsize = stream.offset = 0;
    advance(40); finish_frame();

    // Suspend stops submissions; a retained transfer receives bounded recovery on resume.
    advance(40); ready = false; attempts = driver_calls;
    advance(2000); assert(driver_calls == attempts);
    ready = true; advance(0); advance(1000);
    assert(!uvc_transport_busy(0, 0));
    advance(40); finish_frame();

    streaming = false; advance(10); attempts = driver_calls;
    advance(2000); assert(driver_calls == attempts);
    streaming = true; advance(0); advance(40); finish_frame();

    // Millisecond counter wrap must preserve frame pacing and timeout arithmetic.
    streaming = false; advance(0); now_ms = UINT32_MAX - 20;
    streaming = true; advance(0); attempts = driver_calls;
    advance(39); assert(driver_calls == attempts);
    advance(1); assert(uvc_transport_busy(0, 0)); finish_frame();

    frame.len = sizeof(transfer) + 1; attempts = driver_calls;
    advance(40); assert(driver_calls == attempts && acquired == returned);
    puts("PASS: normal pacing/ownership, initial/continuation failure, failed packet, timeout,");
    puts("      late completion, commit/interface restart, suspend/resume, counter wrap, oversized frame.");
    return 0;
}
