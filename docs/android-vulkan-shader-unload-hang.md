# Android Vulkan shader-unload hang on Mali

Status: tabled for separate investigation

## Summary

On the RG477M (Android 14, MediaTek/Mali), unloading a video shader preset with
the `Y` action can freeze presentation while the RetroArch process remains
alive. The failure reproduces without backgrounding the application.

Android reports that the BLAST buffer queue has exhausted its acquired frames:

```text
BLASTBufferQueue: Already acquired max frames 7 max:5 + 2
```

This is a presentation/acquire deadlock or leak pattern, not a native crash.

## Environment

- Device: RG477M
- OS: Android 14
- GPU stack: MediaTek / Mali (`mt6897`)
- Package: `com.retroarch.aarch64`
- Video driver: Vulkan
- Trigger: open the shader preset menu and press `Y` to unload the preset

## Reproduction

1. Start RetroArch with Vulkan and load content.
2. Apply a video shader preset.
3. Open the shader preset menu.
4. Press `Y` over the preset action to unload it.

The freeze may not occur every time, but on the captured baseline it reproduced
on the first attempt. No Home/background/resume cycle occurred beforehand.

## Evidence

The decisive reproduction used commit
`76796ee6d0e41abd44fd73dcfaa71e9c7787fb42`, the immediate parent of the
framebuffer-resize PR. It includes the merged swapchain-capacity and
shader-menu-recursion fixes, but excludes both of the following:

- framebuffer resize lifetime fix (#19194)
- framebuffer memory recycle follow-up (`15f239eae8`)

The log begins reporting BLAST exhaustion at `10:41:58.488` and records at
least 317 repeated failures through `10:42:14.855`:

```text
BLASTBufferQueue: ... Already acquired max frames 7 max:5 + 2
```

Captured log:

`/Users/matt/Downloads/RetroArch-pre-framebuffer-y-unload-hang-20260716-104236.txt`

## Conclusions

- The framebuffer resize fix and its recycler follow-up did not introduce this
  hang: the bug reproduces without either change.
- The swapchain-capacity fix is not implicated by this capture: the active
  queue limit is three images, well below the old capacity boundary.
- The shader-menu recursion fix is required to avoid an older immediate
  recursion crash. It makes the normal unload path reachable, but the BLAST
  starvation is a separate failure in that path.
- This should not block the independently validated Android resume or
  cached-menu-frame PRs.

## Recommended follow-up

Investigate this as an isolated Android Vulkan issue. Instrument the shader
unload path around acquire, submit, present, and every early-return/cleanup
branch to find where an acquired swapchain image stops being presented or
released. Compare that path with a successful shader apply. Do not fold this
work into the framebuffer, resume, or cached-menu PRs without a minimal,
reproduced fix.
