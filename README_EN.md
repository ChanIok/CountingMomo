[简体中文](README.md) | [English](README_EN.md)

# CountingMomo

Clothing collection data export tool for *Infinity Nikki*. Scan the in-game compendium with one click to quickly export your wardrobe collection progress.

## About

- **Fully Automated**: Automatically cycles through all 27 categories with seamless scrolling, strictly aligned with the in-game compendium.

- **Non-Invasive & Native**: Pure screen capture and visual detection with no reading or writing to game memory; zero external dependencies, portable and ready to run.

## Getting Started

> **System Requirements**: Windows 10 (version 2004 or later).

1. In the game, open **Pear-Pal → Compendium → Search**.
2. Launch the application and click **Start scan** on the main window (press **F8** to stop at any time).
3. Once scanning completes, click **Copy result** (can be pasted directly into [NUAN5.PRO](https://nuan5.pro/data)).

## Output Format

The exported data maps category keys to `[0, 1]` arrays (`1` for owned, `0` for unowned), strictly ordered as they appear in the in-game compendium:

```json
{
  "hair": [0, 1, 0, 1],
  "dresses": [1, 0, 0, 1]
}
```

## Build & Development

For architecture and development guidelines, see [AGENTS.md](AGENTS.md).

```powershell
pnpm run build --release     # Build Release
pnpm run package             # Package portable zip
```
