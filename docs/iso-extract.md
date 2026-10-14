# ISO Extraction Record

## Image

`Urbz, The - Sims in the City (USA).iso`, 1,594,687,488 bytes, located in `D:/Urbz`.

## Tool

`D:/Urbz/tools/xiso_extract.py` — a stdlib-only XDVDFS reader with subcommands `info`, `list`, and `extract`.

## Reproduce

```
python tools/xiso_extract.py info "Urbz, The - Sims in the City (USA).iso"
python tools/xiso_extract.py list "Urbz, The - Sims in the City (USA).iso"
python tools/xiso_extract.py extract "Urbz, The - Sims in the City (USA).iso" assets
```

## Volume geometry

- Partition base: `0x00000000` — the image is a bare game partition, not a full dual-layer dump.
- Root directory sector: 264 (offset `0x84000`).
- Root table size: 120 bytes.
- Volume FILETIME: `0x01C4B016DBFD82D0`.
- Files: 44.
- File data: 1,594,049,853 bytes.

The `extract` run wrote 44 files, 1,591,014,717 bytes, into `D:/Urbz/assets`.

## Contents

- `assets/default.xbe`: 3,035,136 bytes, magic `XBEH`, SHA-1 `5712b6a3e2eaaacfcae228cf6154ade499a0033c`.

| Path | Size (bytes) | Notes |
|---|---|---|
| `DATA/` | — | 23 entries: `.arc` archives plus `index*.ind`, `audiostr.xsb`/`audiostr.xwb` XACT sound bank/wave bank |
| `movies/` | — | 19 `.xmv` files: USA and PAL variants of the EA logo, attract, intro and ending videos |
| `default.xbe` | 3,035,136 | Executable, magic `XBEH`, SHA-1 `5712b6a3e2eaaacfcae228cf6154ade499a0033c` |
| `DEFAULT.NGH` | 1,042,432 | |
| `urbz.bin` | 2,960 | |
| `DATA/samples.arc` | 434,966,567 | Largest archive |
| `DATA/movies.arc` | 322,455,552 | |
| `DATA/datasets.arc` | 187,371,859 | |
| `DATA/audiostr.xwb` | 141,117,440 | XACT wave bank |
| `DATA/textures.arc` | 136,790,668 | |
| `DATA/models.arc` | 64,546,188 | |

## Notes

The extracted tree in `assets/` and the ISO itself are the user's own game data. They are never committed and live only in `assets/`. The SDK can also read the ISO directly.
