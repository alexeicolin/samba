#!/bin/sh

. "${TEST_SCRIPTS_DIR}/unit.sh"

define_test "3 nodes, add a 3 nodes"

setup_nodes <<EOF
192.168.20.41
192.168.20.42
192.168.20.43
192.168.20.44
192.168.20.45
192.168.20.46
EOF

setup_ctdbd <<EOF
NODEMAP
0       192.168.20.41   0x0     CURRENT RECMASTER
1       192.168.20.42   0x0
2       192.168.20.43   0x0
EOF

required_result 0 <<EOF
Node 3 is NEW
Node 4 is NEW
Node 5 is NEW
EOF

simple_test
