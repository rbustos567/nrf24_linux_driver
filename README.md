# nRF24L01+ Linux Kernel Driver

A robust, interrupt-driven **Linux Kernel Module** for the **nRF24L01+** 2.4GHz wireless transceiver, designed for Single Board Computers like the Raspberry Pi.

Unlike user-space Python or C++ libraries, this driver interfaces directly with the Linux Kernel SPI and GPIO subsystems. It exposes a standard Character Device (`/dev/nrf24`) for reading/writing radio payloads, and a clean Sysfs directory (`/sys/nrf24/`) for live hardware configuration.

---

## 🌟 Key Features

* **DMA-Safe SPI Transfers:** Dedicated Kernel buffers allocated with `GFP_DMA` for maximum reliability.
* **Interrupt-Driven (IRQ):** Hardware interrupts wake up waiting processes only when new data arrives (zero CPU spinning).
* **Native UNIX Stream Interface:** Send and receive string/binary payloads directly using standard Unix tools (`cat`, `echo`, `tee`).
* **Live Configuration via `/sys/nrf24/`:** Change RF channels, data rates, transmission power, and Auto-ACK parameters on the fly.
* **Clone-Friendly:** Configurable Auto-ACK toggle to support low-cost or asymmetric nRF24 modules.

---
## IMPORTANT NOTE: Hardware Limitations and Clone Modules

Many low-cost nRF24L01+ modules available on the market are unofficial clones or counterfeit chips (often labeled as Si24R1 or generic clones). Due to manufacturing variations or silicon defects, several of these clone modules exhibit hardware-level flaws where they can only function as transmitters (TX) or only as receivers (RX), but fail when switching modes dynamically.

Labeling Recommendation: It is strongly recommended to test your individual modules, identify which ones work strictly for transmitting or receiving, and physically label or tag each module accordingly to avoid troubleshooting non-functional configurations in future setups.

As a result of these hardware limitations, hardware ACK (Acknowledgement) and dynamic bidirectional payload exchanges may fail silently or drop packets consistently. Consequently, the current driver implementation operates as a unidirectional (UDP-style) communication link, where data frames are transmitted without waiting for hardware ACKs.
---

## 🔌 Hardware Wiring (Raspberry Pi Example)

Important: It is strongly recommended to use the nRF24L01+ Adapter Module (with an on-board AMS1117 3.3V regulator and decoupling capacitors) to ensure power stability. Connect VCC of the adapter module to 5V.
```bash
nRF24L01+ Power Adapter Pinout
             ┌───────────────┐
       GND  ─┤ [1]       [2] ├─ VCC (5V Input)
        CE  ─┤ [3]       [4] ├─ CSN
       SCK  ─┤ [5]       [6] ├─ MOSI
      MISO  ─┤ [7]       [8] ├─ IRQ (Unused)
             └───────────────┘
```

## 🍓 1. Raspberry Pi 3 & Raspberry Pi 5

| nRF24L01+ | RPi Physical Pin (40-pin Header) | Function / Signal |
| :--- | :--- | :--- |
| **VCC** | **Pin 2** or **Pin 4** | **5V Power** |
| **GND** | **Pin 6** | **Ground** |
| **CE** | **Pin 22** | **GPIO 25** |
| **CSN** | **Pin 24** | **GPIO 8 (SPI0_CE0)** |
| **SCK** | **Pin 23** | **GPIO 11 (SPI0_SCLK)** |
| **MOSI** | **Pin 19** | **GPIO 10 (SPI0_MOSI)** |
| **MISO** | **Pin 21** | **GPIO 9 (SPI0_MISO)** |
| **IRQ** | **Pin 18**  | **GPIO 24 (Using it for Hardware Interrupt Line)** |

---
<img width="2064" height="1185" alt="raspberry-pi-5-gpio-pinout-diagram" src="https://github.com/user-attachments/assets/d86e6ee5-1cff-456e-99ff-df1ba5a1b983" />

