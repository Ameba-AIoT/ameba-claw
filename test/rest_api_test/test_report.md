# ameba_claw REST API 测试报告

> 执行时间：2026-07-01 16:34:33  SoC：RTL8721F

## 总览

| 指标 | 数值 |
|------|------|
| 总用例数 | 211 |
| 通过 | 197 |
| 失败 | 5 |
| 错误 | 0 |
| 跳过 | 9 |
| 总耗时 | 50.0s |
| 通过率 | 97.5% |

## 各模块详情

### ✅ HTTP Server Core

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `HTTP-001` test_HTTP_001_status_200_json | ✅ | 0.03s |  |
| `HTTP-002` test_HTTP_002_status_response_fields | ✅ | 0.01s |  |
| `HTTP-005` test_HTTP_005_post_body_8kb | ✅ | 0.16s |  |
| `HTTP-006` test_HTTP_006_post_body_over_limit_no_crash | ✅ | 0.36s |  |
| `HTTP-002` test_HTTP_002_four_concurrent_requests | ✅ | 0.06s |  |
| `HTTP-003` test_HTTP_003_fifth_concurrent_request_handled | ✅ | 0.55s |  |
| `HTTP-010` test_HTTP_010_half_open_connection_no_hang | ✅ | 6.12s |  |
| `HTTP-004` test_HTTP_004_unknown_route_404 | ✅ | 0.02s |  |
| `HTTP-008` test_HTTP_008_invalid_method_4xx | ✅ | 0.03s |  |
| `HTTP-009` test_HTTP_009_cors_header_present | ✅ | 0.02s |  |
| `HTTP-007` test_HTTP_007_path_traversal_rejected | ✅ | 0.01s |  |

### ✅ WebUI API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `—` test_api_config_llm_section | ✅ | 0.01s |  |
| `—` test_api_config_returns_all_sections | ✅ | 0.01s |  |
| `WUI-010` test_WUI_010_list_root_directory | ✅ | 0.16s |  |
| `WUI-011` test_WUI_011_nonexistent_path_404_or_empty | ✅ | 0.01s |  |
| `WUI-012` test_WUI_012_delete_and_verify | ✅ | 0.23s |  |
| `WUI-013` test_WUI_013_delete_nonexistent_4xx | ✅ | 0.04s |  |
| `WUI-014` test_WUI_014_delete_path_traversal_rejected | ✅ | 0.01s |  |
| `WUI-003` test_WUI_003_setup_returns_html | ✅ | 0.14s |  |
| `WUI-001` test_WUI_001_status_softap_fields | ✅ | 0.02s |  |
| `WUI-002` test_WUI_002_status_sta_connected | ✅ | 0.01s |  |
| `WUI` test_WUI_002b_status_wifi_mode_field | ✅ | 0.03s |  |
| `WUI-004` test_WUI_004_endpoint_exists_and_validates_json | ✅ | 0.01s |  |
| `WUI-005` test_WUI_005_non_json_body_400 | ✅ | 0.02s |  |
| `WUI-006` test_WUI_006_missing_ssid_400 | ✅ | 0.01s |  |
| `WUI-007` test_WUI_007_empty_ssid_400 | ✅ | 0.01s |  |
| `WUI-008` test_WUI_008_empty_body_400 | ✅ | 0.01s |  |
| `WUI-009` test_WUI_009_wifi_scan_returns_json | ✅ | 0.01s |  |

### ✅ File System API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE-004` test_FILE_004_delete_existing_file | ✅ | 0.18s |  |
| `FILE-005` test_FILE_005_delete_nonexistent_file | ✅ | 0.03s |  |
| `FILE-006` test_FILE_006_delete_config_file_evaluated | ✅ | 0.05s |  |
| `FILE-001` test_FILE_001_list_root | ✅ | 0.14s |  |
| `FILE-002` test_FILE_002_list_memory_dir | ✅ | 0.02s |  |
| `FILE-003` test_FILE_003_list_nonexistent_dir | ✅ | 0.03s |  |
| `FILE-007` test_FILE_007_empty_path_handled | ✅ | 0.01s |  |
| `—` test_upload_and_download | ✅ | 0.04s |  |

