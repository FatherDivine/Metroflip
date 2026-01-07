#include "../metroflip_i.h"
#include <furi.h>
#include "../metroflip_plugins.h"
#include "../api/metroflip/metroflip_api.h"
#define TAG "Metroflip:Scene:Parse"
#include <stdio.h>

void metroflip_scene_parse_on_enter(void* context) {
    Metroflip* app = context;
    
    // Initialize plugin_loaded to false at the start
    app->plugin_loaded = false;
    
    FURI_LOG_I(TAG, "Parse scene entered - card_type: %s, data_loaded: %s", 
               app->card_type ? app->card_type : "NULL", 
               app->data_loaded ? "true" : "false");

    if(!app->card_type ||
   (app->card_type[0] == '\0') ||
   (strcmp(app->card_type, "unknown") == 0) ||
   (strcmp(app->card_type, "Unknown Card") == 0) ||
   (app->is_desfire && is_desfire_locked(app->card_type))) {
        FURI_LOG_I(TAG, "Bad card condition met - sending wrong card event");
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
    } else {
        if((strcmp(app->card_type, "atr") == 0) && app->hist_bytes_count > 0) {
            FURI_LOG_I(TAG, "Tag is either T-Mobilitat or T-Money");
            if(app->hist_bytes[0] == 0x2A && app->hist_bytes[1] == 0x26){
                FURI_LOG_I(TAG, "Card is T-Mobilitat");
                app->card_type = "tmobilitat";
                
            } else if (app->hist_bytes[0] == 0x04 && app->hist_bytes[1] == 0x09) {
                FURI_LOG_I(TAG, "Card is T-Money");

                app->card_type = "tmoney"; 
                //for now we blank out the line above as it's not merged yet
                view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
                return;
            } else {
                view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
                return;
            }
        }
        FURI_LOG_I(TAG, "Card is valid, loading plugin for: %s", app->card_type);
        metroflip_plugin_manager_alloc(app);
        char path[128]; // Adjust size as needed
        snprintf(
            path, sizeof(path), "/ext/apps_assets/metroflip/plugins/%s_plugin.fal", app->card_type);

        FURI_LOG_I(TAG, "Plugin path: %s", path);

        // Try loading the plugin
        if(plugin_manager_load_single(app->plugin_manager, path) != PluginManagerErrorNone) {
            FURI_LOG_E(TAG, "Failed to load plugin: %s", path);
            // Clean up the plugin manager we allocated
            plugin_manager_free(app->plugin_manager);
            composite_api_resolver_free(app->resolver);
            // Show user-friendly error
            view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
            return;
        }

        // Verify the plugin entry point is valid
        const MetroflipPlugin* plugin = plugin_manager_get_ep(app->plugin_manager, 0);
        if(!plugin || !plugin->plugin_on_enter || !plugin->plugin_on_event || !plugin->plugin_on_exit) {
            FURI_LOG_E(TAG, "Plugin loaded but has invalid entry points");
            plugin_manager_free(app->plugin_manager);
            composite_api_resolver_free(app->resolver);
            view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
            return;
        }

        // Mark plugin as successfully loaded
        app->plugin_loaded = true;
        
        // Get and run the plugin's on_enter function
        plugin->plugin_on_enter(app);
    }
}

bool metroflip_scene_parse_on_event(void* context, SceneManagerEvent event) {
    Metroflip* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventWrongCard) {
            FURI_LOG_I(TAG, "Wrong card event received - switching to unknown scene");
            scene_manager_next_scene(app->scene_manager, MetroflipSceneUnknown);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        FURI_LOG_I(TAG, "Back event received - returning to start");
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        return true;
    }

    // Only call plugin event handler if plugin was successfully loaded
    if(app->plugin_loaded) {
        const MetroflipPlugin* plugin = plugin_manager_get_ep(app->plugin_manager, 0);
        if(plugin && plugin->plugin_on_event) {
            return plugin->plugin_on_event(app, event);
        }
    }
    
    return false;
}

void metroflip_scene_parse_on_exit(void* context) {
    Metroflip* app = context;
    
    // Only clean up plugin if it was successfully loaded
    if(app->plugin_loaded) {
        const MetroflipPlugin* plugin = plugin_manager_get_ep(app->plugin_manager, 0);
        if(plugin && plugin->plugin_on_exit) {
            plugin->plugin_on_exit(app);
        }

        plugin_manager_free(app->plugin_manager);
        composite_api_resolver_free(app->resolver);
        app->plugin_loaded = false;
    }
    
    app->is_desfire = false;
    app->data_loaded = false;
}
