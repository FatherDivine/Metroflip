// Parser for CTA Ventra Ultralight cards
// Made by @hazardousvoltage
// Based on my own research, with...
// Credit to https://www.lenrek.net/experiments/compass-tickets/ & MetroDroid project for underlying info
// Credit to @FatherDivine (Github) for Metroflip integration, stop IDs & stop names database, and code tweaks.
//
// This parser can decode the paper single-use and single/multi-day paper passes using Ultralight EV1
// The plastic cards are DESFire and fully locked down, not much useful info extractable
//
// Chicago Transit Authority (CTA) service types:
//   B = Bus service
//   T = Train (Rail) service - specifically the CTA's "L" elevated/subway system
//
// Station database files location:
//   files/ventra/stations/bus/stations.txt   - Bus stops (decimal Cubic IDs)
//   files/ventra/stations/train/stations.txt - Rail/"L" stations (hex Cubic IDs)

#include <flipper_application.h>
#include "../../metroflip_i.h"

#include <dolphin/dolphin.h>
#include <bit_lib.h>
#include <furi_hal.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include "../../api/metroflip/metroflip_api.h"
#include "../../metroflip_plugins.h"

#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>

#include <storage/storage.h>
#include <stream/stream.h>
#include <stream/file_stream.h>
#include <stdio.h>
#include <string.h>

#define TAG "Metroflip:Scene:Ventra"

// Path to station database files (bundled with app via fap_file_assets)
#define VENTRA_BUS_STATIONS_PATH    APP_ASSETS_PATH("ventra/stations/bus/stations.txt")
#define VENTRA_TRAIN_STATIONS_PATH  APP_ASSETS_PATH("ventra/stations/train/stations.txt")

// Static global state for parsing (reset on each parse)
static DateTime ventra_exp_date = {0}, ventra_validity_date = {0};
static uint8_t ventra_high_seq = 0, ventra_cur_blk = 0, ventra_mins_active = 0;

static uint32_t ventra_time_now(void) {
    return furi_hal_rtc_get_timestamp();
}

static DateTime ventra_dt_delta(DateTime dt, uint8_t delta_days) {
    // returns shifted DateTime, from initial DateTime and time offset in seconds
    DateTime dt_shifted = {0};
    datetime_timestamp_to_datetime(
        datetime_datetime_to_timestamp(&dt) - (uint64_t)delta_days * 86400, &dt_shifted);
    return dt_shifted;
}

/*
static long dt_diff(DateTime dta, DateTime dtb) {
    // returns difference in seconds between two DateTimes
    long diff;
    diff = datetime_datetime_to_timestamp(&dta) - datetime_datetime_to_timestamp(&dtb); 
    return diff;
}
*/

// Card is expired if:
// - Hard expiration date passed (90 days from purchase, encoded in product record)
// - Soft expiration date passed:
//   - For passes, n days after first use
//   - For tickets, 2 hours after first use
//   Calculating these is dumber than it needs to be, see xact record parser.
static bool ventra_is_expired(void) {
    uint32_t ts_hard_exp = datetime_datetime_to_timestamp(&ventra_exp_date);
    uint32_t ts_soft_exp = datetime_datetime_to_timestamp(&ventra_validity_date);
    uint32_t ts_now = ventra_time_now();
    return (ts_now >= ts_hard_exp || ts_now > ts_soft_exp);
}

/********************************************************************
 * Stop database: CSV-on-demand lookup
 ********************************************************************/

// Simple line reader for Storage File API.
// Reads until newline or EOF, null-terminates buffer.
// Returns true if a line was read, false on EOF/no data.
static bool ventra_read_line(File* file, char* buf, size_t buf_size) {
    if(buf_size == 0) return false;

    size_t pos = 0;
    char ch;
    size_t read_len = 0;

    while(true) {
        read_len = storage_file_read(file, &ch, 1);
        if(read_len == 0) {
            // EOF
            break;
        }

        if(ch == '\r') {
            // ignore CR, but keep going (handle CRLF)
            continue;
        }

        if(ch == '\n') {
            // end of line
            break;
        }

        if(pos < buf_size - 1) {
            buf[pos++] = ch;
        } else {
            // line too long, keep consuming but don't overflow
        }
    }

    if(pos == 0 && read_len == 0) {
        // nothing read and EOF
        return false;
    }

    buf[pos] = '\0';
    return true;
}

