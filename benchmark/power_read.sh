  for i in $(seq 5); do awk '{printf "%d mA\n", $1/1000}' /sys/class/power_supply/sbs-9-000b/current_now; sleep 3; done; echo "backlight: $(cat /sys/class/backlight/backlight/brightness)/$(cat
  /sys/class/backlight/backlight/max_brightness)"; cat /sys/class/power_supply/sbs-9-000b/status

