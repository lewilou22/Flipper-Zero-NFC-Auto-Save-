/**
 * NFC Autosave — Momentum FAP
 *
 * Loop: detect → read → save .nfc → wait for card leave → repeat.
 * Dumps land in /ext/nfc/lab/ as <Type>_<UID>_<YYYYMMDD_HHMMSS>.nfc
 */

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>

#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>
#include <storage/storage.h>
#include <datetime/datetime.h>
#include <toolbox/keys_dict.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_scanner.h>

#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_sync.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>
#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>
#include <nfc/protocols/mf_desfire/mf_desfire.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller.h>

#define TAG "NfcAutosave"

#define NFC_AUTOSAVE_FOLDER       EXT_PATH("nfc/lab")
#define NFC_AUTOSAVE_SEED_PATH    EXT_PATH("nfc/lab/seed.nfc")
#define NFC_AUTOSAVE_EXTENSION    ".nfc"
#define NFC_CLASSIC_DICT_SYSTEM   EXT_PATH("nfc/assets/mf_classic_dict.nfc")
#define NFC_CLASSIC_DICT_USER     EXT_PATH("nfc/assets/mf_classic_dict_user.nfc")
#define NFC_AUTOSAVE_SEED_KEYS_MAX 32

#define WORKER_FLAG_DETECTED (1u << 0)
#define WORKER_FLAG_READ_DONE (1u << 1)
#define WORKER_FLAG_STOP (1u << 2)

/* Zelda "secret discovered" jingle — played on successful auto-save. */
static const NotificationSequence nfc_autosave_sequence_zelda = {
    &message_display_backlight_on,
    &message_green_255,
    &message_vibro_on,
    &message_note_g5,
    &message_delay_100,
    &message_vibro_off,
    &message_note_fs5,
    &message_delay_100,
    &message_note_ds5,
    &message_delay_100,
    &message_note_a4,
    &message_delay_100,
    &message_note_gs4,
    &message_delay_100,
    &message_note_e5,
    &message_delay_100,
    &message_note_gs5,
    &message_delay_100,
    &message_note_c6,
    &message_delay_250,
    &message_sound_off,
    &message_green_0,
    NULL,
};

typedef enum {
    NfcAutosaveViewMain,
} NfcAutosaveView;

typedef enum {
    NfcAutosaveStatusIdle,
    NfcAutosaveStatusListening,
    NfcAutosaveStatusReading,
    NfcAutosaveStatusSaving,
    NfcAutosaveStatusSaved,
    NfcAutosaveStatusWaitingRemove,
    NfcAutosaveStatusError,
} NfcAutosaveStatus;

typedef struct NfcAutosaveApp NfcAutosaveApp;

typedef struct {
    NfcAutosaveApp* app;
} NfcAutosaveModel;

typedef enum {
    NfcAutosaveClassicReadKeysNone = 0,
    NfcAutosaveClassicReadKeysSession,
    NfcAutosaveClassicReadKeysSeed,
} NfcAutosaveClassicReadKeysSource;

struct NfcAutosaveApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    NotificationApp* notifications;
    Storage* storage;

    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcDevice* nfc_device;

    FuriThread* worker;
    volatile bool worker_running;

    NfcProtocol detected_protocols[NfcProtocolNum];
    size_t detected_protocol_num;
    NfcProtocol active_protocol;

    KeysDict* classic_dict_user;
    KeysDict* classic_dict_system;
    KeysDict* classic_dict_current;
    bool classic_using_user_dict;
    size_t classic_hot_key_index;
    NfcAutosaveClassicReadKeysSource classic_read_keys_source;
    bool classic_force_dict;
    uint8_t classic_read_sector;
    MfClassicKeyType classic_read_key_type;
    MfClassicData* classic_key_cache;
    uint8_t classic_cache_uid[10];
    size_t classic_cache_uid_len;
    MfClassicData* classic_seed_data;
    uint8_t classic_seed_keys[NFC_AUTOSAVE_SEED_KEYS_MAX][MF_CLASSIC_KEY_SIZE];
    size_t classic_seed_key_count;
    size_t classic_seed_key_index;

    uint8_t last_uid[10];
    size_t last_uid_len;
    char last_name[80];
    char status_detail[64];
    uint32_t save_count;
    volatile NfcAutosaveStatus status;
    volatile bool ui_dirty;
    FuriMutex* state_mutex;
};

/* Tried before user/system dicts — covers most default-keyed Classic cards quickly. */
static const uint8_t nfc_autosave_classic_hot_keys[][MF_CLASSIC_KEY_SIZE] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
};

