# RetroArch Repo Guidance

## Android Vulkan Investigations

- Use source-backed findings and real device behavior. Keep confirmed defects separate from compatibility risks.
- PPSSPP can fail before swapchain creation. Do not assume `VULKAN_MAX_SWAPCHAIN_IMAGES` or other swapchain settings explain an earlier feature-negotiation failure.
- When reviewing Vulkan device creation, inspect `VkPhysicalDeviceFeatures2`, `pEnabledFeatures`, and emulator-specific wrappers together.
- Known investigation areas include Dolphin's fixed-eight image storage and PPSSPP's Features2 wrapper. Reconfirm against the current source before changing code.

## Controlled CI APK Installs

- A signature mismatch between CI and installed builds is expected; do not bypass it with an uncontrolled reinstall.
- Back up configuration only, reinstall the CI build, then restore through `run-as` so restored files are owned by the app user, not `shell`.
- Verify the backup before uninstalling and verify restored ownership and app startup afterward.
- Do not treat a successful build or APK install as proof that the Vulkan path works. Validate on the target device and core.
