# Test fixtures

`encrypted-zip.zip` contains a 2×2 PPM image named `inside-password.ppm`, protected with
traditional PKWARE encryption. Its test password is `jpegview-test-password`.

Keep this archive checked in rather than creating it as part of the tests: Ubuntu 20.04 ships
libzip 1.5.1, while libzip added support for *writing* traditional PKWARE encryption in 1.7.0.
The application only needs libzip's older read/decrypt support. The encrypted-ZIP tests therefore
use this fixture to exercise reading without depending on the installed library's writer support.
On a system with a libzip version that can write this method, regenerate it with:

```sh
make -C linux regen-encrypted-zip-fixture
```

`jxr-row-height.jxr` is a 3×3 lossless JPEG XR image with a different color pattern on each
row. It exercises the decoder's one-row rectangle conversion. Its source is `jxr-row-height.ppm`;
the checked-in `.jxr` was encoded with jxrlib 1.2's `JxrEncApp` using the 24-bit BGR input format
and lossless quality. Regenerate it from the repository root with:

```sh
convert linux/tests/fixtures/jxr-row-height.ppm BMP3:out/tmp/jxr-row-height.bmp
JxrEncApp -i out/tmp/jxr-row-height.bmp \
  -o linux/tests/fixtures/jxr-row-height.jxr -c 0 -q 1
```