### ✅ File Mgmt Enhanced

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE` test_FILE_EXT_013_delete_empty_directory | ✅ | 0.29s |  |
| `FILE` test_FILE_EXT_014_delete_nonempty_directory_recursively | ✅ | 0.48s |  |
| `FILE` test_FILE_EXT_015_files_inside_deleted_dir_are_inaccessible | ✅ | 0.15s |  |
| `FILE` test_FILE_EXT_009_download_dir_status_200 | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_010_download_dir_content_type_zip | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_011_downloaded_zip_is_structurally_valid | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_012_zip_contains_correct_files_and_content | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_005_download_file_content_exact | ✅ | 0.02s |  |
| `FILE` test_FILE_EXT_006_download_missing_file_404 | ✅ | 0.02s |  |
| `FILE` test_FILE_EXT_007_download_no_path_param_400 | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_008_download_path_traversal_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_EXT_016_mkdir_creates_directory | ✅ | 0.25s |  |
| `FILE` test_FILE_EXT_017_mkdir_duplicate_fails | ✅ | 0.23s |  |
| `FILE` test_FILE_EXT_018_mkdir_path_traversal_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_EXT_019_upload_to_nonexistent_dir_returns_404 | ✅ | 0.01s |  |
| `FILE` test_FILE_EXT_001_entries_have_mtime_field | ✅ | 0.09s |  |
| `FILE` test_FILE_EXT_002_directory_entries_have_size_minus_one | ✅ | 0.09s |  |
| `FILE` test_FILE_EXT_003_empty_file_has_size_zero | ✅ | 0.12s |  |
| `FILE` test_FILE_EXT_004_nonzero_file_size_matches_upload | ✅ | 0.12s |  |

### ✅ File Content API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE` test_FILE_CONT_015_binary_file_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_014_put_empty_content | ✅ | 0.07s |  |
| `FILE` test_FILE_CONT_001_get_no_path | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_002_get_nonexistent_file | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_003_get_path_traversal_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_004_get_directory_instead_of_file | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_005_get_text_file_status_200 | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_006_get_text_file_content_matches | ✅ | 0.02s |  |
| `FILE` test_FILE_CONT_007_get_text_file_content_type | ✅ | 0.02s |  |
| `FILE` test_FILE_CONT_016a_max_allowed_file_ok | ✅ | 0.07s |  |
| `FILE` test_FILE_CONT_016b_put_exceeding_limit_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_013_overwrite_existing_file | ✅ | 0.08s |  |
| `FILE` test_FILE_CONT_008_put_no_path | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_009_put_path_traversal_rejected | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_010_put_save_to_nonexistent_directory | ✅ | 0.01s |  |
| `FILE` test_FILE_CONT_011_put_save_new_file | ✅ | 0.12s |  |
| `FILE` test_FILE_CONT_012_put_content_verifiable_via_get | ✅ | 0.16s |  |

### ❌ Untested Endpoints

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `UNT-015` test_UNT_015_cap_invoke_no_body_400 | ✅ | 0.02s |  |
| `UNT-016` test_UNT_016_cap_invoke_list_dir_capability | ✅ | 0.01s |  |
| `UNT-010` test_UNT_010_modules_list_returns_array | ✅ | 0.01s |  |
| `UNT-011` test_UNT_011_modules_entries_have_required_fields | ✅ | 0.03s |  |
| `UNT-012` test_UNT_012_post_modules_no_body_400 | ✅ | 0.01s |  |
| `UNT-013` test_UNT_013_post_modules_missing_mask_400 | ✅ | 0.01s |  |
| `UNT-014` test_UNT_014_post_modules_valid_mask_roundtrip | ✅ | 0.19s |  |
| `UNT-007` test_UNT_007_upload_lua_script_ok | ✅ | 0.02s |  |
| `UNT-008` test_UNT_008_uploaded_script_appears_in_list | ✅ | 0.04s |  |
| `UNT-009` test_UNT_009_upload_no_file_field_400 | ✅ | 0.01s |  |
| `UNT-001` test_UNT_001_root_returns_html | ✅ | 0.24s |  |
| `UNT-002` test_UNT_002_setup_post_no_body_returns_4xx_or_200 | ✅ | 0.01s |  |
| `UNT-003` test_UNT_003_setup_post_missing_ssid_rejected | ✅ | 0.01s |  |
| `UNT-004` test_UNT_004_wechat_qrcode_responds | ✅ | 0.01s |  |
| `UNT-005` test_UNT_005_wechat_status_returns_json | ✅ | 0.01s |  |
| `UNT-006` test_UNT_006_wechat_token_returns_json_with_token_field | ❌ | 0.03s | AssertionError: 'token' not found in {'ok': True, 'token_present': False} : Resp |

