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
