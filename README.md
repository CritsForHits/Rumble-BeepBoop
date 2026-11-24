<div align="center">

### A lightweight Rumble integration for OBS Studio

**Chat · Alerts · Text-to-Speech · Goal Progress Bars**

[Author on Rumble](https://rumble.com/user/CritsForHits)

Apache 2.0 + Common Clause

[0.42b Demo](https://rumble.com/v7it4ve-rumble-bb-obs-plugin-demo.html)

</div>

---

## Overview

**Rumble-BeepBoop** is a lightweight OBS Studio plugin that connects directly to the Rumble API.

Everything runs on your own PC. No browser source, external hosting, or intermediary service is required.

Super fast, no lag, no connection issues.

Commercial-Free, including assets.

(Slight exception - You are not allowed to copy my code or assets and then try to pawn them off as your own.)

## Features

- **Chat** — Display Rumble Chat as a fading stack in OBS.
- **Alerts** — Display stream alerts.
- **Likes VFX** — Little reminder, displays a VFX everytime user hits thumbs up. 
- **Text-to-Speech (TTS)** — Chatterbox interface, rants and/or chats can be voiced easily with your own wav file.
- **TTS Voice Mimic** — Users can input !voice to mimic a character - if available.
- **Goal Progress Bars** — Sub/Follower total/monthly/weekly/daily goals with progress bar and scene change on completion.
- **Local operation** — All local, all inside OBS (except chatterbox), super light-weight.

## Installation

1. Download and extract the plugin archive.
2. Copy the extracted plugin files into your OBS Studio plugins folder.
3. Restart OBS Studio.

## 0.4 Updates (Beta)

- Removed excess user options for chatterbox - (chatterbox was ignoring it anyways)
- Decided to stick with turbo chatterbox until project is finished - Removed language option.
- Fixed chatterbox's refresh connection to work if chatterbox was previously dc'd.
- Added Likes VFX (Remind user to hit that thumbs up without you saying it)
- Added !voice mimic for viewer fun.
(Requires you to have a voice file matching. E.G. spongebob.wav - user's input !spongebob Hi - voice in spongebob "Hi")
- New additional assets - Check them out!

## 0.41

- Fixed Likes VFX, no longer loops.
- Renamed Male_1 and Male_2 to MaleA and MaleB (Stop confusion for mimic)

## 0.42

- Fixed !mimic function
- Added grey whale badge

## Chatterbox

Chatterbox is open sourced (here on GitHub) AI TTS.
I've included an installer which does the following:
- Installs basic prereqs for chatterbox (E.G. Python3)
- Installs Chatterbox from GitHub
- Creates an API python script to run Chatterbox as a API.
- Creates a run script to start Chatterbox.

To add a custom voice, drop a 5-20 second clear .WAV file of the voice you wish to use into Chatterbox/voices.
We'll use Chattebox's turbo model: Requirements = 4GB of VRAM (English Only).
Chatterbox supports audio features such as [gasp] or [laugh] inputs.
Chatterbox is commercial-free.

Installing: Run the powershell script (Windows - ps1) or the bash script (Linux - sh)

If powershell script fails, try one or more of the following in your powershell terminal:

	.\install_chatterbox.ps1 -UpdateRepo

	Set-ExecutionPolicy -Scope Process Bypass

If bash script fails, try in your terminal:

	chmod +x install_chatterbox.sh

## Images and Animations

I've included a couple of progress bar overlays and a few alert .webm videos with audio and transparency. Use them as you wish or use your own.

## Credits (Copyright)

*This is under Apache 2.0 + Common Clause License*

**You ARE allowed to use plugin for stream profit - No need for credit. (Although it's appreciated)**

**You are NOT allowed to use this code or fork it for profit.**

Feel safe to use this commercially free in your Rumble stream.

All assets used in this plugin were made with Apache 2.0 license and royalty free media.

(Note: Animations were done with LTX: LTX is commercially free for under 10 million dollars annual revenue.)

(Final Note: I'm not a lawyer, this isn't legal advice. Use at your own risk.)

---

<div align="center">

*This is not an official rumble app. Do not contact rumble support about this app. Drop by my stream and post a complaint or suggestion.*

</div>