/* Station lookup for bus (decimal) and train (hex) IDs.
 *
 * File format (IDs stored as exact strings):
 *   Bus:   16959,Harlem & Addison
 *   Train: 003B,Jefferson Park
 *
 * Line type determines which file to search:
 *   line == 1 → Train (VENTRA_TRAIN_STATIONS_PATH)
 *   line == 2 → Bus (VENTRA_BUS_STATIONS_PATH)
 */
static bool ventra_lookup_stop_name_str(const char* id_str, uint8_t line_type, char* out_name, size_t out_size) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return false;

    const char* file_path = (line_type == 1) ? VENTRA_TRAIN_STATIONS_PATH : VENTRA_BUS_STATIONS_PATH;

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char line[256];
    bool found = false;

    while(ventra_read_line(file, line, sizeof(line))) {
        // Skip comment lines
        if(line[0] == '#') continue;

        char* comma = strchr(line, ',');
        if(!comma) continue;

        *comma = '\0';
        const char* csv_id = line;
        const char* csv_name = comma + 1;

        while(*csv_name == ' ' || *csv_name == '\t')
            csv_name++;

        if(strcmp(csv_id, id_str) == 0) {
            strncpy(out_name, csv_name, out_size);
            out_name[out_size - 1] = '\0';
            found = true;
            break;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return found;
}

/********************************************************************
 * Original Ventra transaction parsing, with stop-name injection
 ********************************************************************/

/* Format Cubic stop IDs into lookup keys:
 *   Bus   → decimal string
 *   Train → 4‑digit uppercase hex
 */
static void ventra_format_id(uint16_t locus, uint8_t line, char* out, size_t out_size) {
    if(line == 2) {
        // Bus → decimal
        snprintf(out, out_size, "%u", (unsigned int)locus);
    } else if(line == 1) {
        // Train → 4‑digit uppercase hex
        snprintf(out, out_size, "%04X", (unsigned int)locus);
    } else {
        // Purchases or unknown → fallback decimal
        snprintf(out, out_size, "%u", (unsigned int)locus);
    }
}

static FuriString* ventra_parse_xact(const MfUltralightData* data, uint8_t blk, bool is_pass) {
    FuriString* ventra_xact_str = furi_string_alloc();
    uint16_t ts = data->page[blk].data[0] | data->page[blk].data[1] << 8;
    uint8_t tran_type = ts & 0x1F;
    ts >>= 5;
    uint8_t day = data->page[blk].data[2];
    uint32_t work = data->page[blk + 1].data[0] | data->page[blk + 1].data[1] << 8 |
                    data->page[blk + 1].data[2] << 16;
    uint8_t seq = work & 0x7F;
    uint16_t exp = (work >> 7) & 0x7FF;
    uint8_t exp_day = data->page[blk + 2].data[0];
    uint16_t locus = data->page[blk + 2].data[1] | data->page[blk + 2].data[2] << 8;
    uint8_t line = data->page[blk + 2].data[3];

    // This computes the block timestamp, based on the card expiration date and delta from it
    DateTime dt = ventra_dt_delta(ventra_exp_date, day);
    dt.hour = (ts & 0x7FF) / 60;
    dt.minute = (ts & 0x7FF) % 60;

    // If sequence is 0, block isn't used yet (new card with only one active block, typically the first one.
    // Otherwise, the block with higher sequence is the latest transaction, and the other block is prior transaction.
    // Not necessarily in that order on the card.  We need the latest data to compute validity and pretty-print them
    // in reverse chrono.  So this mess sets some globals as to which block is current, computes the validity times, etc.
    if(seq == 0) {
        furi_string_printf(ventra_xact_str, "-- EMPTY --");
        return (ventra_xact_str);
    }
    if(seq > ventra_high_seq) {
        ventra_high_seq = seq;
        ventra_cur_blk = blk;
        ventra_mins_active = data->page[blk + 1].data[3];
        // Figure out the soft expiration.  For passes it's easy, the readers update the "exp" field in the transaction record.
        // Tickets, not so much, readers don't update "exp", but each xact record has "minutes since last tap" which is
        // updated and carried forward.  That, plus transaction timestamp, gives the expiration time.
        if(tran_type == 6) { // Furthermore, purchase transactions set bogus expiration dates
            if(is_pass) {
                ventra_validity_date = ventra_dt_delta(ventra_exp_date, exp_day);
                ventra_validity_date.hour = (exp & 0x7FF) / 60;
                ventra_validity_date.minute = (exp & 0x7FF) % 60;
            } else {
                uint32_t validity_ts = datetime_datetime_to_timestamp(&dt);
                validity_ts += (120 - ventra_mins_active) * 60;
                datetime_timestamp_to_datetime(validity_ts, &ventra_validity_date);
            }
        }
    }

    // Type 0 = Purchase, 1 = Train (T), 2 = Bus (B)
    // CTA uses "T" for Train (Rail/"L" system) and "B" for Bus
    char linemap[3] = "PTB";

    // Unified bus/train stop lookup
    char stop_name[128];
    bool have_name = false;
    char id_key[16];

    // Convert locus → lookup key (decimal for bus, hex for train)
    ventra_format_id(locus, line, id_key, sizeof(id_key));

    // Look up the formatted key in the appropriate station file
    have_name = ventra_lookup_stop_name_str(id_key, line, stop_name, sizeof(stop_name));

    if(have_name) {
        // Use the station name + the exact ID key used for lookup
        furi_string_printf(
            ventra_xact_str,
            "%c %s\n  %04d-%02d-%02d %02d:%02d",
            (line < 3) ? linemap[line] : '?',
            stop_name,
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute);
    } else {
        // Fallback to original formatting
        if(line == 2) {
            furi_string_printf(
                ventra_xact_str,
                "%c %u %04d-%02d-%02d %02d:%02d",
                (line < 3) ? linemap[line] : '?',
                (unsigned int)locus,
                dt.year,
                dt.month,
                dt.day,
                dt.hour,
                dt.minute);
        } else {
            furi_string_printf(
                ventra_xact_str,
                "%c %04X %04d-%02d-%02d %02d:%02d",
                (line < 3) ? linemap[line] : '?',
                (unsigned int)locus,
                dt.year,
                dt.month,
                dt.day,
                dt.hour,
                dt.minute);
        }
    }

    return (ventra_xact_str);
}

// Reset global state before each parse
static void ventra_reset_state(void) {
    ventra_exp_date = (DateTime){0};
    ventra_validity_date = (DateTime){0};
    ventra_high_seq = 0;
    ventra_cur_blk = 0;
    ventra_mins_active = 0;
}

static bool ventra_parse(FuriString* parsed_data, const MfUltralightData* data) {
    furi_assert(parsed_data);
    furi_assert(data);

    // Reset state for fresh parse
    ventra_reset_state();

    bool parsed = false;

    do {
        // This test can probably be improved -- it matches every Ventra I've seen, but will also match others
        // in the same family.  Or maybe we just generalize this parser.
        if(data->page[4].data[0] != 0x0A || data->page[4].data[1] != 4 ||
           data->page[4].data[2] != 0 || data->page[6].data[0] != 0 ||
           data->page[6].data[1] != 0 || data->page[6].data[2] != 0) {
            FURI_LOG_D(TAG, "Not Ventra Ultralight");
            break;
        }

        // Parse the product record, display interesting data & extract info needed to parse transaction blocks
        // Had this in its own function, ended up just setting a bunch of shitty globals, so inlined it instead.
        FuriString* ventra_prod_str = furi_string_alloc();
        uint8_t otp = data->page[3].data[0];
        uint8_t prod_code = data->page[5].data[2];
        bool is_pass = false;
        switch(prod_code) {
        case 2:
        case 0x1F: // Only ever seen one of these, it parses like a Single
            furi_string_cat_printf(ventra_prod_str, "Single");
            break;
        case 0x01: // gleamed from a single-use ticket purchased 12-06-2025 (FatherDivine)
            furi_string_cat_printf(ventra_prod_str, "Single");
            break;
        case 3:
        case 0x3F:
            is_pass = true;
            furi_string_cat_printf(ventra_prod_str, "1-Day");
            break;
        case 4: // Last I checked, 3 day passes only available at airport TVMs & social service agencies
            is_pass = true;
            furi_string_cat_printf(ventra_prod_str, "3-Day");
            break;
        default:
            is_pass =
                true; // There are some card types I don't know what they are, but they parse like a pass, not a ticket.
            furi_string_cat_printf(ventra_prod_str, "0x%02X", data->page[5].data[2]);
            break;
        }

        uint16_t date_y = data->page[4].data[3] | (data->page[5].data[0] << 8);
        uint8_t date_d = date_y & 0x1F;
        uint8_t date_m = (date_y >> 5) & 0x0F;
        date_y >>= 9;
        date_y += 2000;
        ventra_exp_date.day = date_d;
        ventra_exp_date.month = date_m;
        ventra_exp_date.year = date_y;
        ventra_validity_date = ventra_exp_date; // Until we know otherwise

        // Parse the transaction blocks.  This sets a few sloppy globals, but it's too complex and repetitive to inline.
        FuriString* ventra_xact_str1 = ventra_parse_xact(data, 8, is_pass);
        FuriString* ventra_xact_str2 = ventra_parse_xact(data, 12, is_pass);

        uint8_t card_state = 1;
        uint8_t rides_left = 0;

        char* card_states[5] = {"???", "NEW", "ACT", "USED", "EXP"};

        if(ventra_high_seq > 1) card_state = 2;
        // On "ticket" product, the OTP bits mark off rides used.  Bit 0 seems to be unused, the next 3 are set as rides are used.
        // Some, not all, readers will set the high bits to 0x7 when a card is tapped after it's expired or depleted.  Have not
        // seen other combinations, but if we do, we'll make a nice ???.  1-day passes set the OTP bit 1 on first use.  3-day
        // passes do not.  But we don't really care, since they don't matter on passes, unless you're trying to rollback one.
        if(!is_pass) {
            switch(otp) {
            case 0:
                rides_left = 3;
                break;
            case 2:
                card_state = 2;
                rides_left = 2;
                break;
            case 6:
                card_state = 2;
                rides_left = 1;
                break;
            case 0x0E:
            case 0x7E:
                card_state = 3;
                rides_left = 0;
                break;
            default:
                card_state = 0;
                rides_left = 0;
                break;
            }
        }
        if(ventra_is_expired()) {
            card_state = 4;
            rides_left = 0;
        }

        furi_string_printf(
            parsed_data,
            "\e#Ventra %s (%s)\n",
            furi_string_get_cstr(ventra_prod_str),
            card_states[card_state]);

        furi_string_cat_printf(
            parsed_data,
            "Exp: %04d-%02d-%02d %02d:%02d\n",
            ventra_validity_date.year,
            ventra_validity_date.month,
            ventra_validity_date.day,
            ventra_validity_date.hour,
            ventra_validity_date.minute);

        if(rides_left) {
            furi_string_cat_printf(parsed_data, "Rides left: %d\n", rides_left);
        }

        furi_string_cat_printf(
            parsed_data,
            "%s\n",
            furi_string_get_cstr(ventra_cur_blk == 8 ? ventra_xact_str1 : ventra_xact_str2));

        furi_string_cat_printf(
            parsed_data,
            "%s\n",
            furi_string_get_cstr(ventra_cur_blk == 8 ? ventra_xact_str2 : ventra_xact_str1));

        furi_string_cat_printf(
            parsed_data, "TVM ID: %02X%02X\n", data->page[7].data[1], data->page[7].data[0]);
        furi_string_cat_printf(parsed_data, "Tx count: %d\n", ventra_high_seq);
        furi_string_cat_printf(
            parsed_data,
            "Hard Expiry: %04d-%02d-%02d",
            ventra_exp_date.year,
            ventra_exp_date.month,
            ventra_exp_date.day);

        furi_string_free(ventra_prod_str);
        furi_string_free(ventra_xact_str1);
        furi_string_free(ventra_xact_str2);

        parsed = true;
    } while(false);

    return parsed;
}

static NfcCommand ventra_poller_callback(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolMfUltralight);

    Metroflip* app = context;
    const MfUltralightPollerEvent* mf_ultralight_event = event.event_data;

    if(mf_ultralight_event->type == MfUltralightPollerEventTypeReadSuccess) {
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfUltralight, nfc_poller_get_data(app->poller));

        const MfUltralightData* data =
            nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
        uint32_t poller_event = (data->pages_read == data->pages_total) ?
                                    MetroflipCustomEventPollerSuccess :
                                    MetroflipCustomEventPollerFail;
        view_dispatcher_send_custom_event(app->view_dispatcher, poller_event);
        return NfcCommandStop;
    } else if(mf_ultralight_event->type == MfUltralightPollerEventTypeAuthRequest) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventPollerFail);
    } else if(mf_ultralight_event->type == MfUltralightPollerEventTypeAuthSuccess) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventPollerSuccess);
    }

    return NfcCommandContinue;
}

