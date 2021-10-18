# OSX develop and test

## Homebrew packages

- cmake
- srtp
- openssl@1.1
- libmicrohttpd
- opus

## Generate workspace
Run either
```
./generate_codelite_project.sh
./generate_xcode_project.sh
```
If you want to switch you need to remove CMakeCache.txt, CMakeFiles and the googletest folders first.

To build locally on MacOSX run
./generate_localmake.sh Debug
make -j8

## Running unit tests
Some tests require an audio file "jpsample.raw" (2 x 48kHz 16-bit). 
You can download and run with
```
gsutil cp gs://rtc-smb-test/jpsample.raw .
./UnitTest
```

# OSX cross dev CentOS 7
You can develop, compile and test in CentOS7 with vscode on Mac. Compiling is done in docker and test is run in vagrant.
## Requirements
* docker installed on local computer.
* setup gcloud docker registry. https://cloud.google.com/container-registry/docs/pushing-and-pulling/
* docker image buildsmb:latest available in gcr. If not you should build and push one in Jenkins.
* vagrant installed locally
* vagrant plugin install vagrant-hostsupdater
* vagrant plugin install vagrant-vbguest
* In VSCODE install CodeLLDB extension v1.5.3.

## Build debug build in docker
Vscode has built in support for opening project inside a container. SMB project files have support for this. See folder .devcontainer.
This means you can click the left corner and select reopen in container and your environment switches to use the container only. 
Press shift-F6 and select flavour to build. You Debug as usual with F5.
The GTest extension also works fine insode docker container but you have to install the extensions you need once again 
while in the container environment.

## Debug in vagrant
Start vagrant with following command sequence. It will place vagrantdev entry in /etc/hosts so lldb can connect.
```
cd tools/vagrantdev
vagrant up
sudo ./vagrant-extract-ip.sh
vagrant ssh
rtc-smb/tools/vagrantdev/start-lldb-server.sh
```

In vscode enter debug view and select `Remote LLDB` or `Remote UnitTest`. Then press `F5`.

You can bind `SHIFT-F6` in vscode to build and make centos-build task the default build task.

## Load CentOS core dump
* use "ulimit -c unlimited" to enable core dumps. Edit /etc/security/limits.conf to set permanently.
* set core_pattern if needed with sudo echo "/tmp/core.%e.%p.%h.%t" > /proc/sys/kernel/core_pattern
* Copy the core.dump file, the executable, and the libs files into a local folder.
* Edit the launch.json `lldb coredump` setting to point to your executable, dump file and libs folder.
* Press `F5`
* To provoke a core dump on a stalled SMB, use kill -4 <pid> to inject an illegal instruction crash.
* Once you loaded the dump you can find out the expected source paths with  source info -a $pc
and write those into launch.json

## Configuring CentOS
SMB tries to run engine thread at real time prio. But this requires you to allow this from linux.
* sudo setcap CAP_SYS_NICE+ep smb
will enable this for the smb file. If you replace the file, you will have to set the cap again.

# Build and smoke test with release scripts
## Build in docker
Create an smb.zip file with the following command.
* ./docker/dockerbuild.sh <flavour> with `dbg` or `rel`.

## Vagrant setup smoke test
There is a Vagrant setup for running the Centos build locally. To use:

1. first make a local build according to above
2. from tools/vagrant run `vagrant up --provision`
3. `vagrant ssh`
4. start the smb using ./startsmb.sh

