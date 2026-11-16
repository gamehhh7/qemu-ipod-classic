# Closeout update — 2026-11-16

User stopped repair and requested GitHub publication/cleanup. Fresh process check found no app/QEMU/backend. Historical LIVE/manual-test text below is superseded. See ../../CLOSEOUT-20261116.md; audio NOT FIXED. No R141 work started.

---
# R140 — performance repair in progress

## Latest: user takes manual testing control, 2026-11-16 ~21:30HK

User asked “能不能我自己测试”. Stop all automated input/screenshots/profiling; leave current R140b ordinary session playing for user. Do not close/roll back during manual test. Agents audio_visual/audio_perf_visual notified; no further experiments until user feedback/continuation.

LIVE is second R140 session: app5300 creation21:23:03.370HK/qemu29384 creation21:23:05.712HK; run20261116T132303Z-5300, sameCDF3250...engine. First app2944/qemu2016 is CLOSED. New UI agent audio_perf_visual (Luna low) achieved Call the shots/Roselia/3of36; screenshot observed13:25:10.416Z under r140b-ui. Note agent called window handle3540732 an app id; actual app PID is5300, do not confuse.

`r140b-120s.json`: wall120.0131864s, V/W1.00064125, decode176582.20B/s, hostdrain176448.61B/s, nominal176468; new overflow0/stale0/empty_callbacks0; maxhostrefillgap13.2912ms; ring5540..37304B. This two-minute supply test passes; user listening/menu/Vortex acceptance remains pending. No claim fullyfixed or substantial globalperformanceheadroom.

IMPORTANT measurement bug found: R140 optional comparison selector ran once per remaining DMA element, so an intended baseline block transferred one element with old path then switched to cached path. compare_seen1024 but baseline_calls0/fast_calls512. **No valid speedup percentage can be calculated from this.** Actual fast transport and120s steady supply are still exercised after comparison ended. Next root work must latch baseline/fast selection for the whole pl080_pace_request (if comparison_path==1, keep returning legacy until that request completes), add a whole-block comparison test, and rebuild a separate candidate. R141 bulk idea was discussed internally but NO R141 code/build exists. Do not silently claim benchmark success.

First R140 music never started because prolonged UI inactivity reached knownlowpowerWFI22002e74; native read saved r140-stalled-2122.txt, detached. Normalclose andsecondlaunch, noHold, no sleeprepair. Currentprocess29384 has never had debugger attached; addresses140b.json resolvedoffline. 120s sampling finished; no active sampler intended.

Updated 2026-11-16 ~21:17 Hong Kong. Audio NOT FIXED. User latest explicitly asks for substantial execution-performance headroom, including menu and Vortex activity. Sleep remains deferred. No Cursor, DSH, technical subagents or Firecrawl.

R137 user listening: “仍有，但明显减轻”. R138 stream-stop re-priming did not execute during the measured playback (stream_stops delta0, prime_events delta0). R138120s V/W1.000019 but125 empty callbacks and35972 bytes stale trim. Not accepted. Current MP3 frame/sample counters say44100Hz, I2S44117Hz, Windows mix48000Hz; user's imported originals reportedly at most48kHz. Do not generalize to all source files or confuse sample rate with bitrate.

R138 high-frequency RPM: first55s trace was NOT aligned with UI operations. V/W0.99897; process CPU0.152 core-equivalents; clock residual range -59.56..262.03ms;6 empty callbacks. Second120s trace includes actual M/menu/wheel actions; V/W1.000263; CPU0.19584; residual -107.53..273.28ms;308 empty callbacks, no trims. Menu screenshots selected Shuffle Songs then Settings; no Vortex entered. These are average CPU/counter observations, not proof all interactive bursts have headroom. r138-ui-aligned-trace.json / -analysis.json and r138-ui-load screenshots.

WPR CPU.light was rejected by Windows policy (0xc5585011), no recording started. Used bounded native RIP sampler instead:25s,8threads,2322ticks,0 failures; each SuspendThread immediately resumed; aggregate hold238.573ms across threads, max0.153ms. This perturbs timing and is NOT listening acceptance. r138-native-analysis.json contains module/function samples, not exact on-CPU weighting. Repeated physical-address translation and memory access observed; no global CPU saturation conclusion from that sampling alone.

R139 clock-wobble10ms candidate compiled but NEVER launched. User redirected to execution performance. R140 inherits R138 (notR139); original auto clock tolerance retained.

R140 per-block DMA mapping cache: only paced memory->I2S,16-bit source/destination, incrementing RAM source, fixed FIFO. Exact source range and destination MemoryRegion verified. Caches RAM and FIFO mapping during one already-synchronous block. Preserves each16-bit MMIO store, source/count update order, ordinary LLI and TC/IRQ path. Other formats/devices/range failures use original loop. No firmware/guest PC/RAM/IRQ injection or sleep/timing shortcut. Actual helper255 boundary/order/fallback cases passed, plus disabled-path fallback. IRQ and actual listening still require guest acceptance.

CLASSIC7_DMA_COMPARE=1 optionally alternates old/new transport for1024 eligible blocks after100 real decoded MP3 frames; after512 pairs automatically stays fast. Timing counters include whole pl080_pace_request, with bytes/calls per path. No repeated indefinite A/B. CLASSIC7_DMA_CACHE=0 disables optimization (comparison has no effect). Source: r140-dma-cache.inc, r140-pl080.c, r140-sm1.c, prepare140.py, compile140.json, r140.rsp. r140-sm1.c only exports existing read-only decoded-frame counter for comparison start gating.

R138 app21328/qemu22964/backend3460 normally closed13:13:36Z; all writers absent before launch. LIVE R140: app2944 creation21:15:03.213HK, qemu2016 creation21:15:05.646HK, backend27624; run20261116T131503Z-2944. Engine CDF3250EFC3275876C637ADA09B9DDD70E7E8EDDC8DFDF45375CC9971DE20F4F. auto/align=off/sleep=on; USBoff; oneK COW. Formal UI DE2A8E1D6D9473A19ACC26D6070C7941F7CCB3E68563526B6F968F4AAFD6EEA6 and engine B55E89D6C4D33E8430F32D3A8D0F05FE8D20165C764D2AE6F00498528DB0549D freshly unchanged21:16HK.

No debugger attached to R140: map-static-addresses.py resolves exact-file symbols in offline GDB then relocates using fresh modulebase/PIDcreation/hash. addresses140.json; read-dma-perf.py validates creation_filetime. read-live138.py accepts this JSON mapping; ongoing bounded120s sampler waits up to90s for playback. Agent audio_visual (authorized Luna low) was asked to play Call the shots/Roselia; latest root at13:16:09Z still decoded_frames0, so music/paired comparison not yet accepted. Do not claim measured performance gain until results exist.

Next: obtain actual playback/UI response, read paired DMA result, read120s supply and nonintrusive trace, then test real menu/Cover Flow/Vortex with audio. If performance gain small or stutter remains, continue root profiling/repair; no claim huge headroom from a component speedup alone. Update CURRENT-RESULT and full R140 Markdown before ending. Single-writer guard includes all backend_persistent.py Python processes (ports dynamic). Old sleep_visual stays interrupted; only audio_visual for images/UI. Preserve seed/original rollback, no formal overwrite. Week remaining70% at last real check;5h window unknown, no reset redeemed.
