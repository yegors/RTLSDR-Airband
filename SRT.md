# SRT output

RTLSDR-Airband can send audio over the [SRT protocol](https://www.srtalliance.org/).
Building with SRT support requires the development files for **libsrt**.
When configuring with CMake leave the `-DSRT` option enabled (default)
and ensure `pkg-config` can locate the library. If libsrt is missing the
feature is disabled automatically.

The SRT output supports three audio formats controlled by the `format`
setting in the configuration:

- `pcm` (default) – raw 32‑bit float PCM
- `mp3` – encoded using libmp3lame
- `wav` – PCM wrapped in a WAV header so players like VLC can connect
  without any extra parameters

When streaming in `mp3` or `wav` formats the following `ffplay` command
provides low latency playback:

```bash
ffplay -fflags nobuffer -flags low_delay srt://<host>:<port>
```

## Configuration

```
outputs: (
  {
    type = "srt";
    listen_address = "0.0.0.0";
    listen_port = 8890;
    format = "mp3";       # pcm|mp3|wav
    continuous = true;    # optional, default false
  }
);
```

`continuous` controls whether the stream pauses when the squelch is
closed. Set it to `true` if the receiving application does not handle
frequent reconnects well.

