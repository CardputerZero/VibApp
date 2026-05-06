# CardputerZero VibApp

## STOP: Do Not Use / 停用提醒

> **DO NOT USE VibApp RIGHT NOW.**
>
> **当前不要使用 VibApp。**
>
> **English:** The current CM0 performance is not sufficient for this workflow, and building directly on CardputerZero has not been successfully validated yet.
>
> **中文：** 当前 CM0 性能不足，暂时还没有跑通在 CardputerZero 上直接编译。因此现在不要使用 VibApp 这个应用。

LVGL-based VibApp launcher/client for M5Stack Cardputer Zero.

## Repository Layout

- `main/src/` - application and keyboard input source code
- `main/include/` - application headers and compatibility key definitions
- `main/Kconfig` - project configuration entries
- `share/vibapp/skills` - git submodule containing the bundled Cardputer Zero skill
- `SConstruct` and `main/SConstruct` - SCons build scripts
- `config_defaults.mk` - default build configuration

Generated build outputs are intentionally ignored by git.

## Skill Submodule

VibApp uses the `m5stack-cardputer-applauncher` skill through the `share/vibapp/skills` submodule:

```bash
git submodule update --init --recursive
```

At runtime, the app sets `VIBAPP_SKILL_PATH` to:

```text
../share/vibapp/skills/m5stack-cardputer-applauncher/SKILL.md
```

relative to the VibApp binary directory when that file exists. This mirrors the APPLaunch install layout under `/usr/share/APPLaunch/share/vibapp/skills`.
