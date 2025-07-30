echo "Flashing Device 1 on /dev/cu.usbserial-0001..."
pio run --target upload --upload-port /dev/cu.usbserial-0001

echo "Flashing Device 2 on /dev/cu.usbserial-5..."
pio run --target upload --upload-port /dev/cu.usbserial-5

echo "Done! Both devices flashed."
echo "Press button on either device to send messages!"