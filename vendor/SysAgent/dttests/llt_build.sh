#!/bin/bash

CUR_PATH=$(dirname $(readlink -f $0))
ROOT_PATH=${CUR_PATH}/..

clean_test() {
    cd ${ROOT_PATH}/dttests
    hdt clean
}

download_hdtcpp() {
    echo ${ROOT_PATH}mockcpp
    echo "download hdtcpp start"
    command -v hdt >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "had install hdt"
    else
        echo "install hdt..."
        pip3 install hw-hdt-cli -i https://cmc.centralrepo.rnd.huawei.com/artifactory/product_pypi/simple --extra-index-url https://cmc.centralrepo.rnd.huawei.com/pypi/simple --trusted-host cmc.centralrepo.rnd.huawei.com
    fi

    cd ${ROOT_PATH}/dttests
    echo "-------------------0"
    hdt comp -i --os=linux --compiler=clang15.0.4 --isa=arm64
    echo "-------------------00"
}

function test() {
    cd ${ROOT_PATH}/dttests
    hdt build -c on   # 编译、运行测试代码，并生成单测报告；-c 是覆盖率开关，默认是关闭的
}

function run_test() {

    cd ${ROOT_PATH}/dttests
    echo "-------------------3"
#    ln -s /usr/lib64/libpython3.so ${ROOT_PATH}/platform/mindIE/lib/libpython3.10.so.1.0
#    export ASAN_OPTIONS=detect_odr_violation=0
#    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${ROOT_PATH}/platform/mindIE/lib/grpc
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${ROOT_PATH}/../platform/lib
    echo ${LD_LIBRARY_PATH}
    hdt run -c on -s on  # 编译测试代码
    echo "-------------------4"
}

function report_test() {
    cd ${ROOT_PATH}/dttests
    rm -rf ${ROOT_PATH}/dttests/gcov
    OUTPUT=$(hdt comp | grep lcov)
    LCOV_PATH=$(echo "$OUTPUT" | awk '{print $NF}')
    echo "lcov path is ${LCOV_PATH}/bin/lcov"
    wget https://s3-kp-szv-gen.hics.huawei.com/wecodetest-package-suzhou/gcov/7_3_arm/gcov -O gcov --no-check
    chmod +x gcov
    mkdir -p ./build/lcov_report
    ${LCOV_PATH}/bin/lcov -d ./ -c -o ./build/lcov_report/lcov_report.info -q --rc lcov_branch_coverage=1 --gcov-tool ${ROOT_PATH}/dttests/gcov
    ${LCOV_PATH}/bin/lcov -r ./build/lcov_report/lcov_report.info */platform/* */usr/include/* */extern/* */dttests/* */gtest/include/* -o ./build/lcov_report/lcov_report.info
    ${LCOV_PATH}/bin/genhtml ./build/lcov_report/lcov_report.info -o ./build/lcov_report/
    cp ${ROOT_PATH}/dttests/build/lcov_report/lcov_report.info ${ROOT_PATH}/dttests/build/lcov_report/result.info
}

function main() {
    clean_test
    download_hdtcpp
    test
    run_test
    report_test
}

main
