# ameba_claw REST API 测试报告

> 执行时间：2026-06-04 11:23:29  SoC：RTL8721F

## 总览

| 指标 | 数值 |
|------|------|
| 总用例数 | 115 |
| 通过 | 107 |
| 失败 | 4 |
| 错误 | 0 |
| 跳过 | 4 |
| 总耗时 | 16.8s |
| 通过率 | 96.4% |

## 各模块详情

### ✅ HTTP Server Core

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `HTTP-001` test_HTTP_001_status_200_json | ✅ | 0.02s |  |
| `HTTP-002` test_HTTP_002_status_response_fields | ✅ | 0.02s |  |
| `HTTP-005` test_HTTP_005_post_body_8kb | ✅ | 0.06s |  |
| `HTTP-006` test_HTTP_006_post_body_over_limit_no_crash | ✅ | 0.36s |  |
| `HTTP-002` test_HTTP_002_four_concurrent_requests | ✅ | 0.02s |  |
| `HTTP-003` test_HTTP_003_fifth_concurrent_request_handled | ✅ | 0.57s |  |
| `HTTP-010` test_HTTP_010_half_open_connection_no_hang | ✅ | 6.12s |  |
| `HTTP-004` test_HTTP_004_unknown_route_404 | ✅ | 0.03s |  |
| `HTTP-008` test_HTTP_008_invalid_method_4xx | ✅ | 0.02s |  |
| `HTTP-009` test_HTTP_009_cors_header_present | ✅ | 0.03s |  |
| `HTTP-007` test_HTTP_007_path_traversal_rejected | ✅ | 0.04s |  |

### ✅ WebUI API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `—` test_api_config_llm_section | ✅ | 0.07s |  |
| `—` test_api_config_returns_all_sections | ✅ | 0.02s |  |
| `WUI-010` test_WUI_010_list_root_directory | ✅ | 0.10s |  |
| `WUI-011` test_WUI_011_nonexistent_path_404_or_empty | ✅ | 0.08s |  |
| `WUI-012` test_WUI_012_delete_and_verify | ✅ | 0.18s |  |
| `WUI-013` test_WUI_013_delete_nonexistent_4xx | ✅ | 0.04s |  |
| `WUI-014` test_WUI_014_delete_path_traversal_rejected | ✅ | 0.01s |  |
| `WUI-003` test_WUI_003_setup_returns_html | ✅ | 0.30s |  |
| `WUI-001` test_WUI_001_status_softap_fields | ✅ | 0.02s |  |
| `WUI-002` test_WUI_002_status_sta_connected | ✅ | 0.02s |  |
| `WUI` test_WUI_002b_status_wifi_mode_field | ✅ | 0.02s |  |
| `WUI-004` test_WUI_004_endpoint_exists_and_validates_json | ✅ | 0.04s |  |
| `WUI-005` test_WUI_005_non_json_body_400 | ✅ | 0.02s |  |
| `WUI-006` test_WUI_006_missing_ssid_400 | ✅ | 0.03s |  |
| `WUI-007` test_WUI_007_empty_ssid_400 | ✅ | 0.02s |  |
| `WUI-008` test_WUI_008_empty_body_400 | ✅ | 0.05s |  |
| `WUI-009` test_WUI_009_wifi_scan_returns_json | ✅ | 0.03s |  |

### ✅ File System API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE-004` test_FILE_004_delete_existing_file | ✅ | 0.16s |  |
| `FILE-005` test_FILE_005_delete_nonexistent_file | ✅ | 0.05s |  |
| `FILE-006` test_FILE_006_delete_config_file_evaluated | ✅ | 0.07s |  |
| `FILE-001` test_FILE_001_list_root | ✅ | 0.10s |  |
| `FILE-002` test_FILE_002_list_memory_dir | ✅ | 0.05s |  |
| `FILE-003` test_FILE_003_list_nonexistent_dir | ✅ | 0.04s |  |
| `FILE-007` test_FILE_007_empty_path_handled | ✅ | 0.01s |  |
| `—` test_upload_and_download | ✅ | 0.13s |  |

