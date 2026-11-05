# Radio tool

A static page that puts a recording on Melon's Radio firmware. It needs no
build step and is deployed to GitHub Pages by `.github/workflows/radio-tool.yml`
whenever `radio/` changes on main.

- `dsp.js` decodes the file at the firmware's audio rate of 36.6 kHz, applies
  level and fades, resamples for the lower qualities and encodes the result as
  packed 10-bit or µ-law.
- `uf2.js` adds the recording to the compiled firmware, the same way
  `../postbuild.py` does.
- `player.js` plays it back the way `radio.ino` does, `knob.js` draws the
  panel's knobs.
- `flash.js` restarts Melon into its bootloader with a 1200 baud touch over Web
  Serial and writes the UF2 onto the RP2350 drive with the File System Access
  API.

To try it locally, put a compiled firmware and the example next to it and
serve the folder:

```
arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2350 --output-dir build radio/
cp build/radio.ino.uf2 radio/web/radio.uf2
cp radio/sample.wav radio/web/example.wav
python3 -m http.server --directory radio/web
```

Web Serial and the File System Access API only work on `localhost` or over
HTTPS. The fonts are subsets of Montserrat and Nunito Sans under the licences
in `fonts/`.
