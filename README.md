<div align="center">

### A lightweight Rumble integration for OBS Studio

**Chat · Alerts · Text-to-Speech · Goal Progress Bars**

[Author on Rumble](https://rumble.com/user/CritsForHits)

Apache 2.0 + Common Clause

</div>

---

## Overview

**Rumble-BeepBoop** is a lightweight OBS Studio plugin that connects directly to the Rumble API.

Everything runs on your own PC. No browser source, external hosting, or intermediary service is required.

## Features

- **Chat** — Display Rumble Chat as a fading stack in OBS.
- **Alerts** — Display stream alerts.
- **Text-to-Speech (TTS)** — Chatterbox interface, rants and/or chats can be voiced easily with your own wav file.
- **Goal Progress Bars** — Sub/Follower total/monthly/weekly/daily goals with progress bar and scene change on completion.
- **Local operation** — All local, all inside OBS (except chatterbox), super light-weight.

## Installation

1. Download and extract the plugin archive.
2. Copy the extracted plugin files into your OBS Studio plugins folder.
3. Restart OBS Studio.

## Chatterbox

Chatterbox is our AI TTS choice.
Chatterbox installer with royalty free voices included. Chatterbox itself is also commercially free - however no longer in development.
(Chatterbox requires 4gb of vram.
I've set up the chatterbox installer to use it's turbo model.
Chatterbox does support multilanguage but, as of now, my plugin is English only.
Although, with a few lines of code, it can easilly be added in.
Chatterbox is high quality and supports audio features such as [gasp] or [laugh].
It's two competing alternatives are Qwen Voice and Kokoro.
I went with Chatterbox because it's nearly as good as qwen, but uses much less
resources and Qwen is pay to play. Kokoro is very light-weight, but commercial-free; however, low quality and cumbersome to make
new voices.)

Add Chatterbox Voices: Obtain a ~20 second audio clip of the voice you'd wish to use as a .wav file. Drop it into Chatterbox/voices. Reconnect with Rumble-BeepBoop, you'll now see your new voice.

Installing: run the powershell script (Windows - ps1) or the bash script (Linux - sh)

If powershell script fails, try in your powershell terminal:

	.\install_chatterbox.ps1 -UpdateRepo

	Set-ExecutionPolicy -Scope Process Bypass

If bash script fails, try in your terminal:

	chmod +x install_chatterbox.sh

## Images and Animations

I've included a couple of progress bar overlays and a few alert .webm videos with audio and transparency. Use them as you wish or use your own.

## Credits (Copyright)

*This is under Apache 2.0 + Common Clause License*

**You ARE allowed to use plugin for stream profit. (Donos, subs, etc.)**

**You are NOT allowed to use this code or fork it for profit. (Copying this code and selling it? Better lawyer up)**

Feel safe to use this commercially free in your Rumble stream.

All assets used in this plugin were made with Apache 2.0 license and royalty free media.

(Note: Animations were done with BiRefNet and LTX: LTX is commercially free for under 10 million dollars annual revenue.)

(Final Note: I'm not a lawyer, this isn't legal advice. Use at your own risk.)

---

<div align="center">

*This is not an official rumble app. Do not contact rumble support about this app. Drop by my stream and post a complaint or suggestion.*

</div>
