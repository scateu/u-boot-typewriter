echo "= 1. What idle states exist + do they use WFI? =="
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/desc
# (WFI as the shallowest C-state is what we care about)
#= 1. What idle states exist + do they use WFI? ==
#WFI
#cpu-sleep
#cluster-sleep
#ARM WFI
#cpu-sleep
#cluster-sleep

echo "= 2. Is the CPU actually entering idle (not busy)? usage counts =="
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/usage
#= 2. Is the CPU actually entering idle (not busy)? usage counts ==
#119056
#8884
#74313

echo "= 3. Which interrupt is the timer, and is it firing (broadcast vs local)? =="
grep -iE 'tick|timer|arch_timer' /proc/interrupts
#= 3. Which interrupt is the timer, and is it firing (broadcast vs local)? ==
# 20:          0          0          0          0          0          0     GICv3  27 Level     kvm guest vtimer
# 23:      13286       6151      33218      30753       9241       4662     GICv3  30 Level     arch_timer
# 25:      10250       5552       4011       2878       3971       3087     GICv3 113 Level     rk_timer
#IPI4:      2672       2805       2109       2492       2001       2502       Timer broadcast interrupts

echo " 4. The 32.768kHz clock + RTC — who uses it =="
grep -r . /sys/kernel/debug/clk/clk_summary 2>/dev/null | grep -iE '32k|rtc|xin'
# or:
cat /sys/kernel/debug/clk/clk_summary | grep -iE '32768|rtc|xin24m'
# 4. The 32.768kHz clock + RTC — who uses it ==
# xin32k                              0       0        0        32768       0          0     50000      Y   deviceless                      no_connection_id         
# xin24m                              21      23       0        24000000    0          0     50000      Y   deviceless                      no_connection_id         
#          clk_emmc                   1       2        0        150000000   0          0     50000      Y            fe330000.mmc                    clk_xin                  
#    clk_32k_suspend_pmu              0       0        0        32743       0          0     50000      Y      deviceless                      no_connection_id         
# xin32k                              0       0        0        32768       0          0     50000      Y   deviceless                      no_connection_id         
# xin24m                              21      23       0        24000000    0          0     50000      Y   deviceless                      no_connection_id         

echo "= 5. Is there a broadcast timer (needed if local timer stops in idle)? =="
cat /sys/devices/system/clockevents/broadcast/current_device 2>/dev/null
dmesg | grep -iE 'clockevent|arch_timer|broadcast|sched_clock' | head
#= 5. Is there a broadcast timer (needed if local timer stops in idle)? ==
#rk_timer
#[    0.000000] arch_timer: cp15 timer(s) running at 24.00MHz (phys).
#[    0.000002] sched_clock: 56 bits at 24MHz, resolution 41ns, wraps every 4398046511097ns

echo "= 6. Exception level Linux was entered at (sanity, we think EL2) =="
dmesg | grep -iE 'CPU: All CPU|Booting Linux|EL2|EL1|psci' | head
#= 6. Exception level Linux was entered at (sanity, we think EL2) ==
#[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd034]
#[    0.000000] psci: probing for conduit method from DT.
#[    0.000000] psci: PSCIv1.1 detected in firmware.
#[    0.000000] psci: Using standard PSCI v0.2 function IDs
#[    0.000000] psci: MIGRATE_INFO_TYPE not supported.
#[    0.000000] psci: SMC Calling Convention v1.5
#[    0.043088] CPU: All CPU(s) started at EL2
#[    0.043137] CPU features: detected: 32-bit EL1 Support