### ✅ File Mgmt Enhanced

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE` test_FILE_EXT_013_delete_empty_directory | ✅ | 0.25s |  |
| `FILE` test_FILE_EXT_014_delete_nonempty_directory_recursively | ✅ | 0.41s |  |
| `FILE` test_FILE_EXT_015_files_inside_deleted_dir_are_inaccessible | ✅ | 0.20s |  |
| `FILE` test_FILE_EXT_009_download_dir_status_200 | ✅ | 0.02s |  |
| `FILE` test_FILE_EXT_010_download_dir_content_type_zip | ✅ | 0.02s |  |
| `FILE` test_FILE_EXT_011_downloaded_zip_is_structurally_valid | ✅ | 0.05s |  |
| `FILE` test_FILE_EXT_012_zip_contains_correct_files_and_content | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_005_download_file_content_exact | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_006_download_missing_file_404 | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_007_download_no_path_param_400 | ✅ | 0.05s |  |
| `FILE` test_FILE_EXT_008_download_path_traversal_rejected | ✅ | 0.02s |  |
| `FILE` test_FILE_EXT_016_mkdir_creates_directory | ✅ | 0.31s |  |
| `FILE` test_FILE_EXT_017_mkdir_duplicate_fails | ✅ | 0.21s |  |
| `FILE` test_FILE_EXT_018_mkdir_path_traversal_rejected | ✅ | 0.04s |  |
| `FILE` test_FILE_EXT_019_upload_to_nonexistent_dir_returns_404 | ✅ | 0.03s |  |
| `FILE` test_FILE_EXT_001_entries_have_mtime_field | ✅ | 0.13s |  |
| `FILE` test_FILE_EXT_002_directory_entries_have_size_minus_one | ✅ | 0.12s |  |
| `FILE` test_FILE_EXT_003_empty_file_has_size_zero | ✅ | 0.19s |  |
| `FILE` test_FILE_EXT_004_nonzero_file_size_matches_upload | ✅ | 0.19s |  |

### ✅ File Content API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FILE` test_FILE_CONT_015_binary_file_rejected | ✅ | 0.07s |  |
| `FILE` test_FILE_CONT_014_put_empty_content | ✅ | 0.04s |  |
| `FILE` test_FILE_CONT_001_get_no_path | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_002_get_nonexistent_file | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_003_get_path_traversal_rejected | ✅ | 0.04s |  |
| `FILE` test_FILE_CONT_004_get_directory_instead_of_file | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_005_get_text_file_status_200 | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_006_get_text_file_content_matches | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_007_get_text_file_content_type | ✅ | 0.04s |  |
| `FILE` test_FILE_CONT_016a_max_allowed_file_ok | ✅ | 0.22s |  |
| `FILE` test_FILE_CONT_016b_put_exceeding_limit_rejected | ✅ | 0.02s |  |
| `FILE` test_FILE_CONT_013_overwrite_existing_file | ✅ | 0.06s |  |
| `FILE` test_FILE_CONT_008_put_no_path | ✅ | 0.03s |  |
| `FILE` test_FILE_CONT_009_put_path_traversal_rejected | ✅ | 0.02s |  |
| `FILE` test_FILE_CONT_010_put_save_to_nonexistent_directory | ✅ | 0.04s |  |
| `FILE` test_FILE_CONT_011_put_save_new_file | ✅ | 0.13s |  |
| `FILE` test_FILE_CONT_012_put_content_verifiable_via_get | ✅ | 0.10s |  |

### ✅ Untested Endpoints

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `UNT-015` test_UNT_015_cap_invoke_no_body_400 | ✅ | 0.01s |  |
| `UNT-016` test_UNT_016_cap_invoke_list_dir_capability | ✅ | 0.02s |  |
| `UNT-010` test_UNT_010_modules_list_returns_array | ✅ | 0.04s |  |
| `UNT-011` test_UNT_011_modules_entries_have_required_fields | ✅ | 0.03s |  |
| `UNT-012` test_UNT_012_post_modules_no_body_400 | ✅ | 0.03s |  |
| `UNT-013` test_UNT_013_post_modules_missing_mask_400 | ✅ | 0.03s |  |
| `UNT-014` test_UNT_014_post_modules_valid_mask_roundtrip | ✅ | 0.22s |  |
| `UNT-007` test_UNT_007_upload_lua_script_ok | ✅ | 0.02s |  |
| `UNT-008` test_UNT_008_uploaded_script_appears_in_list | ✅ | 0.09s |  |
| `UNT-009` test_UNT_009_upload_no_file_field_400 | ✅ | 0.02s |  |
| `UNT-001` test_UNT_001_root_returns_html | ✅ | 0.39s |  |
| `UNT-002` test_UNT_002_setup_post_no_body_returns_4xx_or_200 | ✅ | 0.02s |  |
| `UNT-003` test_UNT_003_setup_post_missing_ssid_rejected | ✅ | 0.03s |  |
| `UNT-004` test_UNT_004_wechat_qrcode_responds | ✅ | 0.25s |  |
| `UNT-005` test_UNT_005_wechat_status_returns_json | ✅ | 0.03s |  |
| `UNT-006` test_UNT_006_wechat_token_returns_json_with_token_field | ✅ | 0.04s |  |

