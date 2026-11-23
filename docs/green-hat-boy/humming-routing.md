# Humming audio routing on 9.40

The emulator mic support in this source kit forwards DualSense audio into the guest mic stream. Zelda's Humming mode needed a second, game-specific routing change. With the matching v2.0.0 executable and the mic trace active, Humming selected the virtual stream first. Instruments selected the physical microphone first.

The verified change selects the physical stream first and the virtual stream second for Humming, leaving the mode selector, condition flags and Instruments path intact. The first tested session produced Humming estimates of 231.5, 181.2 and 201.9 Hz. Instruments still recognized the same player's input later in that session. This was verified on firmware 9.40.

For developers reproducing that result against their own matching executable, the two replacements are at offsets in the decompressed image, before adding the IPS32 header offset.

| Image offset | Replacement | Purpose |
|---|---|---|
| `0x336D598` | `mov x19, x21` | Use the first physical-stream selection |
| `0x336DD70` | `mov x0, x8` | Use the existing secondary-stream selection |

The tested game-specific routing used the existing diagnostic IPS as its base. The public source kit includes the emulator mic implementation. [Original verification and trace excerpts](https://github.com/Vampinity/GreenHatBoy/discussions/2#discussioncomment-19763262)