static const char* nfc_autosave_status_text(NfcAutosaveStatus status) {
    switch(status) {
    case NfcAutosaveStatusListening:
        return "Listening...";
    case NfcAutosaveStatusReading:
        return "Reading...";
    case NfcAutosaveStatusSaving:
        return "Saving...";
    case NfcAutosaveStatusSaved:
        return "Saved!";
    case NfcAutosaveStatusWaitingRemove:
        return "Remove card";
    case NfcAutosaveStatusError:
        return "Error";
    case NfcAutosaveStatusIdle:
    default:
        return "Idle";
    }
}

static void nfc_autosave_view_draw(Canvas* canvas, void* model) {
    NfcAutosaveApp* app = ((NfcAutosaveModel*)model)->app;
    NfcAutosaveStatus status;
    char detail[64];
    uint32_t save_count;

    furi_check(furi_mutex_acquire(app->state_mutex, FuriWaitForever) == FuriStatusOk);
    status = app->status;
    save_count = app->save_count;
    snprintf(detail, sizeof(detail), "%s", app->status_detail);
    app->ui_dirty = false;
    furi_mutex_release(app->state_mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "NFC Autosave");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 16, AlignCenter, AlignTop, nfc_autosave_status_text(status));

    if(detail[0] != '\0') {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, detail);
    }

    char count_line[32];
    snprintf(count_line, sizeof(count_line), "Saved: %lu", (unsigned long)save_count);
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, count_line);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Hold Back = exit");
}

static void nfc_autosave_set_status(
    NfcAutosaveApp* app,
    NfcAutosaveStatus status,
    const char* detail) {
    /* Safe from worker / NFC callbacks: only mutates shared state. GUI redraws on tick. */
    furi_check(furi_mutex_acquire(app->state_mutex, FuriWaitForever) == FuriStatusOk);
    app->status = status;
    if(detail) {
        snprintf(app->status_detail, sizeof(app->status_detail), "%s", detail);
    } else {
        app->status_detail[0] = '\0';
    }
    app->ui_dirty = true;
    furi_mutex_release(app->state_mutex);
}

static void nfc_autosave_tick_callback(void* context) {
    NfcAutosaveApp* app = context;
    if(app->ui_dirty) {
        with_view_model(
            app->view, NfcAutosaveModel * model, { UNUSED(model); }, true);
    }
}

static NfcProtocol nfc_autosave_pick_protocol(const NfcProtocol* protocols, size_t count) {
    /* Prefer the richest dump for festival wristbands. */
    static const NfcProtocol preference[] = {
        NfcProtocolMfUltralight,
        NfcProtocolNtag4xx,
        NfcProtocolMfDesfire,
        NfcProtocolType4Tag,
        NfcProtocolMfClassic,
        NfcProtocolMfPlus,
        NfcProtocolIso14443_4a,
        NfcProtocolIso14443_3a,
        NfcProtocolFelica,
        NfcProtocolIso15693_3,
        NfcProtocolSlix,
        NfcProtocolSt25tb,
        NfcProtocolIso14443_4b,
        NfcProtocolIso14443_3b,
        NfcProtocolEmv,
    };

    for(size_t p = 0; p < COUNT_OF(preference); p++) {
        for(size_t i = 0; i < count; i++) {
            if(protocols[i] == preference[p]) {
                return preference[p];
            }
        }
    }

    return count > 0 ? protocols[0] : NfcProtocolInvalid;
}

static void nfc_autosave_scanner_callback(NfcScannerEvent event, void* context) {
    NfcAutosaveApp* app = context;
    if(event.type != NfcScannerEventTypeDetected) {
        return;
    }

    size_t n = event.data.protocol_num;
    if(n > NfcProtocolNum) {
        n = NfcProtocolNum;
    }
    app->detected_protocol_num = n;
    for(size_t i = 0; i < n; i++) {
        app->detected_protocols[i] = event.data.protocols[i];
    }

    furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_FLAG_DETECTED);
}

static bool nfc_autosave_format_uid(const uint8_t* uid, size_t uid_len, char* out, size_t out_len) {
    if(!uid || uid_len == 0 || uid_len > 10 || out_len < (uid_len * 2 + 1)) {
        return false;
    }
    for(size_t i = 0; i < uid_len; i++) {
        snprintf(out + (i * 2), 3, "%02X", uid[i]);
    }
    return true;
}

static void nfc_autosave_sanitize_type_name(const char* src, char* out, size_t out_len) {
    furi_check(out_len > 1);
    size_t j = 0;
    bool last_us = false;

    for(size_t i = 0; src[i] != '\0' && j + 1 < out_len; i++) {
        char c = src[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
            last_us = false;
        } else if(!last_us) {
            out[j++] = '_';
            last_us = true;
        }
    }

    while(j > 0 && out[j - 1] == '_') {
        j--;
    }
    if(j == 0) {
        snprintf(out, out_len, "NFC");
    } else {
        out[j] = '\0';
    }
}

