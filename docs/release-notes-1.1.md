# GABBERKIX 1.1

GABBERKIX as the kick module in a MIDI chain.

- **A MIDI channel for the notes:** the connections page (SEL + REC) has a new last row, MIDI CHANNEL. Put the cursor
  on it with SELECT and turn K1: ALL (any channel, as in 1.0) or 1 to 16. Only the notes are filtered: THRU still
  passes every channel on to what comes after, and clock, Start and Stop are always followed.
- **The tempo and the channel are kept** over a power-off and an update. They are saved two seconds after the last
  change, once GABBERKIX is quiet (not playing, no kick for two seconds), since writing the flash holds the sound up
  a moment.
- The connections page shows two Bluetooth devices at a time (SELECT scrolls through all of them) to make room.

Identity `FM-1_818010100` (pattern `FM-1_818XXYYZZ`). The presets from 1.0 stay as they are.
