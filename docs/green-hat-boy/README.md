# Green Hat Boy T186 fixes

This source kit layers selected Wave Ocarina work on the published GreenHatBoy T186 test base. The fork's older release branch and its installer keep their existing source pin. The kit contains source patches and tests.

## What’s included

| Change | Status |
|---|---|
| Depth-feedback recheck | Tests the views bound by the current draw before inserting the existing feedback barrier. On 13.60 the mean fence wait fell in all five Bilinear pairs and the median paired gain was 1.57 FPS. [Patch and measurements](https://github.com/Vampinity/GreenHatBoy/pull/34/files) |
| Park wait | Already available in T186 through core patches 0989 and 0990. On 9.40 five matched Castle Town pairs moved entry from 14.50 to 17.12 FPS and the walk from 17.70 to 21.52 FPS. Frames over 250 ms fell from 22 to 12. This is validation of an upstream change. [All ten 9.40 arms](park-wait-940.csv) |
| DualSense mic support | W-series host capture and the guest-side feed, including engaged-stream handling and reference audio. Humming and Instruments worked together on 9.40 with the separate game-specific routing correction. [Mic results](https://github.com/Vampinity/GreenHatBoy/discussions/2#discussioncomment-19763262) · [Routing notes](humming-routing.md) |
| Minimap recorder and steering | Record map positions, convert recordings to waypoints, and recover when the minimap disappears briefly. The two ports here target exact T186. The newer Castle Town gate logic has its own local overlay. |
| Shown-frame flash counter | Patch 1211 defaults off. Host checks passed. It needs timed video to judge missed or false detections and matched console runs to measure overhead. [Merged contribution](https://github.com/Vampinity/GreenHatBoy/pull/29) |

The 9.40 Castle Town comparison used a base PS5, handheld 1× at 1080p, Bilinear and a warm cache. Max Resolution was off in the confirmed park-wait comparisons. The median exceeded 15 FPS in both windows. Individual seconds still fell below that target. The 13.60 drawbridge rendering corruption remains under investigation.

## Reconstruct and check the source

From the root of this fork, pass a fresh directory to the source helper.

```sh
bash docs/green-hat-boy/reconstruct-t186.sh /tmp/green-hat-boy-t186
```

The helper verifies the published T120-to-T186 source diffs and checks for outer tree `b61e7cd93a7f6970a8ea7a59f57c7a1e25394df6` before applying any add-ons. The starting core pin is `0ca2032bffc5f1a3fca75427f7d4a4718adda660`. It stages the mic and route source and adds the flash and depth patches to the core patch series.

Use the reconstructed tree with the project's [native build instructions](../BUILDING.md). Use a separate test setup, keep your shader cache and saves backed up, and verify the build's own store identity before doing a warm-cache comparison. The public reconstruction and host checks passed. A native package from this combined source still needs compilation and hardware validation as a unit.

On a prepared T186 build tree, run these host checks from its root.

```sh
c++ -std=c++20 -IEglantine/src tools/test-flash-counter.cpp -o /tmp/gbb-flash-check
/tmp/gbb-flash-check
c++ -std=c++20 tools/test-steer-blind.cpp -o /tmp/gbb-steer-check
/tmp/gbb-steer-check
c++ -std=c++20 -Iheadless tools/test-hum-notes.cpp -o /tmp/gbb-hum-check
/tmp/gbb-hum-check
python3 tools/test-route-from-recording.py
```

For full test context see the [Castle Town report](https://github.com/Vampinity/GreenHatBoy/pull/34). The chart and raw numbers there describe the tested builds and settings for each run.
