# Symphony Media Bridge

The Symphony Media Bridge (SMB) is a media server application that handles audio, video and screen sharing media streams in an RTC conference system.

In RTC conferencing systems, when more than two participants are in a conference there is usually a media server component involved. Each participant in the conference will send their audio and video streams to the media server. The media server is then responsible for sending the correct media streams to each receiving participant.

Conferencing media servers are typically divided into two categories, multipoint conference units (MCU) and selective forwarding units (SFU).
 MCUs decode, mix and transcode media streams delivering a single or a few output streams to each receiver. SFUs do not decode or transcode media streams from participants, but forwards a selection of the incoming media streams to each receiver in some intelligent way. Not all incoming streams are forwarded, but typically more than one stream to each receiver.

SMB is an SFU at its core, but has some hybrid MCU like solutions. Video streams are forwarded and not transcoded, but participants can request a mixed, transcoded audio stream instead of forwarded streams. SMB can also be run in a mode where multiple streams are forwarded, but the contents of each stream can vary between different participants in the conference. This allows for larger conferences than SFUs can typically handle.

Written as a high performance native application with efficient resource management, SMB is designed to scale efficiently as the number of conferences and participants grow.

## Installing

Supported platforms for the release builds are
- Ubuntu Server 20.04
- Centos 7
- RedHat Enterprise Linux 7
- RedHat Enterprise Linux 8

For Ubuntu Server 20.04, the following additional dependencies need to be installed:

```apt-get install libc++-dev libc++abi-dev```

### 1. Download and extract the zip file of a release

### 2. Navigate to rtc-smb/\<target os\>/smb

### 3. Add RT priority capabilities to the smb binary

```setcap CAP_SYS_NICE+ep smb```

### 3. Create a config file

```echo "{}" > config.json"```

### 4. Start

Start manually by executing the smb binary

```./smb config.json```

## Usage example

## Development setup

The Symphony Media Bridge is a cmake based project that can be built and run for development purposes on Linux as well as MacOSX. Here are instruction on what dependencies are needed to build and run locally. SMB currently only supports compiling with clang/llvm and linking with libc++ on both Linux and MacOSX.

### Building for Ubuntu Linux 21.10

#### 1. Install the required dependencies

```apt-get install cmake llvm clang lldb libc++-dev libc++abi-dev libssl-dev libsrtp2-dev libmicrohttpd-dev libopus-dev libunwind-13-dev```

#### 2. Set Clang as compiler

``` export CC=clang && export CXX=clang++```

#### 3. Generate the Makefile

```cmake -DCMAKE_BUILD_TYPE=Debug -G "Unix Makefiles" .```


### Building for MacOSX

#### 1. Install the required dependencies using brew

```brew install cmake srtp openssl@1.1 libmicrohttpd opus```

#### 2.1 Generate the Makefile

```cmake -DCMAKE_BUILD_TYPE=Debug -G "Unix Makefiles" .```

#### 2.2 Alternatively generate Xcode project files

```cmake -G "Xcode" .```

### Running

``` ./smb config.json ```
