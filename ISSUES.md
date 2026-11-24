# Known issues and investigation findings

Investigation snapshot: 2026-11-16.

This document separates reproduced defects from hypotheses. The reason the
iPhone sends `Cancel-Job` is still unresolved; neither Atkinson dithering nor
a particular iPhone timeout has been established as the cause.

## Summary

| Issue | Status |
| --- | --- |
| IPP uploads blocked requests on other connections | Reproduced; concurrent connection handling implemented and observed working |
| iPhone sends `Cancel-Job` during printing | Open; occurs with both dithering algorithms |
| Cancellation can be followed by a completed job status | Confirmed in code and observed in logs |
| Apple Raster and printer resolution advertisements disagree | Confirmed; impact on cancellation unknown |
| Response-side protocol diagnostics are insufficient | Open |
| Atkinson-specific printing failure | Not established; timing differences confound the comparison |

## 1. IPP connection blocking

### Original behaviour

The IPP server accepted one connection and processed it synchronously,
including receiving and converting the entire print document. Status requests
on other connections could not be answered during that upload.

A non-printing test on the running bridge reproduced this:

- An intentionally invalid raster upload was held open for two seconds.
- A concurrent `Get-Printer-Attributes` received no response during that wait.
- The status request completed after the upload was released.
- The invalid document was rejected, and no printer jobs were created.

Initial iPhone uploads took approximately 21-23 seconds. That duration includes
network reception and raster conversion; it is not a measurement of dithering
CPU time alone.

### Implemented change

[IppServer](lib/IppServer/IppServer.h) now supports up to three concurrent
connection tasks. The listener remains responsible for accepting connections
and maintaining mDNS advertisements.

- IPP error messages are request-local rather than shared between connections.
- Only one raster upload is processed at a time to bound memory use.
- Additional simultaneous uploads receive an IPP busy response.
- Rejected uploads with unread bodies close the connection rather than waiting
  for the rest of the document before returning the error.

Subsequent iPhone logs show `Get-Printer-Attributes`, `Get-Jobs`, and
`Get-Job-Attributes` being serviced during uploads and BLE scans. Upload times
in the supplied comparison logs were approximately 7-8 seconds.

**Limitation:** this fixes the reproduced blocking defect, but cancellation
continued. The earlier hypothesis that connection blocking alone explained
the iPhone cancellation was not confirmed.

## 2. iPhone cancellation remains unexplained

DEBUG logs establish that the IPP client sends explicit `Cancel-Job`
operations. The bridge does not spontaneously cancel these jobs because of
the bitmap contents.

The two concurrent-firmware runs provide this comparison:

| Event, seconds since boot | Atkinson | Floyd-Steinberg |
| --- | ---: | ---: |
| `Print-Job` received | 56.399 | 127.909 |
| Job queued | 64.178 | 134.853 |
| BLE connection started | 67.799 | 134.853 |
| First `Cancel-Job` | 72.487 | 142.555 |
| BLE connected | 72.707 | 136.147 |
| Final bridge status | Cancelled | Completed |

In the Atkinson run, a scan delayed connection startup, and cancellation
arrived before the BLE connection completed. In the Floyd-Steinberg run,
the connection completed about 1.3 seconds after queueing, so printing was
already underway when cancellation arrived.

The first cancellation occurred about 16.1 and 14.6 seconds after
`Print-Job`, respectively. This is a timing observation, **not proof of an
iOS 15-second deadline**.

