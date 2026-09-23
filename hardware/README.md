# hardware/

Carrier-board work for the ESP32-CAN-X2 bolt-on.

## ⚠️ Vendor files are NOT redistributed here

`hardware/vendor-*` is **gitignored**. Autosport Labs' Eagle template files are their
work, and this repository is **public** — they are not redistributed without a licence
that explicitly permits it.

**No licence file or copyright header was found in the template itself** (checked
2026-09-22), so redistribution permission is *unknown*, which means *no*.

### Get the template from the source

| | |
|---|---|
| Product wiki | <https://wiki.autosportlabs.com/ESP32-CAN-X2> |
| Board | ESP32-CAN-X2 (ESP32-S3-WROOM-1-N8R8) |
| Files used here | `bolt-on_template.sch` / `.brd` (Eagle) |
| Eagle format version in the files | **9.6.2** (with 8.2/8.3 compatibility notes) |
| Downloaded | 2026-09-22 or earlier — **exact release tag not recorded by the vendor files** |

🔴 **The template has no version stamp of its own.** Eagle's own format version (9.6.2)
is the only marker in the file, and that identifies the *editor*, not the template
revision. If the vendor ever revises it, there is no way to tell from these files which
revision we used. **Record the download date and URL when re-fetching.**

### Local layout (untracked)

```
hardware/
  bolton_template.sch            working copy
  vendor-bolt-on-template/       as downloaded        [gitignored]
  vendor-template-sanitized/     vendor file, edited  [gitignored]
```

### If redistribution is ever wanted

Ask Autosport Labs directly, or look for an explicit licence on the wiki or their
GitHub. Only then commit the files, and add the licence text alongside them.

## What the carrier board adds

See the design record in the Obsidian vault (`Carrier Board — Requirements and BOM`),
which is canonical. Summary of what matters to firmware:

| Item | Note |
|---|---|
| microSD `J2` (`DM3AT-SF-PEJM5`) | **Not on the dev board.** Until the carrier arrives, the file store is backed by LittleFS on internal flash |
| SD bus | **Cannot share the MCP2515 SPI bus** — needs its own |
| RTC | PCF8563 + CR2032, for the `rtc` time-anchor source |

See `docs/hub-integration-plan.md` for how the firmware phases around the carrier's
arrival.
