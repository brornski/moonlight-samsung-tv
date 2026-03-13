# Moonlight Samsung TV (NaCl)

Modified [Moonlight](https://github.com/moonlight-stream) game streaming client for Samsung Smart TVs running Tizen OS 3.0-6.0. Forked from [OneLiberty/moonlight-tizen-nacl](https://github.com/OneLiberty/moonlight-tizen-nacl).

Built and tested on the **Samsung UN75RU9000FXZA** (2019, Tizen 5.0) with the **Samsung OneRemote (TM1950A)**.

## What's Changed

The stock Moonlight NaCl app - sideloaded via tizen studio - has no way to exit a stream or change settings using the Samsung OneRemote. Created due to multiple blackscreens changing resolution, and the inability to return to the landing screen of moonlight. This fork fixes that:

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

## Install Guide

This is a step-by-step guide to sideload Moonlight onto your Samsung Smart TV. No prior experience with Tizen development is required.

### What You Need

- A **Samsung Smart TV** running Tizen 3.0 - 6.0 (most 2017-2021 models)
- A **Windows, Mac, or Linux PC** on the same Wi-Fi network as your TV
- **[Sunshine](https://github.com/LizardByte/Sunshine)** installed on your gaming/host PC
- About 15-20 minutes

### Step 1: Install Tizen Studio on Your PC

Tizen Studio is Samsung's development toolkit. You only need it to push the app to your TV.

1. Download **[Tizen Studio](https://developer.tizen.org/development/tizen-studio/download/)** and run the installer.
2. During installation, you only need the **Tizen SDK tools** — you can skip optional packages to save space.
3. After installation, note where it was installed (default: `C:\tizen-studio` on Windows, `~/tizen-studio` on Mac/Linux).
4. Open the **Tizen Studio Package Manager** and make sure **Samsung Certificate Extension** is installed under the Extensions tab.

### Step 2: Create a Samsung Certificate

Your TV will only run sideloaded apps that are signed with a certificate. You need to create one the first time.

1. Open **Tizen Studio** and go to **Tools > Certificate Manager**.
2. Click the **+** button to create a new certificate profile.
3. Select **Samsung** as the certificate type.
4. For the **Author Certificate**, select **Create a new author certificate**. Enter any name and password you'll remember.
5. You'll need to sign in with a **Samsung account**. If you don't have one, create one at [account.samsung.com](https://account.samsung.com).
6. For the **Distributor Certificate**, select **Use the default distributor certificate**.
7. Click **Finish**. Your certificate profile is now active.

> **Important:** The certificate is tied to your PC. You can only update or reinstall the app from the same computer you created the certificate on. If you switch PCs, you'll need to uninstall the app from the TV first and create a new certificate.

### Step 3: Enable Developer Mode on Your TV

1. Turn on your Samsung TV.
2. Open the **Apps** panel (press the Smart Hub / Home button, navigate to Apps).
3. Using your remote, enter **`12345`** on the on-screen number pad (or via the remote's 123 button). A dialog should pop up.
4. Toggle **Developer Mode** to **ON**.
5. Enter your **PC's local IP address** (the computer where you installed Tizen Studio). To find it:
   - **Windows:** Open Command Prompt and type `ipconfig`. Look for your IPv4 address (e.g., `10.0.0.188` or `192.168.1.100`).
   - **Mac:** Open Terminal and type `ifconfig | grep inet`. Look for your local IP.
6. **Restart your TV** (power off and back on). You should see "DEVELOP MODE" at the top of the Apps panel.

> **Tip:** If the `12345` dialog doesn't appear, make sure you're in the **Apps** section (not the home screen) and try typing the code slowly.

### Step 4: Find Your TV's IP Address

You'll need your TV's local IP address to connect to it.

1. On your TV, go to **Settings > General > Network > Network Status > IP Settings**.
2. Note the IP address (e.g., `10.0.0.222` or `192.168.1.50`).

### Step 5: Connect to Your TV

Open a terminal/command prompt on your PC and navigate to the Tizen Studio tools directory:

**Windows:**
```
cd C:\tizen-studio\tools
```

**Mac/Linux:**
```
cd ~/tizen-studio/tools
```

Connect to your TV:
```
sdb connect YOUR_TV_IP
```

Verify the connection:
```
sdb devices
```

You should see your TV listed as a connected device. If it says `offline`, try restarting your TV and reconnecting.

### Step 6: Download and Install the App

1. Download the latest **`MoonlightNaCl.wgt`** from the [Releases](https://github.com/brornski/moonlight-samsung-tv/releases) page.

2. Install it to your TV:

   **Windows:**
   ```
   C:\tizen-studio\tools\ide\bin\tizen install -n MoonlightNaCl.wgt -s YOUR_TV_IP:26101
   ```

   **Mac/Linux:**
   ```
   ~/tizen-studio/tools/ide/bin/tizen install -n MoonlightNaCl.wgt -s YOUR_TV_IP:26101
   ```

   You should see output ending in `Installed the package successfully`.

3. On your TV, open the **Apps** panel. Moonlight should appear in your app list.

### Step 7: Pair with Sunshine

1. Make sure **[Sunshine](https://github.com/LizardByte/Sunshine)** is running on your host PC. You can access its web UI at `https://localhost:47990`.
2. Open **Moonlight** on your TV.
3. Your host PC should appear automatically (or click **Add Host** and enter your PC's IP address).
4. Select your PC. A **4-digit PIN** will appear on the TV.
5. Enter that PIN in the **Sunshine web UI** under the Clients/PIN tab.
6. Once paired, your PC's apps/games will show up in Moonlight. Select one to start streaming.

### Troubleshooting

#### "Certificate error" or "Author certificate not match" on install
Your TV already has a version of Moonlight signed with a different certificate (e.g., from a different PC). You need to uninstall the old version first:
- Open the **Apps** panel on your TV, long-press on Moonlight, and select **Delete**.
- Then retry the install command.

#### sdb connect says "failed to connect"
- Make sure your PC and TV are on the **same Wi-Fi network**.
- Make sure **Developer Mode** is enabled and your **PC's IP** is entered correctly on the TV.
- Try restarting the TV after enabling Developer Mode.

#### "Loading Moonlight plugin..." hangs forever
This usually means the NaCl binary is missing or corrupted. Make sure you downloaded the `.wgt` from the Releases page and not just the source code.

#### Black screen when streaming
- Anything above **1080p** in Moonlight's settings will cause a black screen on many Samsung models.
- Wait **15 seconds** and the app will auto-exit.
- Change your **PC's display resolution** instead of changing Moonlight's resolution setting.

#### App doesn't appear after install
- Make sure the install command showed `Installed the package successfully`.
- Try closing and reopening the Apps panel on your TV.
- Check that your TV's Tizen version is between 3.0 and 6.0.

#### Git Bash on Windows: sdb paths don't work
Git Bash mangles Unix-style paths. Prefix your commands with:
```
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" sdb connect YOUR_TV_IP
```
Or use **Command Prompt** or **PowerShell** instead of Git Bash.

## Build from Source
- Install the Pepper SDK
- Clone the repo and run `make`
- Translate PNaCl to NaCl ARM: `pnacl-translate -arch arm moonlight-chrome.pexe -o moonlight-chrome-arm.nexe`
- Package with Tizen Studio: `tizen package -t wgt -s YOUR_PROFILE -- .`

## Credits
- [Moonlight for Chrome OS](https://github.com/moonlight-stream/moonlight-chrome) by Moonlight Developers
- [OneLiberty/moonlight-tizen-nacl](https://github.com/OneLiberty/moonlight-tizen-nacl) - original NaCl port for Samsung TVs
- [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) - open source game streaming host
