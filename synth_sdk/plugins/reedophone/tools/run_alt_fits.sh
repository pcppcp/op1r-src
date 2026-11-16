#!/bin/sh
# fit the two competing models one after the other (each uses ~18 processes)
cd "$(dirname "$0")"
python3 fit.py spec_stk1.json --gens 250 --pop 20 --tag stk1 > fits_stk1.log 2>&1
python3 fit.py spec_fm1.json --gens 250 --pop 20 --tag fm1 > fits_fm1.log 2>&1
