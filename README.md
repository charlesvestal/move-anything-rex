# REX Player

A [Move Everything](https://github.com/charlesvestal/move-everything) module that plays Propellerhead ReCycle loop files on the Ableton Move. Each slice in the loop is mapped to a MIDI note for playback.

Supports mono and stereo `.rx2` files. Older `.rex` and `.rcy` formats are not supported.

## Installation

Install via the Module Store in Move Everything, or download the latest release from the [releases page](https://github.com/charlesvestal/move-anything-rex/releases) and copy the `rex/` folder to your Move:

```
/data/UserData/move-anything/modules/sound_generators/rex/
```

## Adding Loops

Upload `.rx2` files through the Move Everything file manager, or copy them directly to the module's `loops/` directory on the device:

```
/data/UserData/move-anything/modules/sound_generators/rex/loops/
```

## Format Attribution

The REX2 file format (`.rx2`, `.rex`, `.rcy`) is a proprietary format created by Propellerhead Software (now [Reason Studios](https://www.reasonstudios.com/)). ReCycle is a trademark of Reason Studios AB.

This module is an independent, clean-room implementation. It does not contain any Reason Studios code. The file format and DWOP codec were reverse-engineered for interoperability by analyzing the binary structure of REX2 files. See [REX2_FORMAT.md](REX2_FORMAT.md) for the full specification and methodology.

## License

[MIT](LICENSE)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