### ✅ Config Boundary Tests

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `CFG-003` test_CFG_003_ssid_32_bytes | ⏭️ | 0.00s | CFG-003: Triggers WiFi disconnect (32-byte SSID connection attempt) |
| `CFG-004` test_CFG_004_ssid_33_bytes | ⏭️ | 0.00s | CFG-004: Triggers WiFi disconnect (33-byte SSID, no length validation) |
| `CFG-005` test_CFG_005_ssid_empty_rejected | ✅ | 0.01s |  |
| `CFG-006` test_CFG_006_password_empty_open_ap | ⏭️ | 0.00s | CFG-006: Triggers WiFi disconnect (open AP connection attempt) |
| `CFG-007` test_CFG_007_password_63_bytes | ⏭️ | 0.00s | CFG-007: Triggers WiFi disconnect (63-byte password connection) |
| `CFG` test_CFG_empty_body_400 | ✅ | 0.01s |  |
| `CFG` test_CFG_missing_ssid_field_400 | ✅ | 0.01s |  |
| `CFG` test_CFG_non_json_body_400 | ✅ | 0.01s |  |
| `—` test_setup_llm_section_accepted | ✅ | 0.07s |  |
| `—` test_setup_no_section_field_handled | ✅ | 0.02s |  |
| `—` test_setup_non_json_body | ✅ | 0.02s |  |
| `—` test_setup_telegram_section | ✅ | 0.05s |  |
| `—` test_setup_unknown_section_handled | ✅ | 0.01s |  |

