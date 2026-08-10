---
name: marketplace_downloader
description: "Download and install a skill from the Ameba-Claw GitHub marketplace. Requires the exact skill name; verifies peripheral compatibility before installing."
metadata:
  cap_groups: [cap_lua, cap_http_request, cap_skill_mgr]
  manage_mode: readonly
---

# marketplace_downloader

Use this skill when the user asks to install a skill from the GitHub marketplace.

## Workflow (two-step)

### Step 1: fetch metadata

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill.lua",
  "args": {
    "action": "fetch_metadata",
    "skill_name": "<skill_name>"
  }
}
```

The script prints the skill's `_metadata.json`, which contains `metadata.peripherals` and `extra_files`.

- If `metadata.peripherals` is non-empty, perform a compatibility check **before proceeding to install**.
  Activate `board_hardware_info`, then for each entry in `metadata.peripherals`:
  - **Generic peripheral type** (`i2c` / `spi` / `uart` / `gpio` / `pwm` / `adc` / `audio` / `rtc` / `ir`, or an instance like `SPI0`): call `board_query_peripheral(value)` and check that `supported` is `true` and at least one instance is free.
  - **Named device ID** (anything else, e.g. `display_lcdc_rgb_st7701p`): call `board_list_devices()`, collect every `id` in the returned `devices` array, and verify the required ID is present.
  - If any entry fails its check, **stop immediately — do not install**. Tell the user which peripherals or devices are missing and which board provides them (e.g. `PKE8721FLM-VA4-N33-HMI-ST7701P`).
- Record `name` from `_metadata.json` as `skill_name_from_metadata` and keep `extra_files` for step 2.

### Step 2: install

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill.lua",
  "args": {
    "action": "install",
    "skill_name": "<skill_name>",
    "skill_name_from_metadata": "<name from metadata>",
    "extra_files": {
      "scripts": ["<file1.lua>"],
      "references": ["<ref.md>"],
      "assets": []
    }
  }
}
```

After a successful install, call `skill_activate(name)` to add the new skill to the current session.

## Error handling

- **Skill not found (404)**: tell the user to check the name; suggest using `marketplace_search`.
- **Allowlist error**: tell the user to add `github.com` to the HTTP Request allowlist.
- **Peripheral mismatch**: stop by default and tell the user which peripherals are missing. Do not proceed unless the user explicitly requests a forced install.

## Notes

- Skill name must match `^[A-Za-z0-9_-]+$`.
- Never guess a skill name from a description -- use the exact id (run `marketplace_search` first if unsure).
- Install path is always `vfs:/skills/<skill_name>/`.
