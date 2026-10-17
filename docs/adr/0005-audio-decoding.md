# Voice and music: decode xWMA in the converter with FFmpeg's libraries, store Ogg Vorbis

Step 4 of the order of work (SYSTEMS.md D2) needs the dialogue voices; the
music (W7) uses the same format. Decided on 2026-10-17:

- Skyrim's voices (`.fuz`: a `.lip` plus an xWMA stream) and music (`.xwm`)
  are WMA2 in an XWMA RIFF. Godot plays WAV, Ogg Vorbis and MP3, so the
  converter re-encodes them to **Ogg Vorbis**; the engine loads them with
  `AudioStreamOggVorbis.load_from_buffer`.
- Decoding uses **libavcodec/libavformat from vcpkg** (`ffmpeg` port, no
  default features, only `avcodec` and `avformat`; LGPL-2.1-or-later,
  compatible with the project's GPL-3.0-or-later), linked into `bethconv`
  and fed from memory. Encoding uses **libvorbis/libogg from vcpkg**
  (BSD-3-Clause). No external program is needed at convert time, so the
  pack tool still ships one `bethconv` binary (TOOLS-REQUIREMENTS 1.1) and
  the Windows and ARM64 builds get the same decoder.
- A `.fuz` keeps its vpath and becomes one `sound` asset in a small SkyDot
  container (`SDVC`) holding the Ogg stream and the untouched `.lip` bytes
  for facial animation later (A16). A `.xwm` keeps its vpath and becomes a
  plain Ogg Vorbis `sound` asset.

## Considered options

- **Run `ffmpeg` as a child process:** least code, but an install-time
  dependency on Windows and the Steam Frame, and output that changes with
  the user's FFmpeg build (the blob store dedupes by content).
- **Write or vendor a WMA2 decoder:** no small, maintained one exists; a
  copy of FFmpeg's `wmadec.c` pulls in most of libavcodec's internals.
- **Store PCM WAV:** the engine already plays it, but the voices alone would
  grow from 1.7 GB to roughly 10 GB.
- **Opus:** smaller, but Godot has no Opus stream; it would need a decoder
  in the engine.

## Consequences

- The first vcpkg build of the converter compiles FFmpeg (several minutes;
  the binary cache keeps it).
- Lossy to lossy: Vorbis quality 0.4 is meant to be indistinguishable from
  the source for speech; the user's listening check decides.