### ✅ HTTP Request Config

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `HTTPCFG-030` test_HTTPCFG_030_rule_253_chars_accepted | ✅ | 0.10s |  |
| `HTTPCFG-031` test_HTTPCFG_031_rule_254_chars_rejected | ✅ | 0.04s |  |
| `HTTPCFG-032` test_HTTPCFG_032_too_long_rule_error_message | ✅ | 0.02s |  |
| `HTTPCFG-033` test_HTTPCFG_033_total_length_511_accepted | ✅ | 0.06s |  |
| `HTTPCFG-034` test_HTTPCFG_034_total_length_512_rejected | ✅ | 0.01s |  |
| `HTTPCFG-035` test_HTTPCFG_035_second_rule_too_long_rejected | ✅ | 0.01s |  |
| `HTTPCFG-036` test_HTTPCFG_036_multi_star_wildcard_accepted | ✅ | 0.05s |  |
| `HTTPCFG-037` test_HTTPCFG_037_total_length_exactly_one_under_limit | ✅ | 0.07s |  |
| `HTTPCFG-010` test_HTTPCFG_010_invalid_rule_with_space | ✅ | 0.01s |  |
| `HTTPCFG-011` test_HTTPCFG_011_invalid_rule_with_slash | ✅ | 0.01s |  |
| `HTTPCFG-012` test_HTTPCFG_012_invalid_rule_with_at | ✅ | 0.01s |  |
| `HTTPCFG-013` test_HTTPCFG_013_invalid_rule_with_exclamation | ✅ | 0.01s |  |
| `HTTPCFG-014` test_HTTPCFG_014_invalid_rule_mixed_valid_invalid | ✅ | 0.01s |  |
| `HTTPCFG-015` test_HTTPCFG_015_error_message_contains_bad_rule | ✅ | 0.03s |  |
| `HTTPCFG-016` test_HTTPCFG_016_missing_section_field | ✅ | 0.02s |  |
| `HTTPCFG-017` test_HTTPCFG_017_non_json_body | ✅ | 0.01s |  |
| `HTTPCFG-020` test_HTTPCFG_020_save_readback_consistent | ✅ | 0.07s |  |
| `HTTPCFG-021` test_HTTPCFG_021_overwrite_readback_last_value | ✅ | 0.42s |  |
| `HTTPCFG-022` test_HTTPCFG_022_save_star_readback | ✅ | 0.10s |  |
| `HTTPCFG-023` test_HTTPCFG_023_save_empty_readback | ✅ | 0.10s |  |
| `HTTPCFG-001` test_HTTPCFG_001_config_has_http_request_section | ✅ | 0.02s |  |
| `HTTPCFG-002` test_HTTPCFG_002_save_star_allow_all | ✅ | 0.07s |  |
| `HTTPCFG-003` test_HTTPCFG_003_save_multi_rule | ✅ | 0.06s |  |
| `HTTPCFG-004` test_HTTPCFG_004_save_empty_deny_all | ✅ | 0.06s |  |
| `HTTPCFG-005` test_HTTPCFG_005_save_wildcard_subdomain | ✅ | 0.11s |  |
| `HTTPCFG-006` test_HTTPCFG_006_save_rule_with_port | ✅ | 0.11s |  |
| `HTTPCFG-007` test_HTTPCFG_007_save_rules_with_blank_lines | ✅ | 0.10s |  |
| `HTTPCFG-050` test_HTTPCFG_050_empty_allowlist_blocks_any_request | ✅ | 0.10s |  |
| `HTTPCFG-051` test_HTTPCFG_051_star_allowlist_passes_request | ✅ | 0.31s |  |
| `HTTPCFG-052` test_HTTPCFG_052_specific_rule_blocks_non_matching_host | ✅ | 0.12s |  |
| `HTTPCFG-053` test_HTTPCFG_053_exact_host_rule_passes_matching_request | ✅ | 0.39s |  |
| `HTTPCFG-054` test_HTTPCFG_054_wildcard_rule_blocks_non_matching_host | ✅ | 0.10s |  |
| `HTTPCFG-055` test_HTTPCFG_055_wildcard_rule_passes_matching_subdomain | ✅ | 0.25s |  |
| `HTTPCFG-056` test_HTTPCFG_056_invalid_method_rejected | ✅ | 0.09s |  |
| `HTTPCFG-057` test_HTTPCFG_057_invalid_url_rejected | ✅ | 0.07s |  |
| `HTTPCFG-058` test_HTTPCFG_058_rule_with_port_matches_host | ✅ | 0.31s |  |
| `HTTPCFG-059` test_HTTPCFG_059_rule_port_is_hostname_only_match | ✅ | 0.31s |  |
| `HTTPCFG-060` test_HTTPCFG_060_rule_with_port_blocks_non_matching_host | ✅ | 0.08s |  |

