#!/bin/bash

insmod ./nvmev_on.ko memmap_start=256G memmap_size=64G cpus=131,132,135,136 ${NVMEV_EXTRA_MODULE_PARAMS:-}