static bool nfc_autosave_build_path(NfcAutosaveApp* app, FuriString* path) {
    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(app->nfc_device, &uid_len);
    if(!uid || uid_len == 0) {
        return false;
    }

    char uid_hex[24];
    if(!nfc_autosave_format_uid(uid, uid_len, uid_hex, sizeof(uid_hex))) {
        return false;
    }

    const char* type_raw = nfc_device_get_name(app->nfc_device, NfcDeviceNameTypeShort);
    if(!type_raw || type_raw[0] == '\0') {
        type_raw = nfc_device_get_protocol_name(nfc_device_get_protocol(app->nfc_device));
    }
    if(!type_raw || type_raw[0] == '\0') {
        type_raw = "NFC";
    }

    char type_slug[48];
    nfc_autosave_sanitize_type_name(type_raw, type_slug, sizeof(type_slug));

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    furi_string_printf(
        path,
        "%s/%s_%s_%04u%02u%02u_%02u%02u%02u%s",
        NFC_AUTOSAVE_FOLDER,
        type_slug,
        uid_hex,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        NFC_AUTOSAVE_EXTENSION);

    snprintf(
        app->last_name,
        sizeof(app->last_name),
        "%s_%s",
        type_slug,
        uid_hex);
    memcpy(app->last_uid, uid, uid_len);
    app->last_uid_len = uid_len;
    return true;
}

static bool nfc_autosave_save_device(NfcAutosaveApp* app) {
    nfc_autosave_set_status(app, NfcAutosaveStatusSaving, NULL);

    if(!storage_simply_mkdir(app->storage, EXT_PATH("nfc"))) {
        nfc_autosave_set_status(app, NfcAutosaveStatusError, "mkdir nfc fail");
        return false;
    }
    if(!storage_simply_mkdir(app->storage, NFC_AUTOSAVE_FOLDER)) {
        nfc_autosave_set_status(app, NfcAutosaveStatusError, "mkdir lab fail");
        return false;
    }

    FuriString* path = furi_string_alloc();
    bool ok = false;
    if(nfc_autosave_build_path(app, path)) {
        ok = nfc_device_save(app->nfc_device, furi_string_get_cstr(path));
    }
    furi_string_free(path);

    if(ok) {
        furi_check(furi_mutex_acquire(app->state_mutex, FuriWaitForever) == FuriStatusOk);
        app->save_count++;
        furi_mutex_release(app->state_mutex);
        nfc_autosave_set_status(app, NfcAutosaveStatusSaved, app->last_name);
        notification_message(app->notifications, &nfc_autosave_sequence_zelda);
    } else {
        nfc_autosave_set_status(app, NfcAutosaveStatusError, "save failed");
        notification_message(app->notifications, &sequence_error);
    }
    return ok;
}

static bool nfc_autosave_read_iso14443_3a(NfcAutosaveApp* app) {
    Iso14443_3aData* data = iso14443_3a_alloc();
    Iso14443_3aError err = iso14443_3a_poller_sync_read(app->nfc, data);
    if(err == Iso14443_3aErrorNone) {
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(app->nfc_device, NfcProtocolIso14443_3a, data);
    }
    iso14443_3a_free(data);
    return err == Iso14443_3aErrorNone;
}

static bool nfc_autosave_read_ultralight(NfcAutosaveApp* app) {
    MfUltralightData* data = mf_ultralight_alloc();
    MfUltralightPollerAuthContext auth = {
        .skip_auth = true,
        .auth_success = false,
    };
    MfUltralightError err = mf_ultralight_poller_sync_read_card(app->nfc, data, &auth);
    bool ok = (err == MfUltralightErrorNone) || (data->pages_read > 0);
    if(ok) {
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(app->nfc_device, NfcProtocolMfUltralight, data);
    }
    mf_ultralight_free(data);
    return ok;
}

static NfcCommand nfc_autosave_desfire_callback(NfcGenericEvent event, void* context) {
    NfcAutosaveApp* app = context;
    furi_assert(event.protocol == NfcProtocolMfDesfire);
    const MfDesfirePollerEvent* desfire_event = event.event_data;

    if(desfire_event->type == MfDesfirePollerEventTypeReadSuccess) {
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfDesfire, nfc_poller_get_data(app->poller));
        furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_FLAG_READ_DONE);
        return NfcCommandStop;
    }
    if(desfire_event->type == MfDesfirePollerEventTypeReadFailed) {
        /* Still keep whatever public data the poller collected. */
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfDesfire, nfc_poller_get_data(app->poller));
        furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_FLAG_READ_DONE);
        return NfcCommandStop;
    }
    return NfcCommandContinue;
}

