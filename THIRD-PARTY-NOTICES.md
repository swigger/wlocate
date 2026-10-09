# Third-party notices and references

WLocate is a native C++ implementation. It does not link to, launch, or require
Everything, SwiftList, the Everything service, or the .NET runtime.

The binary statically includes PCRE2 10.49, upstream commit
`6f9d7c1373262c541324a16a358785b33ef116cf`, and its pinned SLJIT dependency
`de0259c7aaf36aa40cba8014f3fad3edde9307f9`.
Their complete redistribution notices are in `licenses/PCRE2-LICENCE.md` and
`licenses/SLJIT-LICENSE.txt`. Sources: <https://github.com/PCRE2Project/pcre2>
and <https://github.com/zherczeg/sljit>.

Architectural references:

- SwiftList, MIT licensed: <https://github.com/SwiftList/SwiftList>, local
  reference commit `ef39db8c2aac8507130b789d5448f5305c5dfd27`. Its notice is
  included in `licenses/SwiftList-LICENSE.txt` as reference attribution.
- Everything search syntax: <https://www.voidtools.com/support/everything/searching/>.
- Everything service model: <https://www.voidtools.com/en-us/support/everything/everything_service/>.
- Everything IPC documentation: <https://www.voidtools.com/support/everything/sdk/ipc/>.
- Microsoft NTFS, USN, Windows services and Shell API documentation.

The user's installed Everything 1.4.1.1032 was inspected locally with IDA MCP.
The reference executable's SHA-256 is
`F191F756996A14A11E5445FA7103D302EFD510CF2FBF920E6C0C8ED51D512E36`.
Observed query dispatch routines at `0x1400af3b0` and `0x1400ae4b0` partition
file/folder work and combine results; the NTFS record-reading routine is at
`0x1400cfe80`. These observations informed architectural choices. WLocate's
source is independently implemented; the Everything executable, SDK binary
and decompiled code are not included in the release package. No affiliation
with or endorsement by Voidtools or SwiftList is implied.

WLocate's green magnifying-glass icon is original vector artwork inspired by
Everything's magnifying-glass silhouette. Editable SVG and the reproducible
multi-resolution ICO generator are included in the source tree.

The UTF-8 process manifest follows the user's `D:\work\dev3\utillib\utf8.manifest`
and Microsoft's guidance:
<https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page>.
