#!/bin/bash

LLDB_PATH=
CODELLDB_PATH=
LLDB_SERVER_PATH=
LLDB_PATH=

LD_LINUX_LIB_EL7=/opt/glibc-2.18/lib/ld-linux-x86-64.so.2

function find_lldb_path
{
    local START_TIME=$SECONDS
    local ELAPSED_TIME=0

    while true
    do
        if [ -d $HOME/.vscode-server/extensions/vadimcn.vscode-lldb-* ]; then
            pushd $HOME/.vscode-server/extensions/vadimcn.vscode-lldb-*
            LLDB_PATH="$PWD"
            popd

            CODELLDB_PATH=$LLDB_PATH/adapter/codelldb
            LLDB_SERVER_PATH=$LLDB_PATH/lldb/bin/lldb-server
            LLDB_PATH=$LLDB_PATH/lldb/bin/lldb

            echo "-- LLDB was found: $LLDB_PATH"
            true
            return
        fi

        ELAPSED_TIME=$(($SECONDS - $START_TIME))
        if [ $ELAPSED_TIME -gt 60 ]; then

            echo "vadimcn.vscode-lldb not found after 60 seconds"

            false
            return
        fi

        echo "Waitting for lldb"
        sleep .5
    done
}

# waitting for a file.
# $1 starting time
# $2 file path
function wait_for_file
{
    local ELAPSED_TIME=0

    while true
    do
        # Check file exist
        if [ -f "$2" ]; then

            # Check if there are no process writting the file
            if ! [ `lsof | grep "$2"` ]; then
                echo "-- Found: $2"
                true
                return
            fi
        fi

        ELAPSED_TIME=$(($SECONDS - $1))
        if [ $ELAPSED_TIME -gt 90 ]; then

            echo "$2 not found after 90 seconds"

            false
            return
        fi

        echo "Waitting for: $2"
        sleep .5
    done
}

function wait_for_binaries
{
    local START_TIME=$SECONDS

    wait_for_file $START_TIME "$CODELLDB_PATH"
    if [ $? -ne 0 ]; then
        false
        return
    fi

    wait_for_file $START_TIME "$LLDB_SERVER_PATH"
    if [ $? -ne 0 ]; then
        false
        return
    fi

    wait_for_file $START_TIME "$LLDB_PATH"
    if [ $? -ne 0 ]; then
        false
        return
    fi

    true
}

function registerLlvmToolset7
{
    echo "source scl_source enable llvm-toolset-7" > /etc/profile.d/llvm-toolset-7.sh
    chmod 644 /etc/profile.d/llvm-toolset-7.sh
    . /etc/profile.d/llvm-toolset-7.sh
}


function postCreateEl7
{
    if ! command -v patchelf &> /dev/null; then

        yum install -y patchelf
        if [ $? -ne 0 ]; then
            exit 1
        fi
    fi

    if ! command -v lsof &> /dev/null; then

        yum install -y lsof
        if [ $? -ne 0 ]; then
            exit 1
        fi
    fi

    ## laod llvm toolset 7 environment
    registerLlvmToolset7

    find_lldb_path;
    if [ $? -ne 0 ]; then
        exit 1
    fi

    wait_for_binaries;
    if [ $? -ne 0 ]; then
        exit 1
    fi

    patchelf --set-interpreter "$LD_LINUX_LIB_EL7" "$CODELLDB_PATH"
    if [ $? -ne 0 ]; then
        exit 1
    fi

    patchelf --set-interpreter "$LD_LINUX_LIB_EL7" "$LLDB_SERVER_PATH"
    if [ $? -ne 0 ]; then
        exit 1
    fi

    patchelf --set-interpreter "$LD_LINUX_LIB_EL7" "$LLDB_PATH"
    if [ $? -ne 0 ]; then
        exit 1
    fi

}

function checkIsCentos7
{
    if [ -f /etc/centos-release ]; then
        if [ "$(cat /etc/centos-release | tr -dc '0-9.'|cut -d \. -f1)" = "7" ]; then
            true
            return
        fi
    fi

    false
}


if checkIsCentos7; then
    postCreateEl7
fi

exit 0


