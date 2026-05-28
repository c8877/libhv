#!/bin/bash

. /opt/mp135/mp135env.sh

cmake -Bbuild .
cd build
make -j32
tree -h bin
