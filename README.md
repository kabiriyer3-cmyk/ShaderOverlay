# ShaderOverlay

ReShade-style post-processing for games that ReShade can't hook. Nothing is injected into the game:
the app captures your monitor, runs the effects on your GPU (Direct3D 11), and draws them in a
click-through window on top of the game.

## Get the .exe
- **No install:** push this folder to a GitHub repo, open the **Actions** tab, run "build", download the `ShaderOverlay` file it produces.
- **Visual Studio 2022:** open "x64 Native Tools Command Prompt", `cd` here, run `build.bat`.
- **MSYS2 / mingw-w64:** `sh build_mingw.sh`

## Use
1. Set the game to **Borderless** or **Windowed** (exclusive fullscreen draws over any overlay).
2. Run `ShaderOverlay.exe`. The settings menu opens.
3. Hotkeys (defaults, change them in `ShaderOverlay.ini`):
   - **F8**: effects on/off
   - **End**: open/close the overlay menu
   - **Ctrl+F12**: quit (saves your settings)

Effects: CAS sharpen, vibrance, saturation, contrast, brightness, gamma, color temperature, vignette.
`[General] Monitor=1` in the ini picks your second monitor.

## Limits
- Needs Windows 10 2004+ or Windows 11.
- No depth-buffer effects (no SSAO, DOF, or MXAO), because it only sees the final image.
- About 1 frame of added latency. Skip it for competitive shooters.
- Fullscreen DRM video and UAC prompts can't be captured (the overlay pauses there).

## Using real ReShade on top of ShaderOverlay
1. Run the ReShade installer, pick **ShaderOverlay.exe**, choose **DirectX 10/11/12**, install the shaders you want.
2. Start ShaderOverlay. You'll see ReShade's loading banner.
3. Press **F10** (ReShade input mode). The effect window takes keyboard and mouse focus.
4. Now press ReShade's menu key (Home by default). Tweak away.
5. Press **F10** again to hand control back to the game.

ShaderOverlay never registers Home, End or Insert, so ReShade always gets them.
ReShade only sees the final screen image here, so depth effects (SSAO, DOF) still won't work.

## Labs (experimental): show effects in screen share
Menu > **Labs** > "Show effects in screen share". Discord / OBS full-screen share then sees your effects
(and ReShade, if it's installed on ShaderOverlay.exe). It captures the focused game window instead of
the whole screen, follows alt-tab, and covers anything on top of the game. Borderless/windowed only.
Fast capture needs the GitHub / Visual Studio build; the MinGW build falls back to a slower method.
On Windows 10 a yellow capture border may show around the game (Windows 11 hides it).

## ???
There might be a crab. Try the version badge.
