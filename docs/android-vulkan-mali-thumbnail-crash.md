# Android Vulkan crash on RG477M / Mali

Date observed: 2026-07-07 through 2026-07-09

## Summary

Recent RetroArch AArch64 builds crash on startup on an RG477M when `video_driver = "vulkan"` is used. No tested nightly should currently be treated as proven stable; the earlier 30-second result from an April 5 rebuild was not a sufficient stability test.

The crash is a native SIGSEGV in the vendor Mali driver during RetroArch frame rendering:

```text
Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x7300000000
Cmdline: com.retroarch.aarch64
#00 /vendor/lib64/egl/mt6897/libGLES_mali.so
#01 /vendor/lib64/egl/mt6897/libGLES_mali.so
#02 libretroarch-activity.so
#03 libretroarch-activity.so
#04 libretroarch-activity.so
```

In some traces, the RetroArch frames include `video_driver_frame`, `runloop_iterate`, and `rarch_main`.

Disassembly of the exact crashing AArch64 APK resolves RetroArch PC `0x0d07600` to the indirect call of `vkCmdBeginRenderPass`. The function pointer comes from the GOT relocation for `vulkan_symbol_wrapper_vkCmdBeginRenderPass`. This rules out the earlier font, Ozone, frame-copy, and BCn-upload theories as the immediate crash boundary.

## Device

- Device: RG477M
- Android: 14
- SoC/GPU stack: MediaTek / Mali, `mt6897`
- Package: `com.retroarch.aarch64`

## Tested Config

The crash reproduces with Vulkan even after reducing menu and rendering extras:

```text
video_driver = "vulkan"
menu_driver = "ozone"
menu_thumbnails = "0"
menu_left_thumbnails = "0"
run_ahead_enabled = "false"
video_threaded = "true"
video_vsync = "true"
video_shader_enable = "false"
video_gpu_screenshot = "false"
video_shader_watch_files = "false"
```

Switching only the video driver to GL avoids the crash on affected builds:

```text
video_driver = "gl"
```

## Build Behavior Observed

- 2026-07-08 official buildbot nightly: crashes with Vulkan.
- 2026-07-07 official buildbot nightly: crashes with Vulkan.
- 2026-07-05 official buildbot nightly: crashes with Vulkan.
- 2026-07-04 official buildbot nightly: crashes with Vulkan.
- 2026-07-03 official buildbot nightly: crashes with Vulkan.
- 2026-07-02 official buildbot nightly: crashes with Vulkan.
- 2026-06-24 official buildbot nightly: also reproduced the same Vulkan crash in later testing.
- 2026-04-05 upstream commit `f5a41dbb045c909ee9dd826f4201c8499adf07b8`, rebuilt via GitHub Actions: survived one 30-second startup test, but was not tested long enough to call stable.

The 2026-04-05 GitHub Actions artifact was rebuilt because the original upstream artifact had expired and official buildbot no longer served April Android nightlies.

## Current Test Build

Targeted test build awaiting device validation:

```text
commit: a71e39b150a73e05dc8465ab1c56b9b2d9f67bc3
branch: codex/fix-android-vulkan-stale-swapchain
source base: upstream libretro/RetroArch 18f01661cf
build route: GitHub Actions CI Android
apk: phoenix-aarch64-debug.apk
package: com.retroarch.aarch64
video_driver = "vulkan"
```

The patch rebuilds driver-owned framebuffers at the start of `vulkan_frame()` whenever the context has invalidated or recreated its swapchain. It also rejects out-of-range frame/image indices and refuses to call `vkCmdBeginRenderPass` with a null framebuffer.

## Suspect Change Window

The earlier July 3 thumbnail theory is now considered unlikely to be the root cause. July 2 still crashes with Vulkan, and the reduced config crashes even with menu thumbnails disabled.

There is not yet a trustworthy good/bad nightly boundary. The reliable evidence is the call-site mapping above.

The current source permits `vulkan_acquire_next_image()` to destroy and recreate the context swapchain after an out-of-date result. It sets `VK_CTX_FLAG_INVALID_SWAPCHAIN`, but `vulkan_frame()` previously rebuilt driver-owned framebuffers only near the end of the next frame. That left a window where the first render pass could combine the new swapchain state with an old framebuffer handle. Mali crashes while consuming that handle in `vkCmdBeginRenderPass`.

The targeted fix closes that lifetime window by handling `VK_CTX_FLAG_INVALID_SWAPCHAIN` before frame and swapchain indices are read or command recording begins. On-device validation is still required.

## Practical Notes

- Official buildbot Android nightlies currently start at 2026-06-25, so the likely-good April nightly is not available from buildbot.
- GitHub Actions metadata still listed older Android artifacts, but the April 5 artifact ZIP had expired.
- The fork-built debug APK has a different signing key than buildbot nightlies, so Android requires uninstalling the buildbot package before installing it.
- Back up and restore `/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg` when switching between buildbot and GitHub debug builds.

Suggested issue title:

```text
Android AArch64 Vulkan startup crash on MediaTek/Mali after early-April Vulkan renderer changes
```
