---
name: marketplace_search
description: "Search the Ameba-Claw GitHub skill marketplace by keyword, tag, or category (does not include built-in skills)."
metadata:
  cap_groups: [cap_http_request]
  manage_mode: web
---

# marketplace_search

Use this skill when the user wants to find available skills in the marketplace.

## Workflow

1. Fetch the index file from GitHub Raw:
   ```
   https://raw.githubusercontent.com/Ameba-AIoT/ameba-claw-skills-marketplace/main/skills-index.json
   ```
   Use the `http_request` cap with `method=GET` and the URL above.

2. Parse the returned JSON array; filter in context by keyword (name / description / tags) and optional conditions (category / tags / peripherals).

3. Display matching results: `id`, `description`, `tags`, `author`.

4. Remind the user that installation requires the exact `id` and is done via `marketplace_downloader`.

## Error handling

- If `http_request` is unavailable, stop and tell the user to enable it.
- On allowlist error, tell the user to add `github.com` to the allowlist.
- Never infer a skill id from a description -- always use the exact `id` from the index.

## Notes

- This skill searches only marketplace skills; it does not include skills already built into the device.
- After finding the target, provide the exact id and guide the user to install with `marketplace_downloader`.
