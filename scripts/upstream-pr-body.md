## Summary

Expose the raw `zwp_input_method_v2` of the current input context to external
UI addons, and install the `WaylandIM` public module so they can link it.

This lets a third-party UI addon create a compositor-positioned
`zwp_input_popup_surface_v2` (caret tracking) on Wayland — currently impossible
outside the `waylandim` frontend, because the only accessor returns the internal
`fcitx::wayland::ZwpInputMethodV2` wrapper whose header is not installed.

## Changes

- Add `zwp_input_method_v2 *WaylandIMModule::getInputMethodV2Raw(InputContext *)`
  — a one-liner: `rawPointer(getInputMethodV2(ic))`.
- Export it as a published addon function.
- `INSTALL` the already-existing `WaylandIM` exported CMake module +
  `waylandim_public.h` (currently build-tree only).

## Compatibility

Purely additive: no signature/behaviour changes, no new dependencies. Existing
`getInputMethodV2` consumers are unaffected. This mirrors how the `wayland`
module already installs its `wayland_public.h`, making the input-method frontend
extensible the same way.

## Motivation

Built a predictive input method (engine + n-gram daemon + Qt Quick candidate
bar). The candidate bar needs caret positioning via `get_input_popup_surface`;
without this, it requires patching/forking fcitx5 per distro. This change lets
such UI addons ship as plain addons on any distro.
