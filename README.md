# Kitty IPP Bridge

[![Build](https://github.com/rzeldent/kitty-ipp-bridge/actions/workflows/main.yml/badge.svg)](https://github.com/rzeldent/kitty-ipp-bridge/actions/workflows/main.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue.svg)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-orange?logo=platformio&logoColor=white)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-S3-red?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-s3)

Kitty IPP Bridge runs on an ESP32 and connects compatible Bluetooth Low
Energy (BLE) thermal printers to a Wi-Fi network. It discovers printers by
their BLE service UUIDs and exposes them as IPP printers using mDNS/DNS-SD.
The web dashboard shows discovered printers and print jobs and provides
settings, logs, and a test-page action.

**iPhones are supported through AirPrint**, without installing the printer
manufacturer's app. The same printing workflow is available on iPads. The bridge accepts
**Apple Raster / URF (`image/urf`)**, the raster format used by AirPrint, as
well as PWG Raster for other IPP clients.

The current printer profile supports classic Cat Printer devices using the
AE30 service (including devices advertising AF30), such as the GB0x, GT01,
MX0x, and YT01 families. Other protocols are not currently supported.
IPP jobs must be PWG Raster (`image/pwg-raster`) or Apple Raster
(`image/urf`, used by AirPrint), or one of these sent as
`application/octet-stream`; the bridge converts them to monochrome bitmaps.

## The kitty printer

![Cat-shaped portable thermal printer](assets/images/kitty%20printer.png)

These small, cat-shaped printers use direct thermal printing: the print head
darkens heat-sensitive paper, so no ink or toner is needed. They are useful
for short notes, lists, and monochrome pictures rather than full-size documents
or colour photographs.

The bridge's current classic Cat Printer profile uses these specifications:

| Feature | Details |
| --- | --- |
| Printing | Monochrome, 1-bit raster output |
| Resolution | 203 dpi |
| Printable width | 384 dots, approximately 48 mm; this is the printed area, not the paper-roll width |
| Connection to the bridge | Bluetooth Low Energy (BLE), not Bluetooth Classic |
| Protocol | Legacy CatGFX, with service `0xAE30`; advertisements using `0xAF30` are also recognised |
| BLE characteristics | `0xAE01` for writes, `0xAE02` for status and flow-control notifications |
| Network printing | The ESP32 bridge adds Wi-Fi IPP/AirPrint access; the printer itself connects over BLE |

Similar-looking printers are sold under many names and can contain different
electronics or firmware. The photo shows the enclosure style, not a guarantee
of compatibility: check the BLE protocol rather than appearance alone.
Paper-roll size, battery capacity, charging connector, and automatic power-off
time vary by model; consult the printer's own specifications. Thermal prints
can fade with heat, sunlight, and age, so they are not intended for archival use.

### Image processing and dithering

The ESP32-S3 scales incoming raster images to the printer width and applies
**serpentine error-diffusion dithering** before sending a 1-bit bitmap over BLE.
The supplied firmware build selects Floyd-Steinberg; Atkinson is also available.
Atkinson distributes one eighth of each pixel's quantization error to six
neighbours across the current row and the next two rows, discarding the
remaining quarter.

The compile-time define `KITTY_IPP_BRIDGE_DITHER_ATKINSON` selects the algorithm:
`1` uses Atkinson; `0` uses Floyd-Steinberg. Change the existing
`-DKITTY_IPP_BRIDGE_DITHER_ATKINSON=0` flag in `[esp32_common]`'s `build_flags` in
[platformio.ini](platformio.ini) to `1` to enable Atkinson, then rebuild and
upload. Without a build override, the raster library defaults to Atkinson.
Atkinson uses one extra error-buffer row (about 0.8 KiB at 384 dots).
The raster pipeline's `dither = false` option still uses simple thresholding.

## Status dashboard

Open [http://kitty-ipp-bridge.local/](http://kitty-ipp-bridge.local/) (or the bridge's IP
address) to view the live status dashboard.

![Kitty IPP Bridge status dashboard](assets/images/status%20page.png)

- **Status cards** show uptime, free heap, online and known printers, and
  successful or failed print jobs.
- **Printers** lists each printer's status, signal strength (RSSI), last-seen
  time, and IPP URI. Use the actions to rename a printer, print an identification
  or test page, or forget it. **Scan now** requests a new discovery scan.
- **Jobs** shows queued and recent jobs with their state, progress, and messages.
- **Bridge settings** controls discovery, retries, keep-alive, and printer
  protocol settings. Click **Save** to apply changes; queue-size changes require
  a reboot.
- **Log** shows live diagnostics, including keep-alive attempts and printing
  errors. **Details** provides firmware and memory statistics for troubleshooting.
- **Network** configures Wi-Fi and the bridge hostname.

## Requirements

- An ESP32-S3 development board with PSRAM (N16R8 recommended, or FH4R2) and a USB data cable.
- A compatible BLE Cat Printer.
- A 2.4 GHz Wi-Fi network and a computer or phone on the same network.
- [PlatformIO](https://platformio.org/install) to build and upload the firmware.

### Board selection and memory experience

The bridge needs Wi-Fi and Bluetooth Low Energy **at the same time**, with
enough RAM left for the web dashboard, IPP server, queued jobs, and raster
conversion. A board that supports BLE is not necessarily large enough to run
the complete bridge reliably.

| Board | Suitability for this project |
| --- | --- |
| **ESP32-S3 with octal PSRAM** | Supported target. An **N16R8** board (16 MB flash, 8 MB PSRAM) matches the supplied firmware configuration. |
| **ESP32-S3FH4R2 with quad PSRAM** | Build target `esp32s3-fh4r2`: 4 MB flash, 2 MB PSRAM, and reduced job-memory limits. Hardware validation is still required. |
| **ESP32-C3** | Not enough memory for this project's combined networking, BLE, and printing workload. It has Wi-Fi and BLE, but is not a supported target. |
| **ESP32-S2** | No Bluetooth hardware, so it cannot communicate with the BLE printers, even if the board has PSRAM. |
| **ESP32-S3 without PSRAM** | Can run using internal RAM, with a reduced 160 KiB job-memory budget. Hardware validation is recommended. |

PSRAM provides space for large job and image buffers; it does not replace all
internal RAM needed by Wi-Fi, BLE, and task stacks. The N16R8 build limits each
converted job bitmap and the total queued/active job payloads to 2 MiB each.
The FH4R2 build reduces both limits to 512 KiB, leaving room for raster
conversion and other allocations in its smaller PSRAM. These limits apply to
converted output, not the size of the uploaded raster document. Extra flash
storage is not a substitute for RAM. If PSRAM is not detected, the firmware
uses the normal internal heap and limits the converted-job and queued-payload
budgets to 160 KiB.

Bitmap buffer growth and inter-page gaps respect the converted-job budget.
If a buffer allocation fails (for example, because the internal heap is
fragmented or exhausted), the upload is rejected with an IPP busy response
and a serial/log warning instead of aborting the device. The configured budget
is an upper bound, not a guarantee that enough contiguous RAM is available.

Check the actual module specification before buying: not every ESP32-S3 board
includes PSRAM. Select the matching environment: N16R8 uses **octal PSRAM and
a 16 MB flash partition layout**, while FH4R2 uses **quad PSRAM and a 4 MB
layout** with two 1.875 MiB application slots. Other memory configurations need
matching build settings rather than the unmodified firmware.

## Build and flash

Connect the ESP32 by USB and run this command from the project
directory:

```sh
# ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM
pio run -e esp32s3-n16r8 -t upload

# ESP32-S3FH4R2: 4 MB flash, 2 MB quad PSRAM
pio run -e esp32s3-fh4r2 -t upload
```

The upload command builds the firmware first. The default environment is
`esp32s3-n16r8`. PSRAM is used when detected; otherwise the firmware continues
with internal RAM and the reduced job-memory budget. If PlatformIO cannot find
the board, specify its serial port with `--upload-port`.

Every push runs a GitHub Actions build matrix for `esp32s3-n16r8` and
`esp32s3-fh4r2`. Successful builds provide a separate artifact named after
each environment, containing `firmware.bin` and `firmware.elf`, on the workflow
run's summary page.

### Create a release

In GitHub, open **Actions > Create release > Run workflow**, select the source
branch, and enter a new tag such as `v1.0.0`. The workflow builds both
`esp32s3-n16r8` and `esp32s3-fh4r2` using a matrix. After both builds succeed,
a single publishing job creates an annotated tag on the selected commit and
publishes a GitHub Release with generated release notes. The release includes
`esp32s3-n16r8.bin`, `esp32s3-n16r8.elf`, `esp32s3-fh4r2.bin`, and
`esp32s3-fh4r2.elf`, so the board variants cannot overwrite each other.
GitHub also provides source archives for the tag. Existing tags are rejected.

The workflow must be present on the default branch before it can be run
manually.

## Connect to Wi-Fi

1. On first startup, connect your computer or phone to the setup Wi-Fi network
   named `KittyIPPBridge-XXXX`. Its default password is `kitty-ipp-bridge`.
2. Open [http://192.168.4.1/](http://192.168.4.1/) in a browser. If the
   address differs, check the setup access point address in the serial log.
3. Under **Network**, enter your Wi-Fi SSID and password. You can also change
   the hostname and setup access point password here.
4. Click **Save**, then **Reboot** to apply the network settings. The settings
   are stored on the device.
5. Reconnect your computer or phone to your usual Wi-Fi network. Open
   [http://kitty-ipp-bridge.local/](http://kitty-ipp-bridge.local/) (or use your chosen
   hostname) or use the IP address found below.

If the bridge cannot connect within about 20 seconds, it starts the setup
network again so you can correct the settings. It continues retrying the
configured network and disables the setup network after the connection has
been stable for about a minute.

The dashboard loads Bootstrap from a CDN. Without internet access it may appear
unstyled, and some controls may be unavailable.

The dashboard source is in [html/index.html](html/index.html). PlatformIO embeds
it in the firmware, so rebuild and upload after changing it.

## Find the IP address

Connect the ESP32 by USB and open the PlatformIO serial monitor at 115200 baud:

```sh
pio device monitor -b 115200
```

Restart the board if necessary and look for `Connected, IP ...` in the log.
The setup access point's name and address are also logged. Alternatively,
check your router's connected-device list for the bridge's hostname
(`kitty-ipp-bridge` by default).

## Add a printer

1. Turn on a compatible printer near the bridge. The dashboard scans
   periodically; use **Scan now** to request a scan immediately.
2. Find the printer in the dashboard and use **Test page** to check it. If it
   is not discovered, check the logs and try enabling **Probe devices without
   full UUID list** in **Bridge settings**.
3. Add the discovered IPP printer using your operating system's printer
   settings. Where mDNS/DNS-SD discovery is available, the printer can appear
   automatically; otherwise use the **IPP URI** shown in the dashboard. The
   URI has the form `ipp://kitty-ipp-bridge.local:631/ipp/<printer-id>`. Replace the
   hostname with the bridge IP if `.local` name resolution is unavailable.

### Windows 11

1. Open **Settings > Bluetooth & devices > Printers & scanners** and select
   **Add device**. The printer appears under its dashboard name when Windows
   finds it through mDNS.
2. If it does not appear, select **Add manually**, then **Add a printer using
   an IP address or hostname**. Set **Device type** to **IPP Device** and enter
   the URI with `http://` instead of `ipp://`, preferably using the bridge IP:
   `http://<bridge-ip>:631/ipp/<printer-id>`.
3. Windows installs its built-in IPP class driver; no vendor driver is needed.

Windows discovers printers only on the same subnet, and only when network
discovery is enabled (the default for networks set to **Private**).

### iPhone printing (AirPrint)

The bridge advertises each printer through Bonjour/mDNS for driverless AirPrint
printing from an **iPhone**. This also works on iPads.

1. Connect the iPhone to the same Wi-Fi network as the bridge.
2. Open the photo or document in an app that supports printing.
3. Select **Share > Print**, tap **Printer**, and choose the printer's
   dashboard name.
4. Adjust the available print options and tap **Print**.

AirPrint jobs use **Apple Raster**, also known as **URF (Universal Raster
Format)**, with MIME type **`image/urf`**. The bridge advertises 8-bit grayscale
at 300 dpi (`W8`, `RS300`), decodes the raster data, and converts it to the
printer's monochrome output. This does not change the printer's physical
203 dpi resolution. PDF and JPEG files are not accepted directly as IPP
document formats; print them through an AirPrint-capable app so the system
converts them to a supported raster format.

The iPhone must be on the same network and subnet as the bridge, and the
network must allow multicast (mDNS); guest networks and client isolation often
block discovery. The iPhone connects to the bridge over Wi-Fi,
not directly to the printer over Bluetooth. After a firmware update, reboot
the bridge so it advertises the new printer details.

The bridge handles up to three IPP connections concurrently, so status and
cancellation requests can be answered while an iPhone uploads a document.
Only one print upload is accepted at a time to bound memory use; another
simultaneous upload receives an IPP busy response and can be retried.

### Keep printers awake

Cat Printers power themselves off after a period of inactivity. To prevent
this, enable **Keep printers awake** in **Bridge settings**. While no job is
printing, the bridge then briefly connects to each online printer at the set
**Keep-alive interval** (default 120 seconds) and requests its status; nothing
is printed. The bridge continues trying printers it has already contacted
even if they stop advertising and age out of discovery. Choose an interval
shorter than the printer's power-off time. A printer that is already off
cannot be woken over Bluetooth; turn it on again with its power button.
Keeping printers awake drains their batteries faster.

BLE transfers enforce at least 20 ms between writes, including when older saved
settings specify a shorter delay. Printer pause notifications still stop the
transfer; a pause lasting more than 30 seconds fails the job rather than sending
through backpressure. Enable debug logging to see pause/resume notifications.

The web dashboard and IPP service are intended for a trusted network; the
dashboard does not require a login. Change the default setup access point
password and avoid exposing the bridge to the internet.

## Run native tests

```sh
pio test -e native
```
