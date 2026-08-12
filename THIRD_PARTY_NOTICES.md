# Third-party software

This repository contains modified third-party decoder source code. Each
component remains under its own license:

- `components/freeaptx`: derived from
  [pali/libfreeaptx](https://github.com/pali/libfreeaptx), licensed under
  LGPL-2.1-or-later. The complete license text is included in
  `components/freeaptx/COPYING`.
- `components/libldacdec`: based on
  [hegdi/libldacdec](https://github.com/hegdi/libldacdec), revision
  `7f3bd6cc1586b764b02f2e4f20b16c7ff18758b9`, licensed under the MIT License.
  Its license is included in `components/libldacdec/LICENSE`. This project
  includes ESP-IDF packaging fixes and a single-precision DSP optimization for
  the original ESP32.
- `espressif/esp_audio_codec`: downloaded by the ESP-IDF Component Manager and
  not vendored in this repository. See its upstream package for license terms.
- `idf_patches`: patches and small derived portions targeting Espressif
  ESP-IDF, whose upstream source is licensed under Apache-2.0. Those derived
  portions remain subject to the applicable ESP-IDF license and notices.

LDAC and aptX are trademarks of their respective owners. This is an
independent community project and is not affiliated with or endorsed by Sony,
Qualcomm, or Espressif. Users are responsible for checking any codec patent or
licensing requirements that may apply in their jurisdiction and use case.