static void nfc_autosave_classic_dict_rewind(NfcAutosaveApp* app) {
    app->classic_seed_key_index = 0;
    app->classic_hot_key_index = 0;
    if(app->classic_dict_user) {
        keys_dict_rewind(app->classic_dict_user);
    }
    if(app->classic_dict_system) {
        keys_dict_rewind(app->classic_dict_system);
    }
    app->classic_dict_current = app->classic_dict_user ? app->classic_dict_user :
                                                         app->classic_dict_system;
    app->classic_using_user_dict = (app->classic_dict_current == app->classic_dict_user);
}

static bool nfc_autosave_classic_key_list_add(
    uint8_t keys[][MF_CLASSIC_KEY_SIZE],
    size_t* count,
    size_t max_count,
    const MfClassicKey* key) {
    for(size_t i = 0; i < *count; i++) {
        if(memcmp(keys[i], key->data, MF_CLASSIC_KEY_SIZE) == 0) {
            return false;
        }
    }
    if(*count >= max_count) {
        return false;
    }
    memcpy(keys[*count], key->data, MF_CLASSIC_KEY_SIZE);
    (*count)++;
    return true;
}

static void nfc_autosave_classic_seed_extract_keys(NfcAutosaveApp* app, const MfClassicData* data) {
    app->classic_seed_key_count = 0;
    if(!data) {
        return;
    }

    const uint8_t sectors = mf_classic_get_total_sectors_num(data->type);
    for(uint8_t sector = 0; sector < sectors; sector++) {
        if(mf_classic_is_key_found(data, sector, MfClassicKeyTypeA)) {
            MfClassicKey key = mf_classic_get_key(data, sector, MfClassicKeyTypeA);
            nfc_autosave_classic_key_list_add(
                app->classic_seed_keys,
                &app->classic_seed_key_count,
                NFC_AUTOSAVE_SEED_KEYS_MAX,
                &key);
        }
        if(mf_classic_is_key_found(data, sector, MfClassicKeyTypeB)) {
            MfClassicKey key = mf_classic_get_key(data, sector, MfClassicKeyTypeB);
            nfc_autosave_classic_key_list_add(
                app->classic_seed_keys,
                &app->classic_seed_key_count,
                NFC_AUTOSAVE_SEED_KEYS_MAX,
                &key);
        }
    }
}

static bool nfc_autosave_classic_has_keys(const MfClassicData* data) {
    return data && (data->key_a_mask != 0 || data->key_b_mask != 0);
}

static bool nfc_autosave_classic_load_seed(NfcAutosaveApp* app) {
    if(!storage_file_exists(app->storage, NFC_AUTOSAVE_SEED_PATH)) {
        return false;
    }

    NfcDevice* seed_device = nfc_device_alloc();
    bool ok = false;
    do {
        if(!nfc_device_load(seed_device, NFC_AUTOSAVE_SEED_PATH)) {
            break;
        }
        if(nfc_device_get_protocol(seed_device) != NfcProtocolMfClassic) {
            break;
        }
        const MfClassicData* data =
            nfc_device_get_data(seed_device, NfcProtocolMfClassic);
        if(!nfc_autosave_classic_has_keys(data)) {
            break;
        }
        if(!app->classic_seed_data) {
            app->classic_seed_data = mf_classic_alloc();
        }
        mf_classic_copy(app->classic_seed_data, data);
        nfc_autosave_classic_seed_extract_keys(app, app->classic_seed_data);
        ok = app->classic_seed_key_count > 0;
    } while(0);

    nfc_device_free(seed_device);
    return ok;
}

static void nfc_autosave_classic_seed_store(NfcAutosaveApp* app, const MfClassicData* data) {
    if(!data || !mf_classic_is_card_read(data) || !nfc_autosave_classic_has_keys(data)) {
        return;
    }

    if(!app->classic_seed_data) {
        app->classic_seed_data = mf_classic_alloc();
    }
    mf_classic_copy(app->classic_seed_data, data);
    nfc_autosave_classic_seed_extract_keys(app, app->classic_seed_data);

    /* Persist only if no seed file yet — don't clobber a hand-picked sample. */
    if(!storage_file_exists(app->storage, NFC_AUTOSAVE_SEED_PATH)) {
        storage_simply_mkdir(app->storage, EXT_PATH("nfc"));
        storage_simply_mkdir(app->storage, NFC_AUTOSAVE_FOLDER);

        NfcDevice* seed_device = nfc_device_alloc();
        nfc_device_set_data(seed_device, NfcProtocolMfClassic, data);
        nfc_device_save(seed_device, NFC_AUTOSAVE_SEED_PATH);
        nfc_device_free(seed_device);
    }
}

