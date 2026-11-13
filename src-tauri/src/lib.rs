mod ble;
mod commands;
mod files;
mod globals;
mod rig;
mod state;
mod tuner;

use tauri::Manager;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let core = state::Core::start(app.handle().clone());
            app.manage(core);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_snapshot,
            commands::protocol_info,
            commands::set_preset,
            commands::set_scene,
            commands::block_set_param,
            commands::block_set_on,
            commands::amp_set_on,
            commands::amp_set_eq,
            commands::block_add,
            commands::block_remove,
            commands::block_move,
            commands::capture_load,
            commands::default_folder,
            commands::list_folder,
            commands::upload_file,
            commands::amp_set_present,
            commands::save_preset,
            commands::save_scene,
            commands::export_backup,
            commands::restore_backup,
            rig::memory_usage,
            rig::slot_rename,
            rig::slot_delete,
            rig::inspect_backup,
            rig::library_list,
            rig::library_create_setlist,
            rig::library_delete_setlist,
            rig::library_save,
            rig::library_load,
            rig::library_delete,
            rig::library_import,
            ble::transport_get,
            ble::transport_set,
            ble::ble_scan,
            ble::ble_connect,
            ble::ble_disconnect,
            globals::get_global_settings,
            globals::set_global_eq,
            globals::set_global_eq_all,
            globals::set_input_hpf,
            globals::set_footswitch_mode,
            globals::set_led_config,
            globals::set_footswitch_led,
            globals::save_global_settings,
            tuner::tuner_set,
            tuner::tuner_read,
        ])
        .run(tauri::generate_context!())
        .expect("error while running PulseDeck");
}
