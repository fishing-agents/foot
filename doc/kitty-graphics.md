# Kitty graphics in the fishing-agents Foot fork

This fork implements the static-image portion of the
[Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/).
It is enabled automatically; upstream Sixel and Kitty keyboard support remain
available. This is not a claim of complete Kitty protocol compatibility.

## Build and use

Follow [INSTALL.md](../INSTALL.md) for Foot's existing dependencies. This fork
also requires **libpng** and **zlib** development packages.

```sh
meson setup build -Ddocs=disabled
meson compile -C build
meson test -C build --print-errorlogs
./build/foot
```

Inside the new Foot terminal, display a PNG without installing Kitty:

```sh
python3 tools/kitty-image.py /path/to/image.png --columns 30
```

With Kitty installed, force direct transport rather than local-file or shared
memory transport:

```sh
kitty +kitten icat --transfer-mode=stream /path/to/image.png
```

## Supported protocol operations

- APC sequences `ESC _ G … ESC \\`, including sequences split across PTY reads.
- Direct (`t=d`) base64 image uploads: RGB (`f=24`), RGBA (`f=32`), PNG (`f=100`).
- Chunked uploads (`m=1` / `m=0`) and zlib compression (`o=z`).
- Transmit (`a=t`), transmit-and-display (`a=T`), display (`a=p`), query (`a=q`),
  and delete (`a=d`).
- Image IDs, placement IDs, source rectangles, requested cell dimensions, pixel
  offsets, alpha blending, cursor-movement control, and quiet responses.
- Stored images are independent of their displayed placements: deleting a
  placement does not necessarily delete the image data.

Image data and placement surfaces have bounded memory budgets. Malformed
controls, base64, dimensions, PNGs, or compressed input produce protocol errors
rather than unchecked allocations. CAN/SUB and interrupted escape strings
cancel incomplete requests. Non-graphics APCs are ignored.

## Compatibility boundaries

This implementation deliberately rejects unsupported extensions rather than
pretending they work. File/temporary-file/shared-memory transports, animation,
and Unicode placeholder/virtual placements are not implemented. Use direct
stream transport in client applications.

Rendering and lifecycle boundaries are described here to make client behavior
predictable; they are not hidden behind a blanket “Kitty compatible” label:

- Nonnegative z-order is supported; negative z-order (images below text) is not.
- Resizing/reflow discards displayed placements. Stored image data remains
  available, so clients can recreate placements with `a=p`.
- Partial-region scrolling discards displayed placements rather than reflowing
  them. Ordinary full-screen scrolling anchors images to the scrollback grid
  until their anchor rows are recycled.
- Supported deletion selectors are all, image ID, and image number (`a/A`,
  `i/I`, `n/N`). Cell-, cursor-, range-, and z-based deletion are not implemented.
  Uppercase deletion of a single named placement is rejected with `ENOTSUP`.
  Delete-all currently clears placements in both grids and scrollback, rather
  than Kitty's visible-only selection; uppercase delete-all frees stored data.

## Incremental rendering (Kitty and Sixel)

Both protocols retain already-composited pixels in Foot's normal render buffers.
Full-screen scrolling reuses Foot's existing pixel-copy scroll path. Buffer-age
repairs copy the complete previous frame, including images; neither kind of copy
is treated as a fresh background that should receive another alpha blend.

- Kitty no longer dirties every visible image on every frame. After the text
  workers finish, images composite only where actual text/Sixel repaint damage
  intersects their bounds. New, replaced and deleted placements dirty only
  their affected cells, including pixel offsets and aspect-fit letterboxing.
- Sixel previously copied the full image width for each dirty row chunk. It now
  clips those chunks to dirty-cell rectangles and actual text/glyph damage.
  A snapshot taken before text/opaque images clean cell flags preserves damage
  for multiple images sharing a row. Transparent images are blended only over
  restored backgrounds, and partial-cell edges still erase uncovered pixels.
- Partial-region Kitty scrolling still discards placements, but invalidates
  their old cell bounds rather than explicitly damaging the whole viewport.
  Placement-preserving partial-region reflow is not implemented.

This is **incremental redisplay**, not a new Sixel wire-protocol extension:
Sixel uploads still need to be decoded, and resizing/scaling can rebuild image
caches. Full repaint remains necessary when the underlying viewport is fully
damaged. Graphics are CPU-composited with Pixman, not rendered by a GPU.

`test-image-render` exercises the production Kitty and Sixel compositors with
isolated text-drawing/cache callbacks. It checks exact pixels, alpha stability,
scroll-copy reuse, recycled buffers, unrelated cell edits, shared dirty rows,
partial-cell edges and overflowing glyphs. On a 1024×1024 image with 16×16 cells:

- Kitty: full repaint submits 1,048,576 image pixels; one changed cell submits
  256; a clean frame submits zero.
- Sixel: the former full-width dirty strip was 16,384 pixels; one changed cell
  now submits 256; a clean frame submits zero.

An optional CPU microbenchmark compares full-background/full-image repaint
against one-cell background/image repaint. It is not an end-to-end scrolling
FPS benchmark, and timings depend on the machine and build type:

```sh
./build/test-image-render --benchmark
```

## Tests

`meson test -C build --print-errorlogs` runs the protocol regression tests as well
as Foot's existing tests. A real-terminal smoke test runs the compiled Foot
under an existing Wayland compositor:

```sh
python3 tests/kitty-graphics-smoke.py build/foot
```

The smoke test checks actual PTY responses for queries, RGB/RGBA/PNG uploads,
compression, multiple placements, chunked uploads, malformed input, interruption,
and deletion. Placements remain alive long enough for a Wayland frame to render.
It also exercises cell edits, forward/reverse scrolling, alternate-grid buffer
reuse, opaque/transparent Sixels and mixed partial-region scrolling. These
Wayland checks validate execution and PTY responsiveness; pixel correctness and
composited work are asserted separately by `test-image-render`. Set
`FOOT_SMOKE_WORKERS=2` to exercise the threaded text renderer as well.
CI uses Weston 14 headless and the test-only `tests/weston-seat.c` module to
expose a real device-less `wl_seat`, which headless Weston otherwise omits.
The module does not mock terminal behavior or manufacture protocol replies.
GitHub Actions builds and runs the tests with headless Weston.
