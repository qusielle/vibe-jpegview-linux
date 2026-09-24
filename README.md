[![Linux CI](https://github.com/qusielle/vibe-jpegview-linux/actions/workflows/linux-build.yml/badge.svg)](https://github.com/qusielle/vibe-jpegview-linux/actions/workflows/linux-build.yml)
[![Ubuntu builds](https://img.shields.io/badge/Ubuntu%20builds-20.04%20%7C%2022.04%20%7C%2024.04%20%7C%2026.04-E95420?logo=ubuntu&logoColor=white)](https://github.com/qusielle/vibe-jpegview-linux/releases)
[![Latest release](https://img.shields.io/github/v/release/qusielle/vibe-jpegview-linux?include_prereleases&label=latest%20release)](https://github.com/qusielle/vibe-jpegview-linux/releases)
[![Downloads](https://img.shields.io/github/downloads/qusielle/vibe-jpegview-linux/total?label=downloads)](https://github.com/qusielle/vibe-jpegview-linux/releases)
[![License: GPL-2.0](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](LICENSE.txt)

# JPEGView for Linux

A fast native Linux image viewer and editor, and a Linux-focused fork of the
[sylikc/jpegview GitHub project](https://github.com/sylikc/jpegview), itself based on JPEGView by
David Kleiner. The Linux frontend is based on the 1.3.46 codebase.

This repository now focuses on maintaining the Linux delivery in [`linux/`](linux/). The original
Windows implementation remains under [`src/`](src/) as the upstream reference. When a Windows fix
or feature can be sensibly supported on Linux, the goal is to port or backport it; Linux-specific
work stays native to the Linux frontend. The project aims for useful cross-platform behavior, not
automatic one-to-one Windows feature parity.

The archived [Windows README](src/README.md), [build notes](src/COMPILING.txt), installation
guides ([English](src/HowToInstall.txt), [Russian](src/HowToInstall_ru.txt)), and
[changelog](src/CHANGELOG.txt) are kept with the Windows source.

## Highlights

- Browse large images and folders with high-quality scaling, zoom and pan, responsive neighbor
  prefetch, and a configurable image cache.
- Navigate folders with flexible ordering, recursive and sibling-folder modes, an in-app browser
  with live filtering and previews, and a neighboring-image thumbnail strip.
- Adjust picture levels, resize and transform images, batch rename or copy, and save processed
  results in a wide range of formats.
- View animated images and use slideshow/movie playback.
- Integrate with the Linux desktop through AppImage and `.deb` packages, system applications,
  clipboard tools, and user-local default-viewer registration.

## Downloads

Get the current Linux release from [GitHub Releases](https://github.com/qusielle/vibe-jpegview-linux/releases).
Releases include x86_64 AppImages and standalone executables built for Ubuntu 20.04, 22.04, 24.04,
and 26.04, with SHA-256 checksums. Ubuntu 24.04 and 26.04 releases also provide `.deb` packages.
The Ubuntu 20.04 AppImage is the broadest-compatibility choice; builds made on newer Ubuntu releases
may require a newer glibc.

To install a `.deb`, download the package for your Ubuntu release and run its matching command.
For Ubuntu 24.04:

```sh
sudo apt install ./jpegview-linux_*_ubuntu24_amd64.deb
```

For Ubuntu 26.04:

```sh
sudo apt install ./jpegview-linux_*_ubuntu26_amd64.deb
```

## Build from source

Install the Linux build dependencies listed in [`linux/README.md`](linux/README.md), then build and
launch the viewer:

```sh
make -C linux
linux/build/jpegview-linux /path/to/image-or-folder
```

The project also provides Ubuntu 20.04, 22.04, 24.04, and 26.04 Docker build environments. The Linux
README has full build and packaging instructions, supported formats, keyboard controls, test targets,
and known Windows-parity gaps.

## Project direction and contributions

Linux is the primary maintenance target for this repository. Windows remains valuable as the source
of existing behavior and future improvements, but platform-specific UI and system integrations are
implemented natively. Linux-only features and outstanding parity gaps are tracked in the
[Linux feature inventory](linux/README.md#linux-branch-changes-in-order-of-importance).

Bug reports and contributions are especially useful when they include the Ubuntu version, package
type, image format, and concise reproduction steps. Run the Linux checks with:

```sh
make -C linux check
```

## Upstream and license

The original project began on [SourceForge](https://sourceforge.net/projects/jpegview/) and includes
contributions from the community, including additional codec support. This fork retains the original
source history and credits. It is distributed under the
[GNU General Public License v2](LICENSE.txt).
