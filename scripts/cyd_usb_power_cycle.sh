#!/usr/bin/env sh
# Power-cycle the CYD USB supply via Tasmota (same endpoints as wfusb1-off / wfusb1-on).
# Override with CYD_USB_POWER_OFF / CYD_USB_POWER_ON if your plug differs.

set -eu
OFF_URL="${CYD_USB_POWER_OFF:-http://192.168.1.169/cm?cmnd=Power%20OFF}"
ON_URL="${CYD_USB_POWER_ON:-http://192.168.1.169/cm?cmnd=Power%20ON}"

echo "USB power OFF: ${OFF_URL}"
curl -sS "${OFF_URL}" || true
sleep 2
echo "USB power ON: ${ON_URL}"
curl -sS "${ON_URL}" || true
