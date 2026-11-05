# melon-firmwares
A repository hosting all melon firmwares using the added WS2812B.
To flash your melon module the easiest way possible please use the deicated [web flasher](https://wgdmodular.de/melon/list/).
For more info please refer to the much better documented [MOD2 repository](https://github.com/modulove/MOD2).

## Manual

The [user manual](https://github.com/wgd-modular/melon-firmwares/releases/latest/download/melon-manual.pdf) (PDF) describes the module, how to build and flash it, and every firmware with all of its controls. It is attached to every release, and the CI run of each commit has a build of it as an artifact. To build it yourself run `pixi run manual`. It only needs `typst`, `python` and `git`, which [pixi](https://pixi.sh) installs, and takes the history of each firmware's folder from `git log`, so clone the repository with its full history. The bill of materials and the build guide come from the [melon's page](https://wgdmodular.de/module/bom/77/) on wgdmodular.de.

## Contributing

Contributions of any kind, especially firmwares that make use of the WS2812B are very welcome. Please open a PR!

Each firmware lives in its own folder with the same name as its `.ino` file, together with a `README.md` about its controls. The manual is generated from these READMEs: the first `#` heading is the firmware's name, the text up to the first section heading its introduction, and every heading of the shallowest level below it starts a section. A section called "Controls" goes on a panel-like plaque up to its first sub-heading, so put the table of the controls there, and an "Author" section ends up in the chapter's credit line. Sections about building or flashing a single firmware are left out, as the manual has a chapter on both. Who worked on a firmware, when it was added and last changed come from the git history of its folder, so credit co-authors with a `Co-authored-by` trailer.