static void ventra_on_enter(Metroflip* app) {
    dolphin_deed(DolphinDeedNfcRead);

    if(app->data_loaded) {
        FURI_LOG_I(TAG, "Ventra data loaded from file");
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FlipperFormat* ff = flipper_format_file_alloc(storage);
        if(flipper_format_file_open_existing(ff, app->file_path)) {
            MfUltralightData* ultralight_data = mf_ultralight_alloc();
            mf_ultralight_load(ultralight_data, ff, 2);
            FuriString* parsed_data = furi_string_alloc();
            Widget* widget = app->widget;

            furi_string_reset(app->text_box_store);
            if(!ventra_parse(parsed_data, ultralight_data)) {
                furi_string_reset(app->text_box_store);
                FURI_LOG_I(TAG, "Unknown card type");
                furi_string_printf(parsed_data, "\e#Unknown card\n");
            }
            widget_add_text_scroll_element(
                widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

            widget_add_button_element(
                widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Delete", metroflip_delete_widget_callback, app);
            mf_ultralight_free(ultralight_data);
            furi_string_free(parsed_data);
            view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        }
        flipper_format_free(ff);
        furi_record_close(RECORD_STORAGE);
    } else {
        FURI_LOG_I(TAG, "Ventra waiting for card");
        // Setup view
        Popup* popup = app->popup;
        popup_set_header(popup, "Apply\n card to\nthe back", 68, 30, AlignLeft, AlignTop);
        popup_set_icon(popup, 0, 3, &I_RFIDDolphinReceive_97x61);

        // Start worker
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewPopup);
        app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
        nfc_poller_start(app->poller, ventra_poller_callback, app);

        metroflip_app_blink_start(app);
    }
}

static bool ventra_on_event(Metroflip* app, SceneManagerEvent event) {
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventCardDetected) {
            Popup* popup = app->popup;
            popup_set_header(popup, "DON'T\nMOVE", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerSuccess) {
            const MfUltralightData* ultralight_data =
                nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
            FuriString* parsed_data = furi_string_alloc();
            Widget* widget = app->widget;

            furi_string_reset(app->text_box_store);
            if(!ventra_parse(parsed_data, ultralight_data)) {
                furi_string_reset(app->text_box_store);
                FURI_LOG_I(TAG, "Unknown card type");
                furi_string_printf(parsed_data, "\e#Unknown card\n");
            }
            widget_add_text_scroll_element(
                widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Exit", metroflip_exit_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Save", metroflip_save_widget_callback, app);
            furi_string_free(parsed_data);
            view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
            metroflip_app_blink_stop(app);
            consumed = true;
        } else if(event.event == MetroflipCustomEventCardLost) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Card \n lost", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventWrongCard) {
            Popup* popup = app->popup;
            popup_set_header(popup, "WRONG \n CARD", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerFail) {
            FuriString* parsed_data = furi_string_alloc();
            Widget* widget = app->widget;

            furi_string_reset(app->text_box_store);
            FURI_LOG_I(TAG, "Unknown card type");
            furi_string_printf(parsed_data, "\e#Unknown card\n");

            widget_add_text_scroll_element(
                widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
            furi_string_free(parsed_data);
            view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
            metroflip_app_blink_stop(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        consumed = true;
    }

    return consumed;
}

static void ventra_on_exit(Metroflip* app) {
    widget_reset(app->widget);

    if(app->poller && !app->data_loaded) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
    }

    // Clear view
    popup_reset(app->popup);

    metroflip_app_blink_stop(app);
}

/* Actual implementation of app<>plugin interface */
static const MetroflipPlugin ventra_plugin = {
    .card_name = "Ventra",
    .plugin_on_enter = ventra_on_enter,
    .plugin_on_event = ventra_on_event,
    .plugin_on_exit = ventra_on_exit,
};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor ventra_plugin_descriptor = {
    .appid = METROFLIP_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = METROFLIP_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &ventra_plugin,
};

/* Plugin entry point - must return a pointer to const descriptor  */
const FlipperAppPluginDescriptor* ventra_plugin_ep(void) {
    return &ventra_plugin_descriptor;
}
