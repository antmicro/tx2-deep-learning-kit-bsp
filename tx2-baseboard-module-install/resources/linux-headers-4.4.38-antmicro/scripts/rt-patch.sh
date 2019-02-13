#!/bin/bash

# The script helps to apply/revert RT patches to/from
# K4.4 kernel source

any_failure=0
apply_rt_patches()
{
	file_list=`find ../rt-patches -name \*.patch -type f | sort`
	for p in $file_list; do
		# set flag in case of failure and continue
		patch -s -d .. -p1 < $p || any_failure=1
	done
	./config --file ../arch/arm64/configs/tegra_t186ref_gnu_linux_defconfig --enable PREEMPT_RT_FULL  --disable CPU_IDLE_TEGRA18X  --disable CPU_FREQ_GOV_INTERACTIVE || any_failure=1

}

revert_rt_patches()
{
	file_list=`find ../rt-patches -name \*.patch -type f | sort -r`
	for p in $file_list; do
		# set flag in case of failure and continue
		patch -s -R -d .. -p1 < $p || any_failure=1
	done
	#  CPU_FREQ_GOV_INTERACTIVE need to keep disable for Automotive
	./config --file ../arch/arm64/configs/tegra_t186ref_gnu_linux_defconfig --disable PREEMPT_RT_FULL  --enable CPU_IDLE_TEGRA18X  --disable CPU_FREQ_GOV_INTERACTIVE || any_failure=1
}

usage()
{
	echo Usages:
	echo 1. ${0} apply-patches : Apply RT patches
	echo 2. ${0} revert-patches : Revert RT patches
	any_failure=1
}

# script starts from here
dir_run_from=`dirname ${0}`
pushd $dir_run_from &>/dev/null

if [ "$1" == "apply-patches" ]; then
	apply_rt_patches
elif [ "$1" == "revert-patches" ]; then
	revert_rt_patches
else
	echo "Wrong argument"
	usage
fi

popd &>/dev/null

exit $any_failure