static bool nfc_autosave_classic_next_key(NfcAutosaveApp* app, MfClassicKey* key) {
    if(app->classic_seed_key_index < app->classic_seed_key_count) {
        memcpy(
            key->data,
            app->classic_seed_keys[app->classic_seed_key_index],
            MF_CLASSIC_KEY_SIZE);
        app->classic_seed_key_index++;
        return true;
    }

    if(app->classic_hot_key_index < COUNT_OF(nfc_autosave_classic_hot_keys)) {
        memcpy(
            key->data,
            nfc_autosave_classic_hot_keys[app->classic_hot_key_index],
            MF_CLASSIC_KEY_SIZE);
        app->classic_hot_key_index++;
        return true;
    }

    uint8_t key_data[MF_CLASSIC_KEY_SIZE];
    while(true) {
        if(app->classic_dict_current &&
           keys_dict_get_next_key(app->classic_dict_current, key_data, MF_CLASSIC_KEY_SIZE)) {
            memcpy(key->data, key_data, MF_CLASSIC_KEY_SIZE);
            return true;
        }

        if(app->classic_using_user_dict && app->classic_dict_system) {
            app->classic_dict_current = app->classic_dict_system;
            app->classic_using_user_dict = false;
            keys_dict_rewind(app->classic_dict_system);
            continue;
        }
        break;
    }
    return false;
}

static bool nfc_autosave_classic_cache_matches_uid(NfcAutosaveApp* app, const MfClassicData* data) {
    if(!app->classic_key_cache || app->classic_cache_uid_len == 0 || !data) {
        return false;
    }
    size_t uid_len = 0;
    const uint8_t* uid = mf_classic_get_uid(data, &uid_len);
    return uid && uid_len == app->classic_cache_uid_len &&
           memcmp(uid, app->classic_cache_uid, uid_len) == 0;
}

static void nfc_autosave_classic_cache_store(NfcAutosaveApp* app, const MfClassicData* data) {
    if(!data || !nfc_autosave_classic_has_keys(data)) {
        return;
    }
    if(!app->classic_key_cache) {
        app->classic_key_cache = mf_classic_alloc();
    }
    mf_classic_copy(app->classic_key_cache, data);

    size_t uid_len = 0;
    const uint8_t* uid = mf_classic_get_uid(data, &uid_len);
    if(uid && uid_len > 0 && uid_len <= sizeof(app->classic_cache_uid)) {
        memcpy(app->classic_cache_uid, uid, uid_len);
        app->classic_cache_uid_len = uid_len;
    }

    nfc_autosave_classic_seed_store(app, data);
}

static const MfClassicData* nfc_autosave_classic_read_keys_data(NfcAutosaveApp* app) {
    if(app->classic_read_keys_source == NfcAutosaveClassicReadKeysSession) {
        return app->classic_key_cache;
    }
    if(app->classic_read_keys_source == NfcAutosaveClassicReadKeysSeed) {
        return app->classic_seed_data;
    }
    return NULL;
}

static bool nfc_autosave_classic_next_template_sector_key(
    NfcAutosaveApp* app,
    uint8_t* sector_num,
    MfClassicKey* key,
    MfClassicKeyType* key_type) {
    const MfClassicData* src = nfc_autosave_classic_read_keys_data(app);
    if(!src) {
        return false;
    }

    const uint8_t sectors_total = mf_classic_get_total_sectors_num(src->type);

    while(app->classic_read_sector < sectors_total) {
        const uint8_t sector = app->classic_read_sector;
        const MfClassicKeyType type = app->classic_read_key_type;

        if(mf_classic_is_key_found(src, sector, type)) {
            *sector_num = sector;
            *key = mf_classic_get_key(src, sector, type);
            *key_type = type;

            if(type == MfClassicKeyTypeA) {
                app->classic_read_key_type = MfClassicKeyTypeB;
            } else {
                app->classic_read_key_type = MfClassicKeyTypeA;
                app->classic_read_sector++;
            }
            return true;
        }

        if(type == MfClassicKeyTypeA) {
            app->classic_read_key_type = MfClassicKeyTypeB;
        } else {
            app->classic_read_key_type = MfClassicKeyTypeA;
            app->classic_read_sector++;
        }
    }
    return false;
}