### ❌ Feishu Webhook

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FS-001` test_FS_001_challenge_verification | ❌ | 0.02s | AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found |
| `FS` test_FS_001b_normal_message_accepted | ❌ | 0.01s | AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found |
| `FS-004` test_FS_004_malformed_json_400 | ❌ | 0.03s | AssertionError: 404 not found in [400] : Expected 400, got 404: Not Found |
| `FS` test_FS_004b_empty_body_handled | ❌ | 0.01s | AssertionError: 404 not found in [200, 400, 500] |
| `FS-008` test_FS_008_no_signature_bypass_audit | ✅ | 0.01s |  |

### ✅ Lua Script HTTP API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `LUA` test_LUA_API_001_list_scripts | ✅ | 0.02s |  |
| `LUA` test_LUA_API_002_put_script | ✅ | 0.02s |  |
| `LUA` test_LUA_API_003_get_script_content | ✅ | 0.05s |  |
| `LUA` test_LUA_API_004_delete_script | ✅ | 0.05s |  |
| `LUA` test_LUA_API_005_delete_nonexistent_4xx | ✅ | 0.04s |  |
| `LUA` test_LUA_API_006_path_traversal_rejected | ✅ | 0.01s |  |

### ✅ WeChat Smoke

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `WX-001` test_WX_001_status_without_base_url_no_crash | ✅ | 0.01s |  |
| `WX-002` test_WX_002_qrcode_endpoint_responds | ✅ | 0.01s |  |
| `WX-003` test_WX_003_status_endpoint_responds | ✅ | 0.02s |  |

### ✅ Board Manager Caps

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `BRD-006` test_BRD_006_get_device_valid_id_returns_200 | ✅ | 0.01s |  |
| `BRD-007` test_BRD_007_get_device_response_has_required_fields | ✅ | 0.04s |  |
| `BRD-008` test_BRD_008_get_device_unknown_id_returns_error_field | ✅ | 0.02s |  |
| `BRD-009` test_BRD_009_get_device_missing_id_returns_error_field | ✅ | 0.03s |  |
| `BRD-010` test_BRD_010_get_device_oled_has_inline_interface | ✅ | 0.01s |  |
| `BRD-011` test_BRD_011_get_device_oled_has_params | ✅ | 0.02s |  |
| `BRD-012` test_BRD_012_get_device_button_type_is_button | ✅ | 0.01s |  |
| `BRD-001` test_BRD_001_list_devices_returns_200 | ✅ | 0.00s |  |
| `BRD-002` test_BRD_002_list_devices_has_board_field | ✅ | 0.00s |  |
| `BRD-003` test_BRD_003_list_devices_has_devices_array | ✅ | 0.00s |  |
| `BRD-004` test_BRD_004_list_devices_device_fields | ✅ | 0.00s |  |
| `BRD-005` test_BRD_005_list_devices_known_board_name | ✅ | 0.00s |  |
| `BRD-013` test_BRD_013_query_i2c_supported | ✅ | 0.01s |  |
| `BRD-014` test_BRD_014_query_i2c_has_I2C0_and_I2C1 | ✅ | 0.03s |  |
| `BRD-015` test_BRD_015_query_i2c_instance_status_valid | ✅ | 0.02s |  |
| `BRD-016` test_BRD_016_query_i2c_occupied_instance_has_config | ✅ | 0.02s |  |
| `BRD-017` test_BRD_017_query_i2c_has_available_pins | ✅ | 0.01s |  |
| `BRD-018` test_BRD_018_query_rtc_no_available_pins | ✅ | 0.01s |  |
| `BRD-019` test_BRD_019_query_unsupported_peripheral_returns_false | ✅ | 0.05s |  |
| `BRD-020` test_BRD_020_query_by_instance_name_resolves_type | ✅ | 0.01s |  |
| `BRD-025` test_BRD_025_reload_returns_200 | ✅ | 0.00s |  |
| `BRD-026` test_BRD_026_reload_response_has_success_and_board | ✅ | 0.00s |  |
| `BRD-027` test_BRD_027_reload_device_count_matches_list | ✅ | 0.00s |  |
| `BRD-021` test_BRD_021_schema_returns_200 | ✅ | 0.00s |  |
| `BRD-022` test_BRD_022_schema_chips_list_has_RTL8721F | ✅ | 0.00s |  |
| `BRD-023` test_BRD_023_schema_builtin_boards_has_known_boards | ✅ | 0.00s |  |
| `BRD-024` test_BRD_024_schema_has_authoring_schema_object | ✅ | 0.00s |  |
| `BRD-028` test_BRD_028_vfs_absent_loads_compile_time_default | ✅ | 0.23s |  |
| `BRD-029` test_BRD_029_vfs_custom_board_reload_takes_effect | ✅ | 0.21s |  |
| `BRD-030` test_BRD_030_vfs_board_extends_inherits_parent_devices | ✅ | 0.22s |  |

### ✅ Session Manager

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `—` test_M2_SES_01_at_new_auto_name | ⏭️ | 0.00s | Serial port not available (bridge unreachable or port busy) |
| `—` test_M2_SES_02_at_new_named | ⏭️ | 0.00s | Serial port not available (bridge unreachable or port busy) |
| `—` test_M2_SES_03_at_list_shows_sessions | ⏭️ | 0.00s | Serial port not available (bridge unreachable or port busy) |
| `—` test_M2_SES_04_at_reset_removed | ⏭️ | 0.00s | Serial port not available (bridge unreachable or port busy) |
| `—` test_M2_SES_22_at_clear_current | ⏭️ | 0.00s | Serial port not available (bridge unreachable or port busy) |
| `—` test_M2_SES_05_new_named_creates_session | ✅ | 0.00s |  |
| `—` test_M2_SES_06_list_state_shows_both_sessions | ✅ | 0.00s |  |
| `—` test_M2_SES_07_resume_switches_session | ✅ | 0.00s |  |
| `—` test_M2_SES_08a_work_removed_from_sessions | ✅ | 0.00s |  |
| `—` test_M2_SES_08b_current_unchanged | ✅ | 0.00s |  |
| `—` test_M2_SES_08c_history_file_gone | ✅ | 0.00s |  |
| `—` test_M2_SES_09_current_session_not_deleted | ✅ | 0.00s |  |
| `—` test_M2_SES_10a_history_cleared | ✅ | 0.00s |  |
| `—` test_M2_SES_10b_chat_map_unchanged | ✅ | 0.00s |  |
| `—` test_M2_SES_11_12_auto_alias_format | ✅ | 0.00s |  |
| `—` test_M2_SES_13a_current_renamed | ✅ | 0.00s |  |
| `—` test_M2_SES_13b_old_name_gone | ✅ | 0.00s |  |
| `—` test_M2_SES_14_rename_conflict_no_change | ✅ | 0.00s |  |
| `—` test_M2_SES_15a_history_survives_switch_away | ✅ | 0.00s |  |
| `—` test_M2_SES_15b_history_intact_after_switch_back | ✅ | 0.00s |  |
| `—` test_M2_SES_15c_current_is_work_after_resume | ✅ | 0.00s |  |
| `—` test_M2_SES_16_resume_no_args_no_change | ✅ | 0.00s |  |
| `—` test_M2_SES_17_delete_no_args_no_change | ✅ | 0.00s |  |
| `—` test_M2_SES_18_resume_notfound_no_change | ✅ | 0.00s |  |
| `—` test_M2_SES_19a_slash_creates_session_state | ✅ | 0.00s |  |
| `—` test_M2_SES_19b_no_llm_history_written | ✅ | 0.00s |  |
| `—` test_M2_SES_23_rename_no_args_no_change | ✅ | 0.00s |  |
| `—` test_M2_SES_24_nonexistent_delete_no_change | ✅ | 0.00s |  |

## 失败详情

### TestWechatEndpoints.test_UNT_006_wechat_token_returns_json_with_token_field
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/home/alejandro_chen/ameba_claw/test/rest_api_test/test_untested_endpoints.py", line 111, in test_UNT_006_wechat_token_returns_json_with_token_field
    self.assertIn("token", data,
AssertionError: 'token' not found in {'ok': True, 'token_present': False} : Response missing 'token' field: {'ok': True, 'token_present': False}
```

