# build
A simple script that will download and build the emulatorjs core files

## One-click DOSBox Pure Docker build

This fork builds the persistent Emscripten DOSBox Pure core and keeps the
completed EmulatorJS packages in `output/`:

```bash
./build-dosbox-pure.sh
```

Docker Compose installs the pinned Emscripten SDK in the image and reuses the
host `compile/` directory. The build script, core settings and patches are mounted
from the checkout, so reusing the builder image still uses current build logic.
The distributable files are written to the host
`output` directory. Prebuilt copies are committed there as well, including
`dosbox_pure.zip` and both threaded `.data` variants.

## Azahar Docker build

Create the builder image once, or rebuild it after changing the SDK or Dockerfile:

```bash
docker compose -f compose.dosbox-pure.yml build builder
```

Build Azahar with bounded parallelism and reuse its CMake build directory:

```bash
docker compose -f compose.dosbox-pure.yml run --rm --no-deps \
  -e BUILD_JOBS=2 -e INCREMENTAL_CMAKE=1 builder \
  bash -lc 'source /opt/emsdk/emsdk_env.sh && ./build.sh --core=azahar'
```

Omit `INCREMENTAL_CMAKE=1` to recreate the CMake build directory. The script
applies the Azahar patch stack on fresh sources and reverses overlapping layers
before reapplying it on incremental builds. Compose preserves the existing source
revision by default through `SKIP_SOURCE_UPDATE=1`.

The outputs are `output/azahar-thread-wasm.data` and `output/reports/azahar.json`.
When the RomM checkout is next to this repository, publish both files together:

```bash
../romm/scripts/update-custom-azahar-core.sh
```

Rebuild the RomM image afterward to include the updated core. See
[Azahar performance notes](AZAHAR_PERFORMANCE.md) for shader checks and measurements.

This script will download and build most of the available retroarch cores.

> **Warning**: Some cores do not compile on ARM based systems (such as M series MacBooks and Raspberry Pi). Use only amd64 based systems to compile.

# Set up your build environment

## VSCode and Dev Containers
This repo contains a devcontainer configuration.

Using docker and VSCode, the repo can be started in a container, where all dependencies are installed for you.

## Local
This guide assumes you're using a Debian or Ubuntu type system.

* Run ``sudo apt update && sudo apt install jq wget curl gpg p7zip-full binutils-mips-linux-gnu build-essential pkgconf python3 git zip libsdl2-dev``

* Run the enclosed ``build_env.sh`` script to configure the build environment. You may need to use sudo to run this script to install system components.

# Compiling
Execute the command ``source ./.emsdk/emsdk_env.sh && bash build.sh`` in the repo root to begin compiling.

Compilation will be performed in ``./compile``, and the completed assets will be copied to ``./output``, with logs in ``./output/logs``.

# Adding new cores
To add a new core, add a stanza like the below:
```json
{
    "name": "mame2003",
    "extensions": [ "zip" ],
    "makeoptions": {
        "buildpath": "./",
        "makescript": "Makefile",
        "arguments": []
    },
    "options": {
        "file": "MAME 2003 (0.78)/MAME 2003 (0.78).opt",
        "settings": {
            "mame2003_skip_disclaimer": "enabled",
            "mame2003_skip_warnings": "enabled"
        },
        "defaultWebGL2": false,
        "supportsMouse": false
    },
    "license": "LICENSE.md",
    "repo": "https://github.com/EmulatorJS/mame2003-libretro",
    "branch": "main"
}
```

| Attribute | Definition |
| --------- | ---------- |
| ``name``      | The name of the core. This value should be the name of the compiled core .data file. |
| ``extensions`` | An array of file extensions used by the core |
| ``license``   | The path to the repo project license file. This path is relative to the root of the repo. |
| ``repo``      | A link to the project repository |
| ``branch``    | The git branch to switch to when building |
| ``makeoptions`` | Settings and options for building the core (see makeoptions table below) |
| ``options``   | Options to be set by the emulator (see options table below) |

### makeoptions
| Attribute | Definition |
| --------- | ---------- |
| ``buildpath`` | The path within the repo to the make script |
| ``makescript`` | The name of the make script |
| ``arguments`` | An array of command line options to pass to the make script |

### options
| Attribute | Definition |
| --------- | ---------- |
| ``file``      | The relative path and file name for the core options file |
| ``settings``  | A hash table of attributes and their values to write to the core options file |
| ``defaultWebGL2`` | A boolean value of if WebGL2 should be defaulted to enabled |
| ``supportsMouse`` | A boolean value defining if the core supports a mouse |
