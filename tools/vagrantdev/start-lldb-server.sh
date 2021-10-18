export LD_LIBRARY_PATH=/home/vagrant/rtc-smb/libs
PATH=$PATH:/opt/rh/llvm-toolset-7/root/usr/bin/

/opt/rh/llvm-toolset-7/root/usr/bin/lldb-server platform --server --listen *:2000 > /dev/null 2>&1