### TestFeishuWebhook.test_FS_001_challenge_verification
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/home/alejandro_chen/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 35, in test_FS_001_challenge_verification
    self.assertIn(r.status_code, [200], f"Expected 200, got {r.status_code}: {r.text}")
AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found
```

### TestFeishuWebhook.test_FS_001b_normal_message_accepted
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/home/alejandro_chen/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 60, in test_FS_001b_normal_message_accepted
    self.assertIn(r.status_code, [200], f"Expected 200, got {r.status_code}: {r.text}")
AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found
```

### TestFeishuWebhook.test_FS_004_malformed_json_400
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/home/alejandro_chen/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 65, in test_FS_004_malformed_json_400
    self.assertIn(r.status_code, [400], f"Expected 400, got {r.status_code}: {r.text}")
AssertionError: 404 not found in [400] : Expected 400, got 404: Not Found
```

### TestFeishuWebhook.test_FS_004b_empty_body_handled
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/home/alejandro_chen/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 70, in test_FS_004b_empty_body_handled
    self.assertIn(r.status_code, [200, 400, 500])
AssertionError: 404 not found in [200, 400, 500]
```

## 测试环境

| 项目 | 值 |
|------|---|
| SoC | RTL8721F |
| Proxy | 127.0.0.1 |
| Framework | Python unittest |
| 生成时间 | 2026-07-01 16:34:33 |