static NfcCommand nfc_autosave_classic_callback(NfcGenericEvent event, void* context) {
    NfcAutosaveApp* app = context;
    furi_assert(event.protocol == NfcProtocolMfClassic);
    MfClassicPollerEvent* mfc_event = event.event_data;
    NfcCommand command = NfcCommandContinue;

    if(mfc_event->type == MfClassicPollerEventTypeRequestMode) {
        /* Dict/Read both require a non-NULL poller_mode.data (furi_check). */
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfClassic, nfc_poller_get_data(app->poller));
        const MfClassicData* live =
            nfc_device_get_data(app->nfc_device, NfcProtocolMfClassic);
        mfc_event->data->poller_mode.data = live;

        app->classic_read_keys_source = NfcAutosaveClassicReadKeysNone;
        if(!app->classic_force_dict) {
            if(nfc_autosave_classic_cache_matches_uid(app, live) &&
               nfc_autosave_classic_has_keys(app->classic_key_cache)) {
                app->classic_read_keys_source = NfcAutosaveClassicReadKeysSession;
            } else if(nfc_autosave_classic_has_keys(app->classic_seed_data)) {
                /* Same-type wristbands often share the seed's sector key map. */
                app->classic_read_keys_source = NfcAutosaveClassicReadKeysSeed;
            }
        }

        if(app->classic_read_keys_source != NfcAutosaveClassicReadKeysNone) {
            mfc_event->data->poller_mode.mode = MfClassicPollerModeRead;
            app->classic_read_sector = 0;
            app->classic_read_key_type = MfClassicKeyTypeA;
            nfc_autosave_set_status(
                app,
                NfcAutosaveStatusReading,
                app->classic_read_keys_source == NfcAutosaveClassicReadKeysSeed ?
                    "Classic seed" :
                    "Classic cached");
        } else {
            mfc_event->data->poller_mode.mode = MfClassicPollerModeDictAttackStandard;
            nfc_autosave_classic_dict_rewind(app);
            nfc_autosave_set_status(app, NfcAutosaveStatusReading, "Classic dict");
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestReadSector) {
        uint8_t sector = 0;
        MfClassicKey key = {};
        MfClassicKeyType key_type = MfClassicKeyTypeA;
        if(nfc_autosave_classic_next_template_sector_key(app, &sector, &key, &key_type)) {
            mfc_event->data->read_sector_request_data.sector_num = sector;
            mfc_event->data->read_sector_request_data.key = key;
            mfc_event->data->read_sector_request_data.key_type = key_type;
            mfc_event->data->read_sector_request_data.key_provided = true;
        } else {
            mfc_event->data->read_sector_request_data.key_provided = false;
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestKey) {
        MfClassicKey key = {};
        if(nfc_autosave_classic_next_key(app, &key)) {
            mfc_event->data->key_request_data.key = key;
            mfc_event->data->key_request_data.key_provided = true;
        } else {
            mfc_event->data->key_request_data.key_provided = false;
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeNextSector) {
        nfc_autosave_classic_dict_rewind(app);
        char detail[32];
        snprintf(
            detail,
            sizeof(detail),
            "sec %u",
            mfc_event->data->next_sector_data.current_sector);
        nfc_autosave_set_status(app, NfcAutosaveStatusReading, detail);
    } else if(mfc_event->type == MfClassicPollerEventTypeDataUpdate) {
        char detail[32];
        snprintf(
            detail,
            sizeof(detail),
            "k%u s%u",
            mfc_event->data->data_update.keys_found,
            mfc_event->data->data_update.sectors_read);
        nfc_autosave_set_status(app, NfcAutosaveStatusReading, detail);
    } else if(
        mfc_event->type == MfClassicPollerEventTypeSuccess ||
        mfc_event->type == MfClassicPollerEventTypeFail) {
        nfc_device_clear(app->nfc_device);
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfClassic, nfc_poller_get_data(app->poller));

        if(mfc_event->type == MfClassicPollerEventTypeSuccess) {
            nfc_autosave_classic_cache_store(
                app, nfc_device_get_data(app->nfc_device, NfcProtocolMfClassic));
        }

        furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_FLAG_READ_DONE);
        command = NfcCommandStop;
    }

    return command;
}

static bool nfc_autosave_read_with_poller(
    NfcAutosaveApp* app,
    NfcProtocol protocol,
    NfcGenericCallback callback) {
    app->poller = nfc_poller_alloc(app->nfc, protocol);
    furi_thread_flags_clear(WORKER_FLAG_READ_DONE | WORKER_FLAG_STOP);
    nfc_poller_start(app->poller, callback, app);

    uint32_t flags = furi_thread_flags_wait(
        WORKER_FLAG_READ_DONE | WORKER_FLAG_STOP, FuriFlagWaitAny, FuriWaitForever);

    nfc_poller_stop(app->poller);
    nfc_poller_free(app->poller);
    app->poller = NULL;

    return (flags & WORKER_FLAG_READ_DONE) != 0;
}

