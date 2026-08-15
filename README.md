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

## 🔌 Hardware Wiring (Raspberry Pi Example)

| nRF24L01+ Pin | Raspberry Pi Pin | Function |
| :--- | :--- | :--- |
| **VCC** | 3.3V (Pin 1) | Power (Add a 10µF capacitor across VCC/GND) |
| **GND** | GND (Pin 6) | Ground |
| **CSN** | SPI0_CE0 (Pin 24) | SPI Chip Select |
| **SCK** | SPI0_SCLK (Pin 23)| SPI Clock |
| **MOSI**| SPI0_MOSI (Pin 19)| SPI Master Out |
| **MISO**| SPI0_MISO (Pin 21)| SPI Master In |
| **CE**  | GPIO 22 (Pin 15)  | Chip Enable / Mode Control |
| **IRQ** | GPIO 25 (Pin 22)  | Hardware Interrupt Line |

---

## 🛠️ Building & Installation

### 1. Prerequisites
Ensure you have the Linux Kernel headers installed on your SBC:
```bash
sudo apt update
sudo apt install raspberrypi-kernel-headers build-essential
```

### 2.Compile the Driver
```bash
git clone [https://github.com/rbustos567/nrf24-linux-driver.git](https://github.com/rbustos567/nrf24-linux-driver.git)
cd nrf24-linux-driver
make
```
### 3. Load the Module
```bash
sudo insmod nrf24_driver.ko
```

### Verify device was created
```bash
ls -l /dev/nrf24
ls -la /sys/nrf24/
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

## 📜 License
This project is licensed under the GPL-2.0 License - see the LICENSE file for details.
