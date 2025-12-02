# Tiny Journal (M5Stack Cardputer)

Tiny Journal is a distraction-light journalling and notes firmware for the **M5Stack Cardputer**.  
It stores plain-text notes on the SD card using simple folders and templates, and can optionally sync them to **Google Drive** over Wi-Fi.

Audio notes can be recorded anywhere with opt + tab.

Word jump with opt + left/right arrow keys.

Word delete with opt + backspace.

Page up/down with opt + up/down arrow keys.

Exit text editor with esc (fn + `)

Bluetooth keyboard support new for v2025.11.2

Ctrl for word jump and word delete. Page up and down, Home and End supported.

US and UK keyboard layouts only.

## Features

- Plain-text notes written directly on the Cardputer keyboard  
- Category-based folder structure on SD:
  - `daily`, `notes`, `meetings`, `projects`, `travel`, `misc`, `archive`
- Template-driven note creation (daily notes, meeting notes, etc.)
- Unique filenames for every note (even multiple notes on the same day)
- Audio note recording to `/journal/audio` with export to `/journal/audio/export`
- Optional Wi-Fi + Google Drive sync for notes and exported audio
- On-device Wi-Fi setup UI (scan networks, enter password, save to SD)
- Safe “initialise / reset journal” function to rebuild folders, templates, and config files

---

## Requirements

### Hardware

- M5Stack Cardputer (ESP32-S3)
- microSD card formatted as **FAT32**

### Software

- Arduino IDE 2.x
- ESP32 / M5Stack board support
- M5Stack Cardputer / M5Unified libraries (via Arduino Library Manager)

---

## Building and flashing

1. Clone the repo:

    git clone https://github.com/joejee90/Tiny-Journal.git
    cd Tiny-Journal

2. Open the main `.ino` file (e.g. `sketch_nov29a.ino`) in **Arduino IDE**.

3. In **Tools → Board**, select the appropriate **M5Stack / Cardputer (ESP32-S3)** board.

4. Select the correct **Port** for your Cardputer.

5. Click **Verify** to compile, then **Upload** to flash the firmware.

Alternatively, you can flash a prebuilt `.bin` from the **Releases** page using **M5Burner** or `esptool.py`.

---

## SD card layout

On first run, Tiny Journal will create a `/journal` folder on the SD card and set everything up:

    /journal
      /templates
      /daily
      /notes
      /meetings
      /projects
      /travel
      /misc
      /archive
      /audio
        /export
      wifi.cfg
      gdrive.cfg
      theme.cfg
      CONFIG-README.txt

- **Templates** in `/journal/templates` are used as the starting text for new notes.  
- **Audio notes** are recorded under `/journal/audio`; exported audio files go to `/journal/audio/export`.  
- `CONFIG-README.txt` is a plain-text guide written by the firmware to explain the config files on-device.

---

## Configuration files

The firmware auto-generates placeholder config files and a helper file:

- `/journal/wifi.cfg`
- `/journal/gdrive.cfg`
- `/journal/theme.cfg`
- `/journal/CONFIG-README.txt`

You can edit them either:

- on your computer with a text editor, or  
- via the built-in settings menus on the Cardputer.

### Wi-Fi config: `wifi.cfg`

**Path:** `/journal/wifi.cfg`  
**Format:** two lines only

Line 1: WiFi SSID  
Line 2: WiFi password  

Example:

    MyHomeWiFi
    supersecretpassword123

On first run, the firmware creates something like:

    YourSSIDHere
    YourPasswordHere

You can:

- edit the file directly on the SD card, or  
- configure Wi-Fi on the device:

> `Fn` + `` ` `` → **Settings** → **Sync & connectivity** → **WiFi setup**

(the same steps are described on-device in `CONFIG-README.txt`).

### Google Drive config: `gdrive.cfg`

**Path:** `/journal/gdrive.cfg`  
**Format:** four lines

Line 1: `refresh_token`  
Line 2: `client_id`  
Line 3: `client_secret`  
Line 4: `folder_id`  

On first run the firmware writes placeholder values, for example:

    YOUR_REFRESH_TOKEN_HERE
    YOUR_CLIENT_ID_HERE
    YOUR_CLIENT_SECRET_HERE
    YOUR_DRIVE_FOLDER_ID_HERE

You need to replace these with real values from your own **Google Cloud** project and Google Drive:

- `refresh_token` — OAuth refresh token for your app  
- `client_id` — from the OAuth client in Google Cloud Console  
- `client_secret` — from the same OAuth client  
- `folder_id` — the ID of the Google Drive folder to sync into  

This project assumes you already know how to:

- create an OAuth client and obtain a refresh token, and  
- find a Google Drive folder ID.  

If you are not comfortable with Google’s APIs, this firmware is probably not the best starting tutorial.

---

## Basic usage

### Navigation

- Use the cursor keys / D-pad to move the selection.  
- **Enter** selects.  
- On the Cardputer, **Esc** is `Fn` + `` ` `` (top-left key).

### Notes and templates

- Choose a category (e.g. **Daily**, **Notes**, **Meetings**).  
- When you create a new note:
  - The firmware loads a matching template from `/journal/templates`.  
  - It saves the note into the category folder (e.g. `/journal/daily`).  
  - Filenames are based on the date, with a numeric suffix added where needed to keep every note name unique.

Notes are automatically saved when:

- you exit the editor,  
- you switch documents, or  
- you power off the device.  

There is no manual “Save” button; it writes changes as needed.

### Audio notes

- Audio is recorded into `/journal/audio`.  
- Exported files (for use on a computer) are written to `/journal/audio/export`.  
- Exported files are eligible for Google Drive sync (see below).

### Google Drive sync

Once `wifi.cfg` and `gdrive.cfg` are correctly filled in:

On the Cardputer:

> `Fn` + `` ` `` → **Settings** → **Sync & connectivity** → **Sync now**

The status line will show:

- Wi-Fi connect / fail  
- Google auth success / failure  
- Upload finished or aborted state  

Tiny Journal attempts to sync:

- text notes in the journal folders  
- exported audio notes in `/journal/audio/export`  

It deliberately **does not** upload:

- `wifi.cfg`, `gdrive.cfg`, `theme.cfg`  
- `CONFIG-README.txt`  
- other internal config / system files  

---

## Reset / initialise journal

From the **Settings / Utilities** menu there is an option to **reset / initialise** the journal:

- Re-creates the `/journal` folder structure.  
- Restores default templates and placeholder config files.  
- Lets you decide whether to **keep or delete** existing Wi-Fi and Google Drive configs.  

This may delete existing notes and audio, so treat it as a “fresh start” option.

---

## Licence

This project uses the same basic licence terms as the **Cardputer Camera Companion** project:

- You may **clone, fork, and modify** the source code for your own **personal, non-commercial** use.  
- You may **not sell**:
  - compiled binaries of this firmware, or  
  - devices with this firmware pre-installed,  
  without explicit permission from the author.  
- You may share forks and modified versions **non-commercially**, as long as:
  - this licence notice (or an equivalent) is kept, and  
  - it is clear that your fork is **unofficial** and **not supported** by the original author.  
- The software is provided **“as is”, without any warranty** of any kind. You use it entirely at your own risk.

In particular:

- The Wi-Fi and Google Drive sync features are **experimental**.  
- There is **no guarantee** against bugs, crashes, or data loss.  
- The author accepts **no responsibility** for:
  - lost or corrupted notes,  
  - damaged SD cards,  
  - failed or partial syncs,  
  - or any other problems arising from use of this firmware.  

If you want to use Tiny Journal (or any substantial part of its code) in a **commercial** context, please contact the author first.
