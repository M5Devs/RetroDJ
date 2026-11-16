# Vendored SMSlib (devkitSMS)

This directory holds the subset of **SMSlib** from
[devkitSMS](https://github.com/sverx/devkitSMS) that the RetroDJ SMS target
uses, together with the devkitSMS peephole rules SDCC needs to emit the
VRAM-safe store sequences the SMS VDP requires.

Only the modules the target links are vendored:

- `SMSlib.c` / `SMSlib.h` — VDP, controller and PSG access
- `SMSlib_textrenderer.c`, `SMSlib_autotext.c`, `SMSlib_string.c` — text output
- `SMSlib_load1bppTiles.c` — 1bpp tile upload
- `SMSlib_VRAMmemset.c` — VRAM fill
- `SMSlib_sprite.c` — sprite helpers
- `SMSlib_common.c` — shared constants
- `peep-rules.txt` — SDCC peephole rules

SMSlib is released into the public domain, so vendoring it here does not affect
the GPLv3 licensing of RetroDJ itself. The `.asm`, `.lst`, `.sym` and `.rel`
files SDCC generates next to these sources are build artifacts and are ignored
by git.
