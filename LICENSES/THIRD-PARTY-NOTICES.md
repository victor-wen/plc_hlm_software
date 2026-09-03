# Third-Party Notices

This product (PLC HMI for Inovance H3U, version 0.1.0) includes the
following third-party components. Each component is used under the
license indicated; the full license texts are in this directory.

## Qt (LGPLv3)

- Component: Qt 6.8.x (Core, Widgets, SerialBus, SerialPort, Sql, Test)
- License: GNU Lesser General Public License v3 (LGPLv3)
- Text: `Qt-LGPLv3.txt`
- Official: https://www.gnu.org/licenses/lgpl-3.0.txt
- Usage: dynamically linked; the application is relinkable against a
  modified Qt. No modifications to Qt are made by this project.

## OpenCV (3-clause BSD)

- Component: OpenCV 4.x (core module, via vcpkg `opencv4`)
- License: 3-clause BSD
- Text: `OpenCV-LICENSE.txt`
- Official: https://opencv.org/license/
- Usage: dynamically linked, isolated in the `hlm_vision` module
  (spec §6, §17). Vision failure never blocks PLC control.

## OpenSSL (OpenSSL License + SSLeay)

- Component: OpenSSL 3.x Crypto (via vcpkg `openssl`)
- License: OpenSSL License + original SSLeay license
- Text: `OpenSSL-LICENSE.txt`
- Official: https://www.openssl.org/source/license.html
- Usage: password derivation in `hlm_core` (spec §17).

## vcpkg

- Component: vcpkg package manager and its installed dependency tree
  (qtbase, qtserialbus, qtserialport, openssl, opencv4)
- License: MIT (vcpkg itself); dependencies carry their own licenses
  as listed above.
- Official: https://github.com/microsoft/vcpkg
- Usage: dependency provisioning for the Windows build only; the
  installed binaries are redistributed under the licenses above.

## Microsoft Visual C++ Runtime

- Component: vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll
- License: Microsoft Visual C++ Redistributable license
- Official: https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist
- Usage: redistributed per the Microsoft Visual C++ Redistributable
  license terms.

## Note on license texts

The full LGPL-3.0 and OpenSSL license texts could not be embedded at
build time because the packaging environment had no network access
when the license files were generated (Task 21). Before shipping a
release, replace `Qt-LGPLv3.txt` and `OpenSSL-LICENSE.txt` with the
complete texts from the official URLs above.
