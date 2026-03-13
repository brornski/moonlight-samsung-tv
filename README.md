# Moonlight Samsung TV (NaCl)

Modified [Moonlight](https://github.com/moonlight-stream) game streaming client for Samsung Smart TVs running Tizen OS 3.0-6.0. Forked from [OneLiberty/moonlight-tizen-nacl](https://github.com/OneLiberty/moonlight-tizen-nacl).

Built and tested on the **Samsung UN75RU9000FXZA** (2019, Tizen 5.0) with the **Samsung OneRemote (TM1950A)**.

## What's Changed

The stock Moonlight NaCl app has no way to exit a stream or change settings using the Samsung OneRemote (which has no physical color buttons). This fork fixes that:

- **Back button exits the stream** - Press Return/Back on your remote to leave a stream and return to the app list. The original app had no way to do this on the OneRemote.
- **In-stream settings overlay** - Press Play/Pause during a stream to open a settings panel. Change resolution, framerate, and bitrate without reinstalling the app.
- **15-second auto-exit** - If no video is received within 15 seconds (black screen), the app automatically exits the stream so you never get stuck.

## Remote Button Map

### During a stream
| Button | Action |
|--------|--------|
| **Back (Return)** | Exit stream, return to app list |
| **Play/Pause** | Open settings overlay |

### Inside the settings overlay
| Button | Action |
|--------|--------|
| **D-pad Up/Down** | Move between settings |
| **D-pad Left/Right** | Change value |
| **Select** | Confirm / cycle value |
| **Back** | Close overlay, resume stream |
| **Apply & Restart Stream** | Save settings and exit (relaunch to apply) |

## Important Notes

- After testing, anything above **1080p** in Moonlight's settings will result in a black screen on this Samsung model. You are better off changing the resolution in your **PC's display settings** rather than in Moonlight.
- **Bitrate and refresh rate** can be changed freely in the overlay.
- If you find yourself on a black screen, **wait 15 seconds** and the app will auto-close.
- This app requires [Sunshine](https://github.com/LizardByte/Sunshine) running on your host PC.

## Install

### Prerequisites
- Samsung TV in Developer Mode
- [Tizen Studio](https://developer.tizen.org/development/tizen-studio/download/) installed on your PC
- Sunshine running on your host PC

### Steps
1. **Enable Developer Mode on your TV**: Go to Apps, enter `12345`, toggle Developer Mode on, enter your PC's IP, restart the TV.
2. **Connect via sdb**:
   ```
   sdb connect YOUR_TV_IP
   sdb devices
   ```
3. **Download the .wgt** from [Releases](https://github.com/brornski/moonlight-samsung-tv/releases) or build from source.
4. **Install**:
   ```
   tizen install -n MoonlightNaCl.wgt -s YOUR_TV_IP:26101
   ```
5. Open Moonlight from your TV's app list and pair with your Sunshine host.

## Build from Source
- Install the Pepper SDK
- Clone the repo and run `make`
- Translate PNaCl to NaCl ARM: `pnacl-translate -arch arm moonlight-chrome.pexe -o moonlight-chrome-arm.nexe`
- Package with Tizen Studio: `tizen package -t wgt -s YOUR_PROFILE -- .`

## Credits
- [Moonlight for Chrome OS](https://github.com/moonlight-stream/moonlight-chrome) by Moonlight Developers
- [OneLiberty/moonlight-tizen-nacl](https://github.com/OneLiberty/moonlight-tizen-nacl) - original NaCl port for Samsung TVs
- [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) - open source game streaming host
