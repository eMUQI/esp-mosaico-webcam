/* Exercise production UI transitions with debounced module-presence snapshots. */
static void submit_frame(void)
{
    const void *data = calloc(1, RAW_BYTES);
    assert(data);
    borrowed = data;
    const webcam_preview_frame_t frame = {
        .data = data, .epoch = mock.epoch,
        .width = FRAME_WIDTH, .height = FRAME_HEIGHT,
    };
    assert(preview_ui_submit(&frame));
    preview_frame();
    assert(!borrowed);
    free((void *)data);
}

static void fail_during_ppa(void)
{
    mock.error = ESP_ERR_TIMEOUT;
}

int main(void)
{
    language_checks(1, NULL);
    select_language(WEBCAM_LANGUAGE_ZH);
    mock.privacy = false;
    mock.screen_off = false;
    mock.streaming = false;
    mock.settings.preview = true;
    mock.error = ESP_FAIL;
    host_module_presence = MOSAICO_MODULE_PRESENCE_ABSENT;
    snap("removed-generic-error");
    assert(!lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(state_label), "请连接摄像头") == 0);

    mock.error = ESP_OK;
    mock.camera_ok = true;
    preview_valid = true;
    snap("removed-stale-ready");
    assert(!preview_valid);
    assert(!lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    const unsigned rendered = accepted;
    submit_frame();
    assert(accepted == rendered);
    assert(!lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));

    host_module_presence = MOSAICO_MODULE_PRESENCE_PRESENT;
    mock.error = ESP_ERR_TIMEOUT;
    snap("present-capture-timeout");
    assert(!lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(camera_art, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(state_label), "摄像头故障") == 0);
    assert(strcmp(lv_label_get_text(guide_hint), "正在自动重试") == 0);
    assert(!lv_label_get_text(notice)[0]);
    submit_frame();
    assert(accepted == rendered);
    assert(guide_waiting_for_preview);
    mock.error = ESP_OK;
    snap("connecting-first-frame");
    assert(!lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(camera_art, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(guide_title), "正在启动摄像头…") == 0);
    host_ppa_error = ESP_FAIL;
    submit_frame();
    assert(accepted == rendered && guide_waiting_for_preview);
    host_ppa_error = ESP_OK;
    host_ppa_hook = fail_during_ppa;
    submit_frame();
    assert(accepted == rendered && guide_waiting_for_preview);
    host_ppa_hook = NULL;
    mock.error = ESP_OK;
    submit_frame();
    assert(accepted == rendered + 1);
    assert(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(!guide_waiting_for_preview);
    snap("reinserted-preview");
    mock.error = ESP_ERR_TIMEOUT;
    snap("live-camera-fault");
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(camera_art, LV_OBJ_FLAG_HIDDEN));

    host_module_presence = MOSAICO_MODULE_PRESENCE_UNKNOWN;
    mock.error = ESP_ERR_TIMEOUT;
    refresh(NULL);
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    host_module_presence = MOSAICO_MODULE_PRESENCE_ABSENT;
    refresh(NULL);
    assert(guide_waiting_for_preview);
    mock.privacy = true;
    refresh(NULL);
    assert(!guide_waiting_for_preview);
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    mock.privacy = false;
    refresh(NULL);
    assert(guide_waiting_for_preview);
    mock.settings.preview = false;
    mock.streaming = false;
    refresh(NULL);
    assert(!guide_waiting_for_preview);
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));

    mock.settings.preview = true;
    mock.streaming = true;
    host_module_query_error = ESP_FAIL;
    refresh(NULL);
    assert(lv_obj_has_flag(camera_guide, LV_OBJ_FLAG_HIDDEN));
    host_module_query_error = ESP_OK;
    select_language(WEBCAM_LANGUAGE_EN);
    snap("removed-en");
    assert(strcmp(lv_label_get_text(state_label), "Connect the camera") == 0);
    puts("Removal, guide-to-preview handoff, failed-frame retention, faults and reinsertion checks passed.");
}
