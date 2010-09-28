#!/usr/bin/env bash
# 
# This scripts generates the dependences for SCIP 
#

EXAMPLES=(Coloring Binpacking Eventhdlr LOP MIPSolver Queens SamplePricer SamplePricer_C TSP VRP)
OPTS=(opt dbg)

for EXAMPLE in ${EXAMPLES[@]}
do
    echo ===== $EXAMPLE =====
    echo
    cd $EXAMPLE
    for OPT in ${OPTS[@]}
    do
	make OPT=$OPT ZIMPL=false LPS=none depend
    done
    cd ..
    echo
done
