#!/bin/bash

DIR=$(dirname "$0")

DIFF=`pushd DBx1000 > /dev/null; git diff; popd > /dev/null`
echo "$DIFF" > "$DIR/../src/diff_DBx1000.patch"