Inspection of [Apple's public CUPS IPP backend](https://github.com/apple/cups/blob/master/backend/ipp.c)
did not reveal a universal 15-second first-page or print-completion deadline.
That source is useful background, but it does not establish the behaviour of
the particular iOS version or printing app used here.

## 3. Cancellation versus successful completion

The successful Floyd-Steinberg print still received two cancellations:

```text
142.555  Cancel-Job
144.846  Cancel-Job
150.306  Job completed
```

The implementation explains how this can happen:

- [CatPrinterDriver::printBitmap](lib/CatPrinterDriver/CatPrinterDriver.cpp)
  checks cancellation through row-progress callbacks.
- The final flush, flow-control wait, and printer-drain delay do not
  independently check cancellation.
- [JobQueue::process](lib/JobQueue/JobQueue.cpp) chooses the completed state
  when execution succeeds, even if cancellation has meanwhile been requested.

Thus, a job can finish despite a late cancellation. This masks the continuing
client-cancellation issue and makes a physical print alone an insufficient
test of protocol success.

The intended behaviour for cancellation during the final transfer/drain needs
to be defined and tested. A fix must distinguish already-transmitted output
from data that can still be stopped; changing only the reported state would
not solve that problem.

## 4. Atkinson is not a proven root cause

Both algorithms produce a 1-bit bitmap on the ESP32-S3 before BLE transfer.
The iPhone does not receive that bitmap and does not acknowledge individual
BLE packets.

- Focused native tests passed with both algorithm selections: 24/24 tests.
- Tests cover reference output, narrow images, page-state reset, and
  thresholding with dithering disabled.
- The user reports successful physical output with Floyd-Steinberg.
- However, the Floyd-Steinberg log also contains `Cancel-Job`, and the two
  hardware runs have substantially different BLE startup delays.

There is currently no demonstrated Atkinson buffer corruption or encoding
failure. Native test success also does not rule out a hardware-specific issue.
A controlled comparison needs the same input, app, printer state, and
comparable scan/connection timing.

The firmware build currently selects `KITTY_IPP_BRIDGE_DITHER_ATKINSON=0` in
[platformio.ini](platformio.ini), retaining Floyd-Steinberg. Setting it to `1`
selects Atkinson. The raster library defaults to Atkinson when no build
override is supplied.

## 5. Inconsistent resolution advertisements

A read-only query of the running bridge returned:

```text
printer-resolution-default:   203 x 203 dpi
printer-resolution-supported: 203 x 203 dpi
urf-supported:                V1.4, CP1, PQ3-4-5, RS300, W8
```

`RS300` advertises 300 dpi Apple Raster input, while the general resolution
attributes advertise only 203 dpi. These capabilities need review and
alignment. Input raster resolution is distinct from the printer's physical
203 dpi print head: accepting 300 dpi input and scaling it is valid in
principle.

**Unknown:** whether this inconsistency contributes to the iPhone cancellation.
It must not be presented as the confirmed cause without further evidence.

## 6. What has and has not been verified about responses

Read-only queries of the running bridge verified:

- Successful IPP status and correctly echoed request IDs.
- Job attributes including `job-id`, `job-state`, and `job-state-reasons`.
- The finished job reporting state `9` (completed),
  `job-completed-successfully`, and one completed impression.

These queries do not capture the responses delivered during the failing
iPhone exchange. Existing DEBUG operation lines primarily identify incoming
requests; they do not prove that the corresponding response was fully written,
received, or accepted by the phone.

The HTTP response writer also does not currently surface every response-write
failure to its caller.

### Next diagnostic steps

1. Log connection identity, IPP operation and request ID, response status,
   returned job state, response duration, and write success/failure. Avoid
   logging document contents, credentials, or unnecessary personal data.
2. Record any cancellation message supplied by the client, the iPhone app in
   use, and the error shown by Print Center.
3. Separate network-read time from decoding/dithering time.
4. Capture a complete failing request/response sequence and validate its
   attributes and framing against IPP/AirPrint requirements.
5. Review the resolution advertisements and cancellation-at-completion
   behaviour independently, with focused tests.

Do not ignore `Cancel-Job`, falsely report completion, or increase timeouts
solely to hide the symptom.

## 7. BLE scanning and keep-alive observations

The scanner and IPP server run in separate FreeRTOS tasks on different cores
on the ESP32-S3. A blocking scan call therefore does not directly block the
IPP task. Wi-Fi and BLE still share radio resources, so scanning can affect
network throughput.

The BLE arbiter prevents a print connection from starting while a scan owns
the radio. This delay is visible in the failed Atkinson run and is separate
from the original IPP connection-blocking defect.

An earlier report described a printer powering off while idle. Later logs
show successful keep-alive sends, but a successful write is not a measurement
that every printer model resets its power-off timer. Long-duration physical
keep-alive behaviour remains a separate validation item.

## Validation scope

- Both dithering selections passed the native suite during this investigation.
- The concurrent ESP32-S3 firmware built successfully and was exercised on
  hardware; supplied iPhone logs demonstrate concurrent request handling.
- The Floyd-Steinberg comparison firmware was uploaded and the bridge was
  verified responsive after reboot.
- The latest firmware build reported 56,540 bytes of static RAM use (17.3%).
  This is not a measurement of peak runtime heap use with all workers active.
- End-to-end iPhone printing without unexpected cancellation is **not yet
  verified**, even though physical output was obtained.