---

## 🛠️ Building & Installation

### 1. Prerequisites
Ensure you have the Linux Kernel headers installed on your SBC:
```bash
sudo apt update
sudo apt install raspberrypi-kernel-headers build-essential device-tree-compiler linux-headers-$(uname -r)
```

### 2. Clone this repository
Clone this repository
```bash
git clone https://github.com/rbustos567/nrf24_linux_driver.git
```

### 3. Compile the DTS source file
```bash
cd nrf24_linux_driver/dts
dtc -@ -I dts -O dtb -o nrf24.dtbo nrf24-overlay.dts
```

### 4. Install and apply the overlay
#### For Raspberry Pi:
```bash
sudo cp nrf24.dtbo /boot/overlays/
# Enable it by adding the following line to /boot/firmware/config.txt or /boot/config.txt (depending on your OS version):
dtoverlay=nrf24
# Ensure the primary SPI Controller is enabled in /boot/firmware/config.txt or /boot/config.txt (depending on your OS version):
dtparam=spi=on
# Shutdown and restart the system to apply the Device Tree changes
sudo shutdown now
# After rebooting, verify that the SPI interface is active
ls -l /dev/spidev*
```
#### For BeagleBone Black:
```bash
sudo cp nrf24.dtbo /lib/firmware/
# Enable it in /boot/uEnv.txt:
uboot_overlay_addr0=/lib/firmware/nrf24.dtbo
# Shutdown and restart the system to apply the Device Tree changes
sudo shutdown now
# Check if spidev device is present
ls -l /dev/spidev*
# Verify overlay loading in kernel logs
dmesg | grep -i "spi"
```

### 5. Compile the Driver
```bash
cd ../src
make
```
### 6. Load the Module
```bash
sudo insmod nrf24.ko
```

### 7. Verify module loading and kernel logs
```bash
# View kernel debug output
dmesg | tail -n 30

# device char was created
ls -la /sys/nrf24/

# List module was uploaded
lsmod | grep nrf24
```

## 🚀 Usage Guide
### Reading Data (Receiver Mode)
Simply read from /dev/nrf24. The process will sleep until a wireless packet arrives via IRQ:
```bash
# Continuous string reader
sudo cat /dev/nrf24

# Inspect raw hexadecimal bytes
sudo hexdump -C /dev/nrf24
```

### Writing Data (Transmitter Mode)
Write any byte sequence (up to 32 bytes per packet) to /dev/nrf24:
```bash
# Send a string message
echo -n "Hello World!" | sudo tee /dev/nrf24 > /dev/null
```

## ⚙️ Hardware Control (/sys/nrf24/)
You can read or update hardware registers at runtime without reloading the module:
### RF Channel (channel)
Valid range: 0 to 125 (2.400 GHz - 2.525 GHz).
```bash
# Check current channel
cat /sys/nrf24/channel

# Set channel to 76 (2.476 GHz)
echo 76 | sudo tee /sys/nrf24/channel
```

### Data Rate (datarate)
Supported options: 250k, 1M, 2M.
```bash
# Set air data rate to 250 kbps (Maximum sensitivity/range)
echo "250k" | sudo tee /sys/nrf24/datarate
```

### Transmission Power (tx_power)
Supported dBm values: -18, -12, -6, 0.
```bash
# Set maximum output power (0 dBm)
echo "0" | sudo tee /sys/nrf24/tx_power
```

### Auto-Acknowledge (auto_ack)
Enable (1) or disable (0). Disabling Auto-ACK (Fire-and-Forget mode) is strongly recommended when using low-cost clone modules.
```bash
# Disable Auto-ACK
echo 0 | sudo tee /sys/nrf24/auto_ack
```

### Unload the kernel Module
To remove the module from memory during testing or driver updates:
```bash
sudo rmmod nrf24
```

## 📜 License
This project is licensed under the GPL-2.0 License - see the LICENSE file for details.
