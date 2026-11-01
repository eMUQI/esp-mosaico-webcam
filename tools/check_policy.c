#include <assert.h>
#include <stdio.h>
#include "frame_policy.h"
#include "orientation_policy.h"
#include "webcam_settings.h"

int main(void)
{
    const unsigned sensors[][2] = {{1024, 768}, {1280, 720}, {640, 480}};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned rotation = 0; rotation < 360; rotation += 90) {
            unsigned scale, x, y;
            assert(frame_fit(sensors[i][0], sensors[i][1], rotation, 640, 480, &scale, &x, &y));
            unsigned w = sensors[i][rotation % 180 ? 1 : 0];
            unsigned h = sensors[i][rotation % 180 ? 0 : 1];
            assert(x + w * scale / 16 <= 640);
            assert(y + h * scale / 16 <= 480);
            assert(x % 2 == 0 && y % 2 == 0);
            assert(w * (scale + 1) / 16 > 640 || h * (scale + 1) / 16 > 480);
        }
    }
    unsigned scale, x, y;
    assert(!frame_fit(0, 768, 0, 640, 480, &scale, &x, &y));
    assert(!frame_fit(1024, 768, 45, 640, 480, &scale, &x, &y));
    assert(!frame_fit(1024, 768, 360, 640, 480, &scale, &x, &y));
    assert(frame_fit(1280, 720, 0, 640, 480, &scale, &x, &y));
    assert(scale == 8 && x == 0 && y == 60);
    for (unsigned angle = 0; angle < 360; angle += 90) {
        assert(frame_fit(640, 480, angle, 480, 360, &scale, &x, &y));
        assert(scale == (angle % 180 ? 9 : 12));
        assert(x == (angle % 180 ? 104 : 0) && y == 0);
    }
    // USB rotation must not change the camera-to-panel mounting transform.
    for (unsigned mirrors = 0; mirrors < 4; ++mirrors) {
        bool mirror_x = mirrors & 1, mirror_y = mirrors & 2;
        for (unsigned video = 0; video < 360; video += 90) {
            for (unsigned panel = 0; panel < 360; panel += 90) {
                unsigned angle = preview_rotation(video, panel, mirror_x, mirror_y);
                unsigned mounted = mirror_x != mirror_y ? (360U - 270U) : 270U;
                unsigned encoded = mirror_x != mirror_y ? (360U - video) % 360U : video;
                assert((panel + angle + encoded) % 360U == mounted);
            }
        }
    }
    orientation_policy_t policy = {0};
    unsigned requested = 0;
    const float poses[][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    for (unsigned turn = 0; turn < 4; ++turn) {
        policy = (orientation_policy_t){0};
        unsigned current = (turn * 90 + 180) % 360;
        assert(!orientation_update(&policy, poses[turn][0], poses[turn][1], current, &requested));
        assert(!orientation_update(&policy, poses[turn][0], poses[turn][1], current, &requested));
        assert(orientation_update(&policy, poses[turn][0], poses[turn][1], current, &requested));
        assert(requested == turn * 90);
        // An unapplied target is available on the next poll without another debounce.
        assert(orientation_update(&policy, poses[turn][0], poses[turn][1], current, &requested));
        assert(!orientation_update(&policy, poses[turn][0], poses[turn][1], requested, &requested));
    }
    const float invalid[][2] = {{0.1f, 0.1f}, {0.6f, 0}, {0.8f, 0.75f}, {NAN, 1}, {1, INFINITY}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        policy = (orientation_policy_t){0};
        assert(!orientation_update(&policy, 1, 0, 0, &requested));
        assert(!orientation_update(&policy, invalid[i][0], invalid[i][1], 0, &requested));
        assert(policy.samples == 0);
        assert(!orientation_update(&policy, 1, 0, 0, &requested));
        assert(!orientation_update(&policy, 1, 0, 0, &requested));
        assert(orientation_update(&policy, 1, 0, 0, &requested) && requested == 90);
    }
    // A frame captured before either privacy transition must remain unpublished.
    assert(frame_publish_allowed(false, 7, 7));
    assert(!frame_publish_allowed(true, 8, 7));
    assert(!frame_publish_allowed(false, 9, 7));
    assert(frame_publish_allowed(false, 9, 9));
    webcam_settings_t settings = {.rotation = 0, .language = WEBCAM_LANGUAGE_EN};
    assert(WEBCAM_DEFAULT_SLEEP_SECONDS == 300);
    uint8_t saved[] = {1, 85, 0, 1, 44, 1, 0};
    assert(webcam_settings_decode(&settings, saved, 6));
    assert(settings.rotation == 0 && settings.brightness == 85 && !settings.preview &&
           settings.mirror && settings.sleep_seconds == 300 && settings.language == WEBCAM_LANGUAGE_ZH);
    saved[0] = 2;
    for (unsigned turn = 0; turn < 4; ++turn) {
        saved[6] = turn;
        assert(webcam_settings_decode(&settings, saved, sizeof(saved)));
        assert(settings.rotation == turn * 90);
    }
    saved[6] = 4;
    assert(!webcam_settings_decode(&settings, saved, sizeof(saved)));
    assert(settings.rotation == 270);
    assert(!webcam_settings_decode(&settings, saved, 6));
    assert(!webcam_settings_decode(&settings, saved, 0));
    saved[6] = 0;
    saved[2] = 2;
    assert(!webcam_settings_decode(&settings, saved, sizeof(saved)));
    saved[2] = 0;
    saved[4] = 1;
    assert(!webcam_settings_decode(&settings, saved, sizeof(saved)));
    uint8_t current[] = {3, 70, 1, 0, 44, 1, 0, WEBCAM_LANGUAGE_EN};
    assert(webcam_settings_decode(&settings, current, sizeof(current)));
    assert(settings.language == WEBCAM_LANGUAGE_EN && settings.sleep_seconds == 300);
    webcam_settings_t previous = settings;
    current[7] = 2;
    assert(!webcam_settings_decode(&settings, current, sizeof(current)));
    assert(settings.language == previous.language && settings.brightness == previous.brightness);
    current[7] = WEBCAM_LANGUAGE_ZH;
    assert(webcam_settings_decode(&settings, current, sizeof(current)));
    assert(settings.language == WEBCAM_LANGUAGE_ZH);
    assert(!webcam_settings_decode(&settings, current, sizeof(current) - 1));
    puts("Frame geometry, orientation hysteresis, epoch and settings migration checks passed");
}
