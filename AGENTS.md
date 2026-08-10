# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`ameba_claw` is an embedded AI assistant running on the Realtek **RTL8721F** SoC (Cortex-M33 class, FreeRTOS).

## AGENTS Working Boundaries (Important)

**Goal:** We are testing and optimizing the **agent harness** (C-layer firmware) of ameba-claw.
- ✅ **Allowed:** directly modify C code under `ameba_claw/` (harness, cap, modules, etc.) and use MCP tools to build/flash for verification (see "Build / Flash / Serial via MCP" below).
- 🚫 **Strictly forbidden during testing:** feeding Lua script prompts or code snippets to ameba-claw's LLM. Tasks under test (e.g. "write a button-detection script") must be completed by ameba-claw's LLM **exploring on its own** — the caps / skills / docs — because that harness behavior is exactly what we are observing and evaluating. You may only send task-unrelated control commands via `AT+CLAW` (e.g. `ask`, `session,clear`); you must not write Lua on its behalf.
For example, avoid using the serial port yourself to query boot-time tasks. Go through the ameba-claw LLM. The most important thing is to simulate the actual user flow.

## AGENTS.md Best Practices
- Act as a **router**, not an encyclopedia.
- Instructions should be **specific** and **repo-oriented**, preferring file paths and commands.
- **Point to source files** only; do not repeat architecture details.
- Clearly state **boundaries and exceptions** (e.g. "do not create pages by default").
- **Keep it up to date** — stale docs are worse than missing ones.

## Build & Flash

**Prefer the ameba-dev MCP** for building and flashing (supports both normal images and examples). If the MCP plugin is not installed, remind the user to install it first; fall back to the command line only if the user cannot install it:

- **Initialize the environment before any command-line build:** `source env.sh` (Windows: `env.bat`), otherwise the toolchain will not be found.
- Build: `./ameba.py build -q` (`-q` for quiet). Use `./ameba.py soc` to select the SoC first, `./ameba.py show` to check the current SoC.
- Build an example: `./ameba.py build -a example <dir_name>`, where `<dir_name>` is the directory name under `example/` (**not the full path**). Read the example's own AGENTS.md before modifying it.
- Flash: prefer MCP; use `./ameba.py flash` when MCP is unavailable.
- Other commands: `menuconfig` / `monitor` / `clean` / `list` — see `./ameba.py help`.

**SoC and artifacts:** The current SoC is recorded in `soc_info.json`; switch with `./ameba.py soc` (do not edit the file by hand). Internal project artifacts go to `build_<SOC>/` at the repo root (e.g. `build_RTL8721F/`); external projects output to their own directory.

**Test builds:** Test code (C unit tests + Lua peripheral driver test scripts, provisioned to `vfs:/` at boot) is compiled only when `CONFIG_CLAW_ENABLE_TESTS` is enabled — off by default. To build it, just enable that one Kconfig symbol via the ameba-dev MCP `kconfig_set` (`CONFIG_CLAW_ENABLE_TESTS=y`); if the MCP is unavailable, run `menuconfig` and toggle **"Build test surface (C unit tests + Lua driver test scripts)"**.

**Environment initialization:** Run `python setup.py` only once on a freshly cloned repo (installs dependencies + git hooks; the commit/coding constraints above only take effect after hooks are installed). For subsequent syncs use `python update.py`.

## Switching the Target Board (`AT+CLAW=board`)

Four board configs are embedded in the firmware; the active one lives in `vfs:/board.json` and can be switched at runtime over serial (default `EV721FL0_R03`). Operator/test-bench command only — not exposed as an LLM tool.

- `AT+CLAW=board` — list embedded boards, `*` marks the active one, ends with `OK`.
- `AT+CLAW=board,<name>` — switch to `<name>`, the board **directory** name under `claw_capabilities/cap_board_mgr/boards/` (not the `board.name` field — two PKE variants share one `board.name`). Overwrites `vfs:/board.json`, re-parses the model synchronously (`$chip`/`$extends` resolved), persists across reboot; prints `+CLAW:board,switched=<name>` then `OK` once fully loaded.

For the full `AT+CLAW` subcommand list, read the header comment of `claw_modules/claw_atcmd/src/cap_atcmd.c`.

## Mandatory Coding Constraints

1. **Prefer `RTK_LOGS`** (defined in each SoC's `swlib/log.h` or `fwlib/include/rom/log.h`).
   - Only basic format specifiers are supported: `%d %u %s %08x %c`; **`%lu %lx %ld` are not supported**, and **floating-point is not supported**.
   - Keep log strings concise to reduce rodata usage.
   - If floating-point printing is absolutely necessary, use `printf`.
2. **Never call FreeRTOS APIs directly.** Use the `rtos_xxx` wrappers: `component/os/os_wrapper/include/` (`os_wrapper.h` and `os_wrapper_task/queue/mutex/semaphore/...`). The pre-commit hook blocks new `#include FreeRTOS.h` and similar violations.
3. **Conserve resources:** new code should minimize stack usage and code size.
4. Follow pre-commit hooks: CppCheck static analysis (warning/performance/portability), AStyle indentation, and various protected-pattern checks (e.g. do not add calls to certain deprecated detection functions; use `EFUSE_GetChipVersion` instead).
5. Avoid local variables larger than 128 bytes on the task stack.

## Agent Development Guidelines

1. **Think before coding:** make no assumptions, hide no confusion, surface trade-offs, and stop to ask when in doubt.
2. **Simplicity first:** solve the problem with the minimum code; do not over-engineer or add unrequested flexibility.
3. **Precise changes:** touch only what must be touched, match the existing style, do not casually fix adjacent code.
4. **Goal-driven:** translate instructions into verifiable success criteria and verify in a loop until met. After coding, actively verify: prefer asking the user, or independently confirm via the real chip's serial port (monitor); if on-board testing is impossible, at least verify by compiling. The build must have zero warnings and zero errors.

## Commit Convention (enforced by commit-msg hook)

- Title format: `[amebaclaw][module:submodule][...] brief description`.
- At least 3 non-empty lines: line 1 is the title, line 2 is a **blank line**, line 3 onward is the body with each item starting with `* ` / `- ` / `+ ` (space required after the first character).
- Short and precise. Refer to history: `git log --oneline` (e.g. `[amebapro3][sdk] sync default.conf with rebuild config`).
- Push: most repos use `git push origin HEAD:refs/for/master` (Gerrit).

## Component Routing

This file (repo-root `AGENTS.md`) is the **top-level rule** and applies to all components. Architecture details and notes for each component are in its own directory's AGENTS.md:
