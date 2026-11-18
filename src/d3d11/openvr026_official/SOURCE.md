# OpenVR 2.15.6 header

`openvr.h` is the unmodified public SDK header from Valve's
[v2.15.6 release](https://github.com/ValveSoftware/openvr/tree/v2.15.6).
Its SHA-256 is
`21f21ff11f23cbbf51297e844c8111eb6eba952d4bc039a2a8f55e8f18597bde`.

The provider translation unit aliases the header's namespace while including
it; the header itself is unchanged. This keeps its interface 026 types
separate from Elite's interface 012 declarations.

The SDK is distributed under BSD-3-Clause. The original license is retained
in [LICENSE.txt](LICENSE.txt) and the project-level third-party notices.
Upstream license: <https://github.com/ValveSoftware/openvr/blob/v2.15.6/LICENSE>.
