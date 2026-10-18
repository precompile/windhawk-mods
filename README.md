# windhawk-mods

My mods for [Windhawk](https://windhawk.net/).

## NVIDIA App Tray Icon

[`mods/nvidia-app-tray-icon.wh.cpp`](mods/nvidia-app-tray-icon.wh.cpp)

NVIDIA driver updates often reset the registry values that show the NVIDIA App
("NVIDIA Settings") tray icon, so the icon disappears after every update while
the GPU activity icon stays. This mod keeps it shown (or hidden) with a toggle:

- Enforces `StartOnLogin` and `ShowInSedona` under
  `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\Parameters\NVTray`, plus
  `HKCU\Software\NVIDIA Corporation\NvTray\StartOnLogin`.
- Watches those keys and re-checks every minute, so a driver update that resets
  them gets undone.
- Restarts the *NVIDIA Display Container LS* service after a change, so the icon
  comes back without a reboot.

The machine-wide values are written from the Windhawk service (which runs as
SYSTEM), and the per-user value from a small background tool process.

### Install

1. In Windhawk, click **Create a new mod**.
2. Replace the template with the contents of `nvidia-app-tray-icon.wh.cpp`.
3. Click **Compile**, then **Enable**.

## License

MIT
