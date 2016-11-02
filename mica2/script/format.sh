#!/bin/bash

CLANG_FORMAT=clang-format-3.6
SCRIPT_DIR=$( dirname "$0" )

${CLANG_FORMAT} -i "$SCRIPT_DIR/"../src/mica/{.,alloc,pool,processor,table,table/ltable_impl,table/ptrie_impl,test,util}/*.{h,cc}