static bool nfc_autosave_read_classic(NfcAutosaveApp* app) {
    nfc_autosave_set_status(app, NfcAutosaveStatusReading, "Mifare Classic");
    bool ok =
        nfc_autosave_read_with_poller(app, NfcProtocolMfClassic, nfc_autosave_classic_callback);
    if(!ok || !app->worker_running) {
        return ok;
    }

    /* Template/cached keys incomplete → immediately run dict (seed/hot/user/system). */
    const MfClassicData* data =
        nfc_device_get_data(app->nfc_device, NfcProtocolMfClassic);
    if(app->classic_read_keys_source != NfcAutosaveClassicReadKeysNone && data &&
       !mf_classic_is_card_read(data)) {
        /* Seed/session template missed some sectors → full dict with seed keys first. */
        if(app->classic_read_keys_source == NfcAutosaveClassicReadKeysSession) {
            app->classic_cache_uid_len = 0;
        }
        app->classic_read_keys_source = NfcAutosaveClassicReadKeysNone;
        app->classic_force_dict = true;
        nfc_autosave_set_status(app, NfcAutosaveStatusReading, "Classic dict");
        ok = nfc_autosave_read_with_poller(
            app, NfcProtocolMfClassic, nfc_autosave_classic_callback);
        app->classic_force_dict = false;
    }
    return ok;
}

static bool nfc_autosave_read_card(NfcAutosaveApp* app, NfcProtocol protocol) {
    app->active_protocol = protocol;
    nfc_autosave_set_status(app, NfcAutosaveStatusReading, nfc_device_get_protocol_name(protocol));

    if(protocol == NfcProtocolMfUltralight) {
        return nfc_autosave_read_ultralight(app);
    }
    if(protocol == NfcProtocolIso14443_3a) {
        return nfc_autosave_read_iso14443_3a(app);
    }
    if(protocol == NfcProtocolMfDesfire) {
        return nfc_autosave_read_with_poller(app, protocol, nfc_autosave_desfire_callback);
    }
    if(protocol == NfcProtocolMfClassic) {
        return nfc_autosave_read_classic(app);
    }

    /* Unsupported rich protocol → at least capture ISO14443-3A identity. */
    return nfc_autosave_read_iso14443_3a(app);
}

static void nfc_autosave_wait_card_removed(NfcAutosaveApp* app) {
    nfc_autosave_set_status(app, NfcAutosaveStatusWaitingRemove, app->last_name);

    while(app->worker_running) {
        if(furi_thread_flags_get() & WORKER_FLAG_STOP) {
            break;
        }

        Iso14443_3aData* data = iso14443_3a_alloc();
        Iso14443_3aError err = iso14443_3a_poller_sync_read(app->nfc, data);
        bool same = false;
        if(err == Iso14443_3aErrorNone) {
            size_t uid_len = 0;
            const uint8_t* uid = iso14443_3a_get_uid(data, &uid_len);
            if(uid && uid_len == app->last_uid_len &&
               memcmp(uid, app->last_uid, uid_len) == 0) {
                same = true;
            }
        }
        iso14443_3a_free(data);

        if(!same) {
            break;
        }
        furi_delay_ms(200);
    }
}

