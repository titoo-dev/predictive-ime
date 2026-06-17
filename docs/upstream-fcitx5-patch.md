# Upstreaming the waylandim public-API patch to fcitx5

Goal: let **external UI addons** create a caret-positioned
`zwp_input_popup_surface_v2` by exposing the raw `zwp_input_method_v2` of the
current input context — so projects like predictive-ime's `qmlpanel` need no
fcitx5 fork. This is the real fix for the patched-fcitx5 problem; the local
patch ([`../ui/waylandim-public.patch`](../ui/waylandim-public.patch)) is only a
stopgap.

## Proposed change (PR description draft)

**Title:** waylandim: expose raw zwp_input_method_v2 to external UI addons

**Why.** `waylandim` already publishes `getInputMethodV2`, but it returns the
internal `fcitx::wayland::ZwpInputMethodV2` wrapper, whose header is not
installed — so external addons can't use it. The input-method-v2 protocol's
`get_input_popup_surface` is the *only* way to get a compositor-positioned
popup at the caret on Wayland; without raw access, third-party candidate UIs
can't do it and are stuck reimplementing/guessing positioning.

**What.**
1. Add `zwp_input_method_v2 *getInputMethodV2Raw(InputContext *)` to
   `WaylandIMModule` — a one-liner: `rawPointer(getInputMethodV2(ic))`.
2. Export it as a published addon function.
3. `INSTALL` the `WaylandIM` exported CMake module + `waylandim_public.h`
   (currently build-only) so external addons can `find_package` it.

**Compatibility.** Purely additive — no signature changes, no behaviour change,
no new dependencies. Existing `getInputMethodV2` consumers are untouched.

**Precedent.** fcitx5 already installs public modules for other frontends
(e.g. the `wayland` module's `wayland_public.h`); this brings `waylandim` in
line so the input-method frontend is extensible the same way.

## After it merges

- predictive-ime's `ui/CMakeLists.txt` keeps `find_package(Fcitx5Module
  COMPONENTS WaylandIM)` and `getInputMethodV2Raw` — they now resolve against
  stock fcitx5 ≥ the merged version.
- Drop the patched-fcitx5 recipe; `BUILD_UI=ON` works on any new-enough distro
  fcitx5. Document the minimum fcitx5 version.
- `qmlpanel` can keep a runtime guard (degrade to classicui) for older fcitx5.

## Status

- [ ] Open the PR against https://github.com/fcitx/fcitx5
- [ ] Link the issue/PR here and in `README.md`
