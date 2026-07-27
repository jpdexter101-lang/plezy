# Vendored Wayland protocol bindings

`color-management-v1` (staging) backs HDR passthrough on the native video
plane. The generated sources are committed rather than produced at build time
because the protocol only appeared in **wayland-protocols 1.41**, which is newer
than the version on the distributions this app is built for — vendoring keeps
the build working regardless of what the host ships, and adds no dependency on
`wayland-scanner`.

Generated from wayland-protocols 1.49 with wayland-scanner 1.25.0. The
interface is at version 3; KWin 6.4.3 implements version 1, and the client
binds `min(advertised, 3)`.

To refresh:

```sh
xml=/usr/share/wayland-protocols/staging/color-management/color-management-v1.xml
cp "$xml" linux/runner/wayland/
wayland-scanner client-header "$xml" linux/runner/wayland/color-management-v1-client-protocol.h
wayland-scanner private-code  "$xml" linux/runner/wayland/color-management-v1-protocol.c
```

Do not hand-edit the generated files.
