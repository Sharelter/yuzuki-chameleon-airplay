# Third-party components

| Component | Source and licensing |
| --- | --- |
| `stb_image.h`, `stb_truetype.h` | [stb](https://github.com/nothings/stb). The original license alternatives and copyright notices are included at the end of each header. |
| Shairport Sync 3.3.8 | [Upstream source](https://github.com/mikebrady/shairport-sync/tree/3.3.8). The upstream source and compiled binary are external dependencies. See upstream `COPYING` and the notices in its source files. |
| TinySVCmDNS compatibility patch | Targets Shairport Sync's bundled TinySVCmDNS. See the [original TinySVCmDNS notice](tinysvcmdns-LICENSE.txt). |
| Noto Sans CJK | Optional runtime font from [Noto CJK](https://github.com/notofonts/noto-cjk/tree/main/Sans), distributed upstream under the [SIL Open Font License](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE). Provide your own font when installing the renderer. |

The root MIT license covers this project's original files. Firmware, disk
images, modified fonts, and prebuilt dependencies are kept out of the release.
