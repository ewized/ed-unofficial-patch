# OpenVR 2.15.6 header

`openvr.h` is the unmodified public SDK header from Valve's
[v2.15.6 release](https://github.com/ValveSoftware/openvr/tree/v2.15.6).
Its SHA-256 is
`21f21ff11f23cbbf51297e844c8111eb6eba952d4bc039a2a8f55e8f18597bde`.

`ivrclientcore.h` is the unmodified [v2.15.6 SDK source declaration](https://github.com/ValveSoftware/openvr/blob/v2.15.6/src/ivrclientcore.h)
for `IVRClientCore_003`. Its SHA-256 is
`07c8ce981a59fb7cd1dc30572f0c9972d98e6275e95527f0f7a74a18bc0cf846`.
SteamVR exposing that interface to its OpenXR application is a compatibility
path, with full-rate rendering when the existing core refuses the lookup.
During an admitted SteamVR/OpenXR instance epoch, the adapter requires one
already loaded `vrclient_x64.dll` image before borrowing any interface. It
checks that image's base, full path and factory owner on each borrow. This
works with alternate installs and refuses ambiguous images, incomplete module
snapshots and paths that do not fit the module enumeration buffer.

The provider translation unit aliases the header's namespace while including
it; the header itself is unchanged. This keeps its interface 026 types
separate from Elite's interface 012 declarations.

The SDK is distributed under BSD-3-Clause. The original license is retained
in [LICENSE.txt](LICENSE.txt) and the project-level third-party notices.
Upstream license: <https://github.com/ValveSoftware/openvr/blob/v2.15.6/LICENSE>.
