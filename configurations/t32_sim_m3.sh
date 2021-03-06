#!/bin/bash
## A cmake configuration call for a FailT32 Simulator variant

cmake $(dirname $0)/.. -DBUILD_BOCHS:BOOL=OFF -DBUILD_X86:BOOL=OFF -DARCH_TOOL_PREFIX:STRING=arm-none-eabi- -DBUILD_T32:BOOL=ON -DBUILD_ARM:BOOL=ON  -DCONFIG_BOCHS_NO_ABORT:BOOL=OFF -DCONFIG_EVENT_BREAKPOINTS:BOOL=ON -DCONFIG_EVENT_BREAKPOINTS_RANGE:BOOL=ON -DCONFIG_EVENT_MEMREAD:BOOL=ON -DCONFIG_EVENT_MEMWRITE:BOOL=ON -DEXPERIMENTS_ACTIVATED:STRING=vezs-example -DT32_ARCHITECTURE:STRING=armm3 -DT32_CPUNAME:STRING=STM32F103RG -DT32_SIMULATOR:BOOL=ON
echo "-------------"
echo "Test me with a recent CiAO version: Application armm3_test"
echo "Don't forget to: export FAIL_ELF_PATH=$CIAOBASE/kconf/config/lib/bin/armm3_test"

