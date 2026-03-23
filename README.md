# 📸 Better Screenshot for OBS Studio

Better Screenshot enhances OBS by allowing flexible screenshot formats, compression, storage, and direct sharing.

## Features
- Custom hotkey (Ctrl+Shift+S)
- Save as PNG, JPG, WEBP
- Custom save folder
- Discord webhook integration

## Installation
Copy plugin files into:
obs-studio/obs-plugins/64bit/

Restart OBS → Tools → Better Screenshot

## Configuration
- Set hotkey
- Choose format (PNG/JPG/WEBP)
- Enable local save + folder
- Optional Discord webhook + message

## Discord Webhook Setup
Server Settings → Integrations → Webhooks → New Webhook → copy URL

## Important (Windows)
Download: 
https://streamrsc.com/uploads/files/OBS-Studio-QT-TLS.zip

Place in:
OBS-Studio/bin/64bit/tls/qschannelbackend.dll

## Why Use It
OBS saves only PNG (large files).
This plugin allows compression and direct Discord upload.

Example:
4K PNG: 8–15MB
4K WEBP: 500KB–2MB

## Notes
- TLS required for webhook
- Format support depends on system

## Clang format fix

```bash
git update-index --chmod=+x .github/scripts/build-macos
git update-index --chmod=+x .github/scripts/build-ubuntu
git update-index --chmod=+x .github/scripts/package-macos
git update-index --chmod=+x .github/scripts/package-ubuntu

git update-index --chmod=+x .github/scripts/utils.zsh/check_macos
git update-index --chmod=+x .github/scripts/utils.zsh/check_ubuntu
git update-index --chmod=+x .github/scripts/utils.zsh/log_debug
git update-index --chmod=+x .github/scripts/utils.zsh/log_error
git update-index --chmod=+x .github/scripts/utils.zsh/log_group
git update-index --chmod=+x .github/scripts/utils.zsh/log_info
git update-index --chmod=+x .github/scripts/utils.zsh/log_output
git update-index --chmod=+x .github/scripts/utils.zsh/log_status
git update-index --chmod=+x .github/scripts/utils.zsh/log_warning
git update-index --chmod=+x .github/scripts/utils.zsh/mkcd
git update-index --chmod=+x .github/scripts/utils.zsh/setup_ubuntu
git update-index --chmod=+x .github/scripts/utils.zsh/set_loglevel

git update-index --chmod=+x build-aux/.run-format.zsh
git update-index --chmod=+x build-aux/run-clang-format
git update-index --chmod=+x build-aux/run-gersemi
git update-index --chmod=+x build-aux/run-swift-format

git update-index --chmod=+x build-aux/.functions/log_debug
git update-index --chmod=+x build-aux/.functions/log_error
git update-index --chmod=+x build-aux/.functions/log_group
git update-index --chmod=+x build-aux/.functions/log_info
git update-index --chmod=+x build-aux/.functions/log_output
git update-index --chmod=+x build-aux/.functions/log_status
git update-index --chmod=+x build-aux/.functions/log_warning
git update-index --chmod=+x build-aux/.functions/set_loglevel
```

```gitattributes
.gitattributes
*.sh text eol=lf
*.zsh text eol=lf
.github/scripts/** text eol=lf
.functions/* text eol=lf
run-clang-format text eol=lf
run-gersemi text eol=lf
run-swift-format text eol=lf
.run-format.zsh text eol=lf
```

```bash
git add --renormalize .
git commit -m "Normalize line endings for Unix scripts"
git push
```
