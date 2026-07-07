#!/bin/sh

DORA_HOME="${DORA_HOME:-/root/dora-riscv64}"
export LD_LIBRARY_PATH="$DORA_HOME/lib:${LD_LIBRARY_PATH:-}"
exec ./tennis ./yolo_model.cvimodel 0