static int32_t nfc_autosave_worker(void* context) {
    NfcAutosaveApp* app = context;

    app->classic_dict_user = NULL;
    app->classic_dict_system = NULL;
    if(keys_dict_check_presence(NFC_CLASSIC_DICT_USER)) {
        app->classic_dict_user =
            keys_dict_alloc(NFC_CLASSIC_DICT_USER, KeysDictModeOpenExisting, MF_CLASSIC_KEY_SIZE);
    }
    if(keys_dict_check_presence(NFC_CLASSIC_DICT_SYSTEM)) {
        app->classic_dict_system = keys_dict_alloc(
            NFC_CLASSIC_DICT_SYSTEM, KeysDictModeOpenExisting, MF_CLASSIC_KEY_SIZE);
    }

    if(nfc_autosave_classic_load_seed(app)) {
        char detail[32];
        snprintf(detail, sizeof(detail), "seed %u keys", (unsigned)app->classic_seed_key_count);
        nfc_autosave_set_status(app, NfcAutosaveStatusListening, detail);
    }

    while(app->worker_running) {
        if(app->classic_seed_key_count > 0) {
            char detail[40];
            snprintf(
                detail,
                sizeof(detail),
                "seed:%u hold tag",
                (unsigned)app->classic_seed_key_count);
            nfc_autosave_set_status(app, NfcAutosaveStatusListening, detail);
        } else {
            nfc_autosave_set_status(app, NfcAutosaveStatusListening, "hold tag to back");
        }
        notification_message(app->notifications, &sequence_blink_start_cyan);

        app->scanner = nfc_scanner_alloc(app->nfc);
        furi_thread_flags_clear(WORKER_FLAG_DETECTED | WORKER_FLAG_STOP);
        nfc_scanner_start(app->scanner, nfc_autosave_scanner_callback, app);

        uint32_t flags = furi_thread_flags_wait(
            WORKER_FLAG_DETECTED | WORKER_FLAG_STOP, FuriFlagWaitAny, FuriWaitForever);

        notification_message(app->notifications, &sequence_blink_stop);
        nfc_scanner_stop(app->scanner);
        nfc_scanner_free(app->scanner);
        app->scanner = NULL;

        if((flags & WORKER_FLAG_STOP) || !app->worker_running) {
            break;
        }

        NfcProtocol protocol =
            nfc_autosave_pick_protocol(app->detected_protocols, app->detected_protocol_num);
        if(protocol == NfcProtocolInvalid) {
            continue;
        }

        bool read_ok = nfc_autosave_read_card(app, protocol);
        if(!app->worker_running) {
            break;
        }

        if(read_ok) {
            nfc_autosave_save_device(app);
            nfc_autosave_wait_card_removed(app);
        } else {
            nfc_autosave_set_status(app, NfcAutosaveStatusError, "read failed");
            notification_message(app->notifications, &sequence_error);
            furi_delay_ms(800);
        }
    }

    if(app->classic_dict_user) {
        keys_dict_free(app->classic_dict_user);
        app->classic_dict_user = NULL;
    }
    if(app->classic_dict_system) {
        keys_dict_free(app->classic_dict_system);
        app->classic_dict_system = NULL;
    }

    return 0;
}

static void nfc_autosave_stop_worker(NfcAutosaveApp* app) {
    app->worker_running = false;
    if(app->worker) {
        furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_FLAG_STOP);
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

static bool nfc_autosave_nav_callback(void* context) {
    UNUSED(context);
    /* Swallow short Back so pocket presses don't exit. */
    return true;
}

static bool nfc_autosave_input_callback(InputEvent* event, void* context) {
    NfcAutosaveApp* app = context;
    if(event->key == InputKeyBack) {
        if(event->type == InputTypeLong) {
            view_dispatcher_stop(app->view_dispatcher);
        }
        return true;
    }
    return false;
}

static NfcAutosaveApp* nfc_autosave_app_alloc(void) {
    NfcAutosaveApp* app = malloc(sizeof(NfcAutosaveApp));
    memset(app, 0, sizeof(NfcAutosaveApp));

    app->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nfc_autosave_nav_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, nfc_autosave_tick_callback, 100);

    app->view = view_alloc();
    view_set_context(app->view, app);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(NfcAutosaveModel));
    view_set_draw_callback(app->view, nfc_autosave_view_draw);
    view_set_input_callback(app->view, nfc_autosave_input_callback);
    with_view_model(
        app->view, NfcAutosaveModel * model, { model->app = app; }, false);
    view_dispatcher_add_view(app->view_dispatcher, NfcAutosaveViewMain, app->view);

    app->nfc = nfc_alloc();
    app->nfc_device = nfc_device_alloc();

    return app;
}

static void nfc_autosave_app_free(NfcAutosaveApp* app) {
    nfc_autosave_stop_worker(app);

    if(app->scanner) {
        nfc_scanner_stop(app->scanner);
        nfc_scanner_free(app->scanner);
    }
    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
    }

    nfc_device_free(app->nfc_device);
    nfc_free(app->nfc);
    if(app->classic_key_cache) {
        mf_classic_free(app->classic_key_cache);
    }
    if(app->classic_seed_data) {
        mf_classic_free(app->classic_seed_data);
    }

    view_dispatcher_remove_view(app->view_dispatcher, NfcAutosaveViewMain);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(app->state_mutex);
    free(app);
}

int32_t nfc_autosave_app(void* p) {
    UNUSED(p);

    NfcAutosaveApp* app = nfc_autosave_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, NfcAutosaveViewMain);
    nfc_autosave_set_status(app, NfcAutosaveStatusListening, "hold tag to back");
    with_view_model(
        app->view, NfcAutosaveModel * model, { UNUSED(model); }, true);

    app->worker_running = true;
    app->worker = furi_thread_alloc_ex("NfcAutosaveWorker", 8 * 1024, nfc_autosave_worker, app);
    furi_thread_start(app->worker);

    view_dispatcher_run(app->view_dispatcher);

    nfc_autosave_stop_worker(app);
    notification_message(app->notifications, &sequence_blink_stop);
    nfc_autosave_app_free(app);
    return 0;
}
