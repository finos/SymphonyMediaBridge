[![FINOS - Active](https://cdn.jsdelivr.net/gh/finos/contrib-toolbox@master/images/badge-active.svg)](https://community.finos.org/docs/governance/Software-Projects/stages/active)

# Symphony Media Bridge

The Symphony Media Bridge (SMB) is a media server application that handles audio, video and screen sharing media streams in an RTC conference system.

In RTC conferencing systems, when more than two participants are in a conference there is usually a media server component involved. Each participant in the conference will send their audio and video streams to the media server. The media server is then responsible for sending the correct media streams to each receiving participant.

Conferencing media servers are typically divided into two categories, multipoint conference units (MCU) and selective forwarding units (SFU).
MCUs decode, mix and transcode media streams delivering a single or a few output streams to each receiver. SFUs do not decode or transcode media streams from participants, but forwards a selection of the incoming media streams to each receiver in some intelligent way. Not all incoming streams are forwarded, but typically more than one stream to each receiver.

SMB is an SFU at its core, but has some hybrid MCU like solutions. Video streams are forwarded and not transcoded, but participants can request a mixed, transcoded audio stream instead of forwarded streams. SMB can also be run in a mode where multiple streams are forwarded, but the contents of each stream can vary between different participants in the conference. This allows for larger conferences than SFUs can typically handle.

Written as a high performance native application with efficient resource management, SMB is designed to scale efficiently as the number of conferences and participants grow.

## Documentation

-   [Wiki](https://github.com/finos/SymphonyMediaBridge/wiki)
-   [Usage example](https://github.com/finos/SymphonyMediaBridge/tree/master/examples)
-   [HTTP API documentation](https://github.com/finos/SymphonyMediaBridge/tree/master/doc/api/READMEapi.md)

## Installing

The Supported platforms for the release builds are

-   Ubuntu Server 20.04 LTS

The following additional dependencies have to be installed:

`apt-get install libc++-dev libc++abi-dev libsrtp2-1 libmicrohttpd12 libopus0`

### Alternative 1. Installing the .deb package

#### 1. Download the .deb file from a release

#### 2. Install

`dpkg -i finos-rtc-smb_<version>.deb`

#### 3. Start with the default empty config file

`smb /etc/finos-rtc-smb/config.json`

### Alternative 2. Download the .tar.gz file

#### 1. Download and extract the .tar.gz file from a release

#### 2. Add realtime priority capabilities to the smb binary

`setcap CAP_SYS_NICE+ep smb`

#### 3. Start with the default empty config file

`./smb config.json`

## Development setup

The Symphony Media Bridge is a cmake based project that can be built and run for development purposes on Linux as well as MacOSX. Here are instruction on what dependencies are needed to build and run locally. SMB currently only supports compiling with clang/llvm and linking with libc++ on both Linux and MacOSX.

### Building for Ubuntu Linux 20.04

#### 1. Install the required dependencies

`apt-get install cmake llvm lld clang lldb python3 git libc++-dev libc++abi-dev libssl-dev libsrtp2-dev libmicrohttpd-dev libopus-dev libunwind-dev`

#### 2. Set Clang as compiler

`export CC=clang && export CXX=clang++`

#### 3. Generate the Makefile

`cmake -DCMAKE_BUILD_TYPE=Debug .`

### Building for MacOSX

#### 1. Install the required dependencies using brew

`brew install cmake srtp openssl@1.1 libmicrohttpd opus`

#### 2.1 Generate the Makefile

`cmake -DCMAKE_BUILD_TYPE=Debug -G "Unix Makefiles" .`

#### 2.2 Alternatively generate Xcode project files

`cmake -G "Xcode" .`

### Running the application

`./smb config.json`

### Running the local test suite

```
./UnitTest
./UnitTest2
```

UnitTest2 contains integration tests that run in emulated network environment and emulated time.
It is very close to end to end tests without adding external RTC agents.

## Contributing

1. Fork it (<https://github.com/finos/SymphonyMediaBridge/fork>)
2. Create your feature branch (`git checkout -b feature/fooBar`)
3. Read our [contribution guidelines](.github/CONTRIBUTING.md) and [Community Code of Conduct](https://www.finos.org/code-of-conduct)
4. Commit your changes (`git commit -am 'Add some fooBar'`)
5. Push to the branch (`git push origin feature/fooBar`)
6. Create a new Pull Request

_NOTE:_ Commits and pull requests to FINOS repositories will only be accepted from those contributors with an active, executed Individual Contributor License Agreement (ICLA) with FINOS OR who are covered under an existing and active Corporate Contribution License Agreement (CCLA) executed with FINOS. Commits from individuals not covered under an ICLA or CCLA will be flagged and blocked by the FINOS Clabot tool (or [EasyCLA](https://github.com/finos/community/blob/master/governance/Software-Projects/EasyCLA.md)). Please note that some CCLAs require individuals/employees to be explicitly named on the CCLA.

_Need an ICLA? Unsure if you are covered under an existing CCLA? Email [help@finos.org](mailto:help@finos.org)_

## License

Copyright 2021 Symphony LLC

Distributed under the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0).

SPDX-License-Identifier: [Apache-2.0](https://spdx.org/licenses/Apache-2.0)
