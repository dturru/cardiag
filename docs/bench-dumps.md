# Raw bench dumps live outside the repo

Raw device dumps (`esptool read_flash` images, coredump partitions, the `.elf`
that was running) contain the provisioned credentials: Wi-Fi SSID and
password, AP password and the hub API token. This repo is **public**.

- Keep them in `%USERPROFILE%\bench-private\cardiag-dumps\`, mirroring the
  repo-relative path (e.g. `analysis/brick-2026-09-25-0748/flash-A.bin`).
- `.gitignore` blocks `analysis/**/*.bin`, `analysis/**/*.elf`, `*flash*.bin`
  and `*coredump*.bin` / `*coredump*.elf`, so a stray copy can't be committed.
- Text logs and README excerpts can name the SSID (the logger prints it on
  join). Redact it before committing; `tools/secret_scan.py` flags it.
- Before pushing anything from `analysis/`, run
  `python tools/secret_scan.py --repo . --secrets firmware/include/secrets.h`.