### ✅ Config Boundary Tests

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `CFG-003` test_CFG_003_ssid_32_bytes | ⏭️ | 0.00s | CFG-003: Triggers WiFi disconnect (32-byte SSID connection attempt) |
| `CFG-004` test_CFG_004_ssid_33_bytes | ⏭️ | 0.00s | CFG-004: Triggers WiFi disconnect (33-byte SSID, no length validation) |
| `CFG-005` test_CFG_005_ssid_empty_rejected | ✅ | 0.01s |  |
| `CFG-006` test_CFG_006_password_empty_open_ap | ⏭️ | 0.00s | CFG-006: Triggers WiFi disconnect (open AP connection attempt) |
| `CFG-007` test_CFG_007_password_63_bytes | ⏭️ | 0.00s | CFG-007: Triggers WiFi disconnect (63-byte password connection) |
| `CFG` test_CFG_empty_body_400 | ✅ | 0.02s |  |
| `CFG` test_CFG_missing_ssid_field_400 | ✅ | 0.04s |  |
| `CFG` test_CFG_non_json_body_400 | ✅ | 0.02s |  |
| `—` test_setup_llm_section_accepted | ✅ | 0.06s |  |
| `—` test_setup_no_section_field_handled | ✅ | 0.04s |  |
| `—` test_setup_non_json_body | ✅ | 0.02s |  |
| `—` test_setup_telegram_section | ✅ | 0.06s |  |
| `—` test_setup_unknown_section_handled | ✅ | 0.03s |  |

### ❌ Feishu Webhook

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `FS-001` test_FS_001_challenge_verification | ❌ | 0.04s | AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found |
| `FS` test_FS_001b_normal_message_accepted | ❌ | 0.03s | AssertionError: 404 not found in [200] : Expected 200, got 404: Not Found |
| `FS-004` test_FS_004_malformed_json_400 | ❌ | 0.02s | AssertionError: 404 not found in [400] : Expected 400, got 404: Not Found |
| `FS` test_FS_004b_empty_body_handled | ❌ | 0.02s | AssertionError: 404 not found in [200, 400, 500] |
| `FS-008` test_FS_008_no_signature_bypass_audit | ✅ | 0.01s |  |

### ✅ Lua Script HTTP API

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `LUA` test_LUA_API_001_list_scripts | ✅ | 0.04s |  |
| `LUA` test_LUA_API_002_put_script | ✅ | 0.04s |  |
| `LUA` test_LUA_API_003_get_script_content | ✅ | 0.08s |  |
| `LUA` test_LUA_API_004_delete_script | ✅ | 0.12s |  |
| `LUA` test_LUA_API_005_delete_nonexistent_4xx | ✅ | 0.02s |  |
| `LUA` test_LUA_API_006_path_traversal_rejected | ✅ | 0.03s |  |

### ✅ WeChat Smoke

| 用例 | 状态 | 耗时 | 备注 |
|------|------|------|------|
| `WX-001` test_WX_001_status_without_base_url_no_crash | ✅ | 0.02s |  |
| `WX-002` test_WX_002_qrcode_endpoint_responds | ✅ | 0.04s |  |
| `WX-003` test_WX_003_status_endpoint_responds | ✅ | 0.02s |  |

## 失败详情

### TestFeishuWebhook.test_FS_001_challenge_verification
```
Traceback (most recent call last):
  File "/usr/lib/python3.10/unittest/case.py", line 59, in testPartExecutor
    yield
  File "/usr/lib/python3.10/unittest/case.py", line 591, in run
    self._callTestMethod(testMethod)
  File "/usr/lib/python3.10/unittest/case.py", line 549, in _callTestMethod
    method()
  File "/path/to/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 35, in test_FS_001_challenge_verification
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
  File "/path/to/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 60, in test_FS_001b_normal_message_accepted
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
  File "/path/to/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 65, in test_FS_004_malformed_json_400
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
  File "/path/to/ameba_claw/test/rest_api_test/test_feishu_webhook.py", line 70, in test_FS_004b_empty_body_handled
    self.assertIn(r.status_code, [200, 400, 500])
AssertionError: 404 not found in [200, 400, 500]
```

## 测试环境

| 项目 | 值 |
|------|---|
| SoC | RTL8721F |
| Proxy | 127.0.0.1 |
| Framework | Python unittest |
| 生成时间 | 2026-06-04 11:23:29 |