# Residual/reference evidence for barge-in

The application AEC can have loud residual echo during convergence. A fixed energy
threshold alone is therefore insufficient to prove near-end speech. This module
checks whether the AEC residual is strongly correlated with previously submitted
playback reference, without altering either PCM stream.

`core/echo_guard.c` boxcar-decimates16kHz audio to2kHz and compares an80ms residual
window against every causal0.5ms reference-lag point through300ms. Absolute
normalized correlation of at least0.60 rejects that frame as independent near-end
evidence. The audio VAD and consecutive-frame confirmation still apply. A true
`near_allowed` means only that strong reference correlation was not found; it is
not proof that a human spoke.

Integration:

- Feed every frame after `dsp_process`, including reference silence.
- Gate only reply-state barge detection, never normal uplink PCM or its speech-end
  detector. There is no elapsed-time startup mute.
- Retain this history through `dsp_reset` and playback flush: earlier actually
  played audio can still explain room tail after queued playback is discarded.
- Initialize fresh for a new audio session. State is2376 bytes on the host.
- Work is bounded at601×160 integer MACs per call, about4.8M/s for20ms calls;
  silent-reference/residual windows take the fast path. Other call sizes change
  work per second. The correlation lag includes DSP latency and is not a measured
  hardware/room-path delay.

Build/execute the tests through the host voice CMake target. The executable with
no arguments runs synthetic no-startup-mute, independent-near, inverted-polarity,
multi-path and causal-delay tests, including0.5ms/off-coarse-grid delays and300ms.
With two raw PCM fixture paths it additionally uses the actual application Speex
DSP (denoise enabled) and dialog controller:

```sh
test_echo_guard \
  build/wake-deps/positive-real.raw build/wake-deps/negative-real.raw
```

The optional fixtures are16kHz mono S16_LE. Current test fixtures are a padded
Tingting “你好涂鸦” playback and an independently spoken “今天天气真不错” command.
They are external ignored artifacts, not checked-in audio or placeholders.

Measured software boundary results on Mac arm64 with`-O2` and UBSan:

- Pure far-end playback,0.7 gain and60ms linear echo: the original energy-only
  detector interrupts at680ms, although there is no near-end voice.
- Adding the guard prevents interruption with VAD starts300,500,1000 and1500.
- A120ms, two-path echo also produces no interruption in this fixture.
- Adding independent near-end speech from t=1000ms produces an interruption at
  t=1220ms. No input clipping occurs. Actual DAC stop latency is not measured.
- Guard processing in these checks averaged roughly46–93µs/call, max178–212µs,
  with2376-byte state. These host timings do not establish V821 real-time budget.

This is an experimental acoustic gate, not a general double-talk guarantee.
Nonlinear/clipped speaker output, reference mismatch, echo beyond the bounded
history, and correlated/sufficiently quiet near-end voices require real-device
acceptance and may defeat the heuristic. Test multi-speaker, distance, volume,
noise, barge-in at reply onset and after playback reset on the actual board before
claiming the300ms experience target.
