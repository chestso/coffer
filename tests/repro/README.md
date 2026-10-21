# tests/repro — the image-store defect reproducers

Throwaway programs, one per finding in
[`docs/image-store-design.md`](../../docs/image-store-design.md). They
are NOT part of `make check` (they print, they do not assert); they are
the evidence behind findings F1–F9 and the fastest way to see a
regression while the fix is in flight.

Build them against a built coffer tree:

```sh
cd tests/repro
for f in *.c; do
    gcc -g -O0 -I../../include "$f" ../../build/src/libcoffer.a \
        -lm -lthorvg-1 -o "${f%.c}" || echo "skip $f (no thorvg?)"
done
```

(`-lthorvg-1` is only needed for the Lottie case; drop it when coffer
is built without ThorVG.)

| Program       | Finding | What it shows                                                                |
| ------------- | ------- | ---------------------------------------------------------------------------- |
| `misclass`    | F1      | a transmitted-but-unplaced kitty image dies on `ESC [ 2 J` and on resize     |
| `evict3`      | F2      | the 257th image evicts the OLDEST, not the newest                            |
| `budget2`     | F3      | the byte-budget eviction takes the newest 1:N image, not the oldest          |
| `collide2`    | F4      | a client's `i=1` collides with the engine's id space (iTerm2, Lottie)        |
| `damage`      | F5      | the kitty path reports no damage at all                                      |
| `host_upload` | F8      | at `content_scale = 2` the host reads past the pixel buffer (ASan: overflow) |
| `repro`       | F6      | a re-transmit of the same id drops the placements (kitty's rule)             |
| `chat`        | —       | a two-image chat transcript: ids, placements, and scroll behaviour           |

`host_upload` is the one to build with `-fsanitize=address`; it is the
memory-safety proof for F8.
