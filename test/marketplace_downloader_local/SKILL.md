---
name: marketplace_downloader_local
description: "Install a skill from a local HTTP server (dev/debug use, replaces GitHub). Same workflow as marketplace_downloader but requires an extra base_url argument."
metadata:
  cap_groups: [cap_lua, cap_http_request, cap_skill_mgr]
  manage_mode: readonly
---

# marketplace_downloader_local

**For development and debugging only.** Same as `marketplace_downloader` but fetches files from a local HTTP server instead of GitHub.

Use when the skill has not been pushed to GitHub yet and you need to test the install flow on the device.

## Step 1: start the local HTTP server

Run on the host (WSL), from the marketplace repo root:

```bash
cd /path/to/ameba-claw-skills-marketplace
python3 -m http.server 8081
```

## Step 2: get the base_url

The board must be able to reach the server over HTTP or HTTPS. Choose based on your network:

### Case A: board and host on the same LAN (direct)

Get the host IP on that network:

```bash
ip addr show   # or ifconfig
```

Use `http://<host-ip>:8081` as `base_url`.

### Case B: corporate network / inbound connections blocked (SSH reverse tunnel)

When the host cannot accept inbound connections (e.g. firewall blocks all inbound, no admin rights),
use an outbound SSH reverse tunnel -- no admin rights required:

```bash
# keep this terminal open while installing
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 \
    -R 80:localhost:8081 ssh.localhost.run
```

Wait for output like:

```
https://b430a669f7a123.lhr.life tunneled with tls termination
```

Copy that HTTPS URL as `base_url`. The subdomain changes on every connection; close this terminal when done.

## Step 3: install the skill

### fetch_metadata (verify URL is reachable and skill exists)

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill_local.lua",
  "args": {
    "action": "fetch_metadata",
    "skill_name": "<skill_name>",
    "base_url": "<base_url>"
  }
}
```

### install (download all files and write to vfs:/skills/)

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/download_skill_local.lua",
  "args": {
    "action": "install",
    "skill_name": "<skill_name>",
    "skill_name_from_metadata": "<skill_name>",
    "base_url": "<base_url>",
    "extra_files": {
      "scripts": ["<script>.lua"],
      "references": [],
      "assets": []
    }
  }
}
```

Take `extra_files` from the `_metadata.json` returned by fetch_metadata.

## Notes

- `base_url` must not have a trailing slash.
- The default allowlist is `*`; no extra configuration needed.
- For development/testing only -- do not use in production.
