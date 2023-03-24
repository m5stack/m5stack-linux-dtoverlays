#!/bin/bash
### Copy the lvgl related library from example Basic to current directory


help() {
    sed -rn 's/^### ?//;T;p' "$0"
}


if [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    help
    exit 1
fi


cp -r ../Basic/lv_porting .
cp -r ../Basic/lvgl .
