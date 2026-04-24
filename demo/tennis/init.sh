#!/bin/sh

export LD_LIBRARY_PATH=/root/AKA-00/lib:$LD_LIBRARY_PATH
exec ./tennis ./tennis_cv181x_bf16.cvimodel 0