# FAH Saver

![FAH Saver displaying a protein model on the macOS lock screen](docs/images/fah-saver.png)

Display the active Folding@home work unit protein model as a screensaver, from a remote machine or local.

This project is not affiliated with Folding@home.

## Install

### macOS (Apple silicon only, macOS 11.0 Big Sur or above)

Install with Homebrew:

```sh
brew install --cask le0-VV/fah-saver/fah-saver
```

For manual installation, open the [latest release](https://github.com/le0-VV/FAH-saver/releases/latest), download the macOS ZIP, extract it and open `FAHSaver.saver`.

### Windows (x64 only, Windows 10 version 1803 or above)

Download and run [FAH Saver Setup.exe](https://github.com/le0-VV/FAH-saver/releases/latest/download/FAHSaver-Setup.exe).

### Connect to Folding@home

When FAH runs on the same computer, set the source to `ws://127.0.0.1:7396/api/websocket`. For another folding computer, use its private LAN or Tailscale address and follow the [direct-access instructions](docs/direct-access.md).

## TODO

- [ ] Folding@home account connection.
- [ ] Upscaling integration.
- [ ] Further renderer optimisation.
- [ ] Linux release.

## Licence

FAH Saver's original code is licensed under **GPL-3.0-or-later**. See [LICENSE](LICENSE), [COPYRIGHT](COPYRIGHT) and the [third-party notices](THIRD-PARTY-NOTICES.md). Third-party components retain their own licences.

For build instructions, technical details, performance measurements and known limitations, see the [documentation in `docs/`](docs/).
