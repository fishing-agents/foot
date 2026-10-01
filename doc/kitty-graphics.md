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
CI uses Weston 14 headless and the test-only `tests/weston-seat.c` module to
expose a real device-less `wl_seat`, which headless Weston otherwise omits.
The module does not mock terminal behavior or manufacture protocol replies.
GitHub Actions builds and runs the tests with headless Weston.
