#!/bin/bash

set -e

cd "$(dirname "${0}")"

if [[ -z "${1}" ]]; then
    echo "Usage: ${0} [release|debug]"
    exit 1
fi

case "${1}" in
debug)
    buildtype=debug
    ;;
debug-nonboot)
    buildtype=debug-nonboot
    ;;
release)
    buildtype=release
    ;;
ci)
    buildtype=ci
    ;;
*)
    echo "Invalid build type ${1}"
    exit 1
esac

source ../compile/env.sh
setup_toolchain

if [ ! -d jsoncpp ]; then
    git clone https://github.com/open-source-parsers/jsoncpp.git
fi

pushd jsoncpp/
    git fetch
    git checkout 60f778b9fcd1c59908293a6145dffd369d4f6daf
    python2 amalgamate.py
popd

export LDFLAGS='-pie'

if [[ "${buildtype}" == debug-nonboot ]]; then
    export CXXFLAGS='-g -O0'
else
    export CXXFLAGS='-fstack-protector-all -D_FORTIFY_SOURCE=2'
    export LDFLAGS+=' -Wl,-z,noexecstack -Wl,-z,now -Wl,-z,relro'
fi

${CXX} ${CXXFLAGS} ${LDFLAGS} \
    -std=c++11 \
    -DVERSION=\"$(get_conf builder version)\" \
    syncdaemon.cpp \
    common.cpp \
    configfile.cpp \
    jsoncpp/dist/jsoncpp.cpp \
    -Ijsoncpp/dist \
    -llog \
    -osyncdaemon

if [[ "${buildtype}" != debug-nonboot ]]; then
    ${STRIP} syncdaemon
fi

#if [[ "${buildtype}" == release ]] && which upx >/dev/null; then
#    upx --lzma syncdaemon
#fi

# /data/local/Inst/bin/valgrind -v --leak-check=full --show-leak-kinds=all --track-origins=yes ./a.out