#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/kobject.h>

#define DRIVER_NAME "nrf24"

/* --- Comandos SPI de nRF24L01+ --- */
#define CMD_R_REGISTER         0x00
#define CMD_W_REGISTER         0x20
#define CMD_R_RX_PAYLOAD       0x61
#define CMD_W_TX_PAYLOAD       0xA0
#define CMD_FLUSH_TX           0xE1
#define CMD_FLUSH_RX           0xE2
#define CMD_W_ACK_PAYLOAD      0xA8
#define CMD_R_RX_PL_WID        0x60
#define CMD_ACTIVATE           0x50

/* --- Registros nRF24L01+ --- */
#define REG_CONFIG             0x00
#define REG_EN_AA              0x01
#define REG_EN_RXADDR          0x02
#define REG_SETUP_RETR         0x04
#define REG_RF_CH              0x05
#define REG_RF_SETUP           0x06
#define REG_STATUS             0x07
#define REG_RX_ADDR_P0         0x0A
#define REG_TX_ADDR            0x10
#define REG_DYNPD              0x1C
#define REG_FEATURE            0x1D
#define REG_RX_PW_P0           0x11

/* --- Estructura de Contexto --- */
struct nrf24_dev {
    struct spi_device *spi;
    struct gpio_desc *ce_gpio;
    struct miscdevice miscdev;
    wait_queue_head_t rx_wq;
    struct mutex lock;
   
    u8 *spi_tx_buf; /* Buffer DMA seguro para TX */
    u8 *spi_rx_buf; /* Buffer DMA seguro para RX */

    u8 rx_buf[32];
    u8 rx_len;
    bool data_ready;
};

static struct kobject *nrf24_kobj;
static struct nrf24_dev *global_nrf24_dev = NULL;

static void nrf24_activate(struct spi_device *spi) {
    u8 tx[2] = { CMD_ACTIVATE, 0x73 };
    struct spi_transfer t = {
        .tx_buf = tx,
        .len = 2,
    };
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(spi, &m);
}

/* --- Métodos auxiliares SPI --- */
static u8 nrf24_read_reg(struct nrf24_dev *dev, u8 reg) {
    struct spi_transfer t = {
        .tx_buf = dev->spi_tx_buf,
        .rx_buf = dev->spi_rx_buf,
        .len = 2,
    };
    struct spi_message m;

    dev->spi_tx_buf[0] = CMD_R_REGISTER | (reg & 0x1F);
    dev->spi_tx_buf[1] = 0x00;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(dev->spi, &m);

    return dev->spi_rx_buf[1];
}

static void nrf24_write_reg(struct nrf24_dev *dev, u8 reg, u8 val) {
    struct spi_transfer t = {
        .tx_buf = dev->spi_tx_buf,
        .len = 2,
    };
    struct spi_message m;

    dev->spi_tx_buf[0] = CMD_W_REGISTER | (reg & 0x1F);
    dev->spi_tx_buf[1] = val;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(dev->spi, &m);
}

static void nrf24_write_buf(struct nrf24_dev *dev, u8 cmd, const u8 *buf, size_t len) {
    struct spi_transfer t = {
        .tx_buf = dev->spi_tx_buf,
        .len = len + 1,
    };
    struct spi_message m;

    if (len > 32) {
	dev_err(&dev->spi->dev, "    [SPI WRITE BUF ERR] len > 32 (%zu)\n", len);
        return;
    }

    dev->spi_tx_buf[0] = cmd;
    memcpy(&dev->spi_tx_buf[1], buf, len);

    if (buf && len > 0) {
        dev_info(&dev->spi->dev, "    ✏️ [SPI WRITE REQ] cmd=0x%02x, len=%zu, tx_data: %*ph\n", cmd, len, (int)len, buf);
    } else {
        dev_info(&dev->spi->dev, "    ✏️ [SPI WRITE REQ] cmd=0x%02x (solo comando)\n", cmd);
    }

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(dev->spi, &m);
}


static int nrf24_read_buf(struct nrf24_dev *dev, u8 cmd, u8 *buf, size_t len) {
    struct spi_transfer t = {
        .tx_buf = dev->spi_tx_buf,
        .rx_buf = dev->spi_rx_buf,
        .len = len + 1,
    };
    struct spi_message m;
    int ret;

    if (len > 32) {
	dev_err(&dev->spi->dev, "    [SPI READ BUF ERR] len > 32 (%zu)\n", len);
        return -EINVAL;
    }

    memset(dev->spi_tx_buf, 0, len + 1);
    memset(dev->spi_rx_buf, 0, len + 1);
    dev->spi_tx_buf[0] = cmd;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    dev_info(&dev->spi->dev, "    🔍 [SPI READ REQ] cmd=0x%02x, len=%zu\n", cmd, len);

    ret = spi_sync(dev->spi, &m);

    if (ret == 0 && buf) {
        memcpy(buf, &dev->spi_rx_buf[1], len);
        dev_info(&dev->spi->dev, "    🔍 [SPI READ RES] rx_data: %*ph\n", (int)len, buf);
    } else if (ret != 0) {
        dev_err(&dev->spi->dev, "    🔍 [SPI READ ERR] spi_sync fallo ret=%d\n", ret);
    }

    return ret;
}

/* --- Control de Auto-ACK (/sys/nrf24/auto_ack) --- */
static ssize_t auto_ack_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    u8 val;
    if (!global_nrf24_dev)
        return -ENODEV;

    val = nrf24_read_reg(global_nrf24_dev, 0x01); /* REG_EN_AA */
    return sysfs_emit(buf, "%d\n", val ? 1 : 0);  /* Retorna 1 si val > 0, de lo contrario 0 */
    /* Necesitaremos acceso al struct del driver o leer directamente el registro SPI */
    //return sysfs_emit(buf, "%d\n", global_nrf24_dev ? nrf24_read_reg(global_nrf24_dev, 0x01) : -1);
}

static ssize_t auto_ack_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    bool enable;
    if (kstrtobool(buf, &enable))
        return -EINVAL;

    if (global_nrf24_dev) {
        mutex_lock(&global_nrf24_dev->lock);
        nrf24_write_reg(global_nrf24_dev, 0x01, enable ? 0x3F : 0x00);
        mutex_unlock(&global_nrf24_dev->lock);
    }
    return count;
}
static struct kobj_attribute auto_ack_attribute = __ATTR_RW(auto_ack);

/* --- Control del Canal RF (/sys/nrf24/channel) --- */
static ssize_t channel_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sysfs_emit(buf, "%d\n", global_nrf24_dev ? nrf24_read_reg(global_nrf24_dev, 0x05) : -1);
}

static ssize_t channel_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    u8 ch;
    if (kstrtou8(buf, 10, &ch) || ch > 125)
        return -EINVAL;

    if (global_nrf24_dev) {
        mutex_lock(&global_nrf24_dev->lock);
        nrf24_write_reg(global_nrf24_dev, 0x05, ch);
        mutex_unlock(&global_nrf24_dev->lock);
    }
    return count;
}
static struct kobj_attribute channel_attribute = __ATTR_RW(channel);

/* --- Control de Velocidad (/sys/nrf24/datarate) --- */
/* Valores aceptados: 250k, 1M, 2M */
static ssize_t datarate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    u8 rf_setup;
    if (!global_nrf24_dev) return -ENODEV;

    rf_setup = nrf24_read_reg(global_nrf24_dev, 0x06); /* REG_RF_SETUP */

    if (rf_setup & (1 << 5))
        return sysfs_emit(buf, "250k\n");
    else if (rf_setup & (1 << 3))
        return sysfs_emit(buf, "2M\n");
    else
        return sysfs_emit(buf, "1M\n");
}

static ssize_t datarate_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    u8 rf_setup;
    if (!global_nrf24_dev) return -ENODEV;

    mutex_lock(&global_nrf24_dev->lock);
    rf_setup = nrf24_read_reg(global_nrf24_dev, 0x06) & ~((1 << 5) | (1 << 3));

    if (sysfs_streq(buf, "250k")) {
        rf_setup |= (1 << 5);       /* Bit RF_DR_LOW = 1 */
    } else if (sysfs_streq(buf, "2M")) {
        rf_setup |= (1 << 3);       /* Bit RF_DR_HIGH = 1 */
    } else if (!sysfs_streq(buf, "1M")) {
        mutex_unlock(&global_nrf24_dev->lock);
        return -EINVAL;             /* Opción no válida */
    }

    nrf24_write_reg(global_nrf24_dev, 0x06, rf_setup);
    mutex_unlock(&global_nrf24_dev->lock);

    return count;
}
static struct kobj_attribute datarate_attribute = __ATTR_RW(datarate);

/* --- Control de Potencia (/sys/nrf24/tx_power) --- */
/* Valores aceptados (dBm): -18, -12, -6, 0 */
static ssize_t tx_power_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    u8 pwr;
    if (!global_nrf24_dev) return -ENODEV;

    pwr = (nrf24_read_reg(global_nrf24_dev, 0x06) >> 1) & 0x03;
    switch (pwr) {
        case 0: return sysfs_emit(buf, "-18dBm\n");
        case 1: return sysfs_emit(buf, "-12dBm\n");
        case 2: return sysfs_emit(buf, "-6dBm\n");
        case 3: return sysfs_emit(buf, "0dBm\n");
    }
    return sysfs_emit(buf, "unknown\n");
}

static ssize_t tx_power_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    u8 rf_setup, pwr_bits = 0;
    if (!global_nrf24_dev) return -ENODEV;

    if (sysfs_streq(buf, "-18"))      pwr_bits = 0;
    else if (sysfs_streq(buf, "-12")) pwr_bits = 1;
    else if (sysfs_streq(buf, "-6"))  pwr_bits = 2;
    else if (sysfs_streq(buf, "0"))   pwr_bits = 3;
    else return -EINVAL;

    mutex_lock(&global_nrf24_dev->lock);
    rf_setup = nrf24_read_reg(global_nrf24_dev, 0x06) & ~(0x06); /* Limpiar bits 2:1 */
    rf_setup |= (pwr_bits << 1);
    nrf24_write_reg(global_nrf24_dev, 0x06, rf_setup);
    mutex_unlock(&global_nrf24_dev->lock);

    return count;
}
static struct kobj_attribute tx_power_attribute = __ATTR_RW(tx_power);

/* --- Grupo completo de atributos en /sys/nrf24/ --- */
static struct attribute *nrf24_attrs[] = {
    &auto_ack_attribute.attr,
    &channel_attribute.attr,
    &datarate_attribute.attr,
    &tx_power_attribute.attr,
    NULL,
};

static struct attribute_group nrf24_attr_group = {
    .attrs = nrf24_attrs,
};

/* --- Manejador de Interrupciones con Puntos de Debug --- */
static irqreturn_t nrf24_irq_handler(int irq, void *dev_id) {
    struct nrf24_dev *dev = dev_id;
    u8 status;

    dev_info(&dev->spi->dev, "⚡ >>> [IRQ START] Interrupción disparada en GPIO IRQ!\n");

    mutex_lock(&dev->lock);
    status = nrf24_read_reg(dev, REG_STATUS);

    dev_info(&dev->spi->dev, "🔔 [IRQ] Interrupción disparada! STATUS = 0x%02x (RX_DR=%d, TX_DS=%d, MAX_RT=%d)\n", status, (status >> 6) & 1, (status >> 5) & 1, (status >> 4) & 1);

    u8 pipe = (status >> 1) & 0x07;

    /* RX Data Ready Interrupt (Bit 6) */
    if ((status & (1 << 6)) || pipe != 0x07) {
        /* Leer tamaño de payload dinámico */
        nrf24_read_buf(dev, CMD_R_RX_PL_WID, &dev->rx_len, 1);
        
        /* Fallback: Si el chip devuelve 0 bytes o > 32, asumimos el paquete estándar de 8 bytes (int + float) */
        if (dev->rx_len == 0 || dev->rx_len > 32) {
            dev_warn(&dev->spi->dev, "⚠️ R_RX_PL_WID devolvió %d bytes. Aplicando fallback a 8 bytes...\n", dev->rx_len);
            dev->rx_len = 32;
        }

        dev_info(&dev->spi->dev, "📥 [IRQ RX] Leyendo %d bytes del FIFO RX...\n", dev->rx_len);

        /* Leer el payload recibido */
        nrf24_read_buf(dev, CMD_R_RX_PAYLOAD, dev->rx_buf, dev->rx_len);
        
        /* Imprimir los bytes leídos en dmesg */
        dev_info(&dev->spi->dev, "📦 [IRQ RX] Bytes leídos del chip: %*ph\n", dev->rx_len, dev->rx_buf);

        dev->data_ready = true;
        wake_up_interruptible(&dev->rx_wq);
	dev_info(&dev->spi->dev, "    [IRQ RX] wake_up_interruptible llamado (data_ready = true)\n");
    }

    /* TX Data Sent (Bit 5) / Max Retries (Bit 4) */
    if (status & (1 << 5)) {
        dev_info(&dev->spi->dev, "📤 [IRQ TX] Paquete enviado con éxito (TX_DS)\n");
    }
    if (status & (1 << 4)) {
//        dev_warn(&dev->spi->dev, "⚠️ [IRQ TX] Reintentos máximos alcanzados sin ACK (MAX_RT). Ejecutando FLUSH_TX...\n");
//        u8 cmd = CMD_FLUSH_TX;
//        spi_write(dev->spi, &cmd, 1);
        dev_warn(&dev->spi->dev, "    [IRQ TX] Error MAX_RT (Reintentos máximos alcanzados). Ejecutando FLUSH_TX...\n");
        nrf24_read_buf(dev, CMD_FLUSH_TX, NULL, 0);
    }

    /* Limpiar flags de interrupción escribiendo '1' en los bits 4, 5 y 6 */
//    nrf24_write_reg(dev, REG_STATUS, status | 0x70);
    nrf24_write_reg(dev, REG_STATUS, 0x70);
    dev_info(&dev->spi->dev, "    [IRQ ACK] STATUS limpiado con 0x70\n");
    mutex_unlock(&dev->lock);

    dev_info(&dev->spi->dev, "⚡ <<< [IRQ END] Manejador de interrupción completado\n");

    return IRQ_HANDLED;
}

/* --- Operación Read con Puntos de Debug --- */
static ssize_t nrf24_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct nrf24_dev *dev = container_of(file->private_data, struct nrf24_dev, miscdev);

    dev_info(&dev->spi->dev, "📖 [READ] Solicitud de lectura de User-Space. Esperando en cola (data_ready=%d)...\n", dev->data_ready);

    if (wait_event_interruptible(dev->rx_wq, dev->data_ready))
        return -ERESTARTSYS;

    dev_info(&dev->spi->dev, "⏰ [READ] Proceso despertó de la cola! Copiando %d bytes a User-Space...\n", dev->rx_len);

    mutex_lock(&dev->lock);
    if (count > dev->rx_len)
        count = dev->rx_len;

    if (copy_to_user(buf, dev->rx_buf, count)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    dev->data_ready = false;
    mutex_unlock(&dev->lock);

    dev_info(&dev->spi->dev, "✅ [READ] Copia a User-Space completada con éxito (%zd bytes)\n", count);
    return count;
}

static ssize_t nrf24_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    struct nrf24_dev *dev = container_of(file->private_data, struct nrf24_dev, miscdev);
    u8 config;

    dev_info(&dev->spi->dev, "📝 >>> [WRITE START] Solicitud de envío de User-Space (%zu bytes)\n", count);
    if (count > 32)
        count = 32;

    mutex_lock(&dev->lock);

    /* 1. Copiar datos desde User-Space al buffer del kernel */
    if (copy_from_user(dev->rx_buf, buf, count)) {
        mutex_unlock(&dev->lock);
	dev_err(&dev->spi->dev, "    [WRITE ERR] copy_from_user falló\n");
        return -EFAULT;
    }

    /* 2. Cambiar a Modo Transmisor (PRIM_RX = 0) */
    config = nrf24_read_reg(dev, REG_CONFIG);
    nrf24_write_reg(dev, REG_CONFIG, config & ~0x01);

    /* 3. Limpiar TX FIFO y cargar el nuevo payload */
    nrf24_write_buf(dev, CMD_FLUSH_TX, NULL, 0);
    nrf24_write_buf(dev, CMD_W_TX_PAYLOAD, dev->rx_buf, count);

    /* 4. Pulsar CE durante 15 us para iniciar la transmisión por aire */
    gpiod_set_value(dev->ce_gpio, 1);
    udelay(15);
    gpiod_set_value(dev->ce_gpio, 0);

    mutex_unlock(&dev->lock);

    dev_info(&dev->spi->dev, "📝 <<< [WRITE END] Pulso CE completado\n");

    return count;
}

static const struct file_operations nrf24_fops = {
    .owner          = THIS_MODULE,
    .read           = nrf24_read,
    .write          = nrf24_write,
};

static void nrf24_print_details(struct nrf24_dev *dev) {
    u8 status, config, rf_ch, rf_setup, en_aa, en_rxaddr, dynpd, feature;
    u8 rx_p0[5], tx_addr[5];
    u8 rx_p1[5];
    u8 rx_p2, rx_p3, rx_p4, rx_p5;
    u8 rx_pw[6];

    /* 1. Leer Registros Simples por SPI */
    status    = nrf24_read_reg(dev, REG_STATUS);
    config    = nrf24_read_reg(dev, REG_CONFIG);
    rf_ch     = nrf24_read_reg(dev, REG_RF_CH);
    rf_setup  = nrf24_read_reg(dev, REG_RF_SETUP);
    en_aa     = nrf24_read_reg(dev, REG_EN_AA);
    en_rxaddr = nrf24_read_reg(dev, REG_EN_RXADDR);
    dynpd     = nrf24_read_reg(dev, REG_DYNPD);
    feature   = nrf24_read_reg(dev, REG_FEATURE);

    /* 2. Leer Direcciones de Pipes */
    nrf24_read_buf(dev, CMD_R_REGISTER | REG_RX_ADDR_P0, rx_p0, 5);
    nrf24_read_buf(dev, CMD_R_REGISTER | 0x0B, rx_p1, 5); // REG_RX_ADDR_P1
    rx_p2 = nrf24_read_reg(dev, 0x0C); // P2
    rx_p3 = nrf24_read_reg(dev, 0x0D); // P3
    rx_p4 = nrf24_read_reg(dev, 0x0E); // P4
    rx_p5 = nrf24_read_reg(dev, 0x0F); // P5
    nrf24_read_buf(dev, CMD_R_REGISTER | REG_TX_ADDR, tx_addr, 5);

    /* 3. Leer Tamaños de Payloads Estáticos (RX_PW_P0..P5) */
    for (int i = 0; i < 6; i++) {
        rx_pw[i] = nrf24_read_reg(dev, 0x11 + i);
    }

    /* 4. Imprimir Configuración formateada en dmesg */
    dev_info(&dev->spi->dev, "================ SPI Configuration ================\n");
    dev_info(&dev->spi->dev, "CSN Pin          = SPI Bus %d, CS %d\n", dev->spi->controller->bus_num, spi_get_chipselect(dev->spi, 0));
    dev_info(&dev->spi->dev, "SPI Speed        = %d Mhz\n", dev->spi->max_speed_hz / 1000000);
    dev_info(&dev->spi->dev, "================ NRF Configuration ================\n");
    dev_info(&dev->spi->dev, "STATUS           = 0x%02x RX_DR=%d TX_DS=%d MAX_RT=%d RX_PIPE=%d TX_FULL=%d\n",
             status,
             (status >> 6) & 1, (status >> 5) & 1, (status >> 4) & 1,
             (status >> 1) & 0x07, status & 1);
    
    dev_info(&dev->spi->dev, "RX_ADDR_P0-1     = 0x%02x%02x%02x%02x%02x 0x%02x%02x%02x%02x%02x\n",
             rx_p0[4], rx_p0[3], rx_p0[2], rx_p0[1], rx_p0[0],
             rx_p1[4], rx_p1[3], rx_p1[2], rx_p1[1], rx_p1[0]);
    
    dev_info(&dev->spi->dev, "RX_ADDR_P2-5     = 0x%02x 0x%02x 0x%02x 0x%02x\n",
             rx_p2, rx_p3, rx_p4, rx_p5);
    
    dev_info(&dev->spi->dev, "TX_ADDR          = 0x%02x%02x%02x%02x%02x\n",
             tx_addr[4], tx_addr[3], tx_addr[2], tx_addr[1], tx_addr[0]);

    dev_info(&dev->spi->dev, "RX_PW_P0-6       = 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
             rx_pw[0], rx_pw[1], rx_pw[2], rx_pw[3], rx_pw[4], rx_pw[5]);

    dev_info(&dev->spi->dev, "EN_AA            = 0x%02x\n", en_aa);
    dev_info(&dev->spi->dev, "EN_RXADDR        = 0x%02x\n", en_rxaddr);
    dev_info(&dev->spi->dev, "RF_CH            = 0x%02x (Channel %d)\n", rf_ch, rf_ch);
    dev_info(&dev->spi->dev, "RF_SETUP         = 0x%02x\n", rf_setup);
    dev_info(&dev->spi->dev, "CONFIG           = 0x%02x\n", config);
    dev_info(&dev->spi->dev, "DYNPD/FEATURE    = 0x%02x 0x%02x\n", dynpd, feature);
    
    /* Decodificación de Data Rate */
    const char *dr_str = "1 Mbps";
    if (rf_setup & (1 << 5)) dr_str = "250 Kbps";
    else if (rf_setup & (1 << 3)) dr_str = "2 Mbps";
    dev_info(&dev->spi->dev, "Data Rate        = %s\n", dr_str);

    /* Decodificación de Potencia PA */
    u8 pa_pwr = (rf_setup >> 1) & 0x03;
    const char *pa_str = (pa_pwr == 3) ? "PA_MAX" : (pa_pwr == 2) ? "PA_HIGH" : (pa_pwr == 1) ? "PA_LOW" : "PA_MIN";
    dev_info(&dev->spi->dev, "PA Power         = %s\n", pa_str);

    /* Decodificación de CRC */
    dev_info(&dev->spi->dev, "CRC Length       = %s\n",
             (config & (1 << 3)) ? ((config & (1 << 2)) ? "16 bits" : "8 bits") : "Disabled");
}

/* --- Inicialización de los Registros del Radio --- */
static void nrf24_hw_init(struct nrf24_dev *dev) {
    
    pr_info("nRF24_DEBUG: Entrando a funcion nrf24_hw_init()...\n");

    const u8 addr[5] = "1Node";
    u8 cmd_ftx = CMD_FLUSH_TX;
    u8 cmd_frx = CMD_FLUSH_RX;

    /* Desactivar CE para poder configurar */
    gpiod_set_value(dev->ce_gpio, 0);

    /* 1. Limpiar FIFOs y Resetear Flags de STATUS a 0x0E */
    spi_write(dev->spi, &cmd_ftx, 1);
    spi_write(dev->spi, &cmd_frx, 1);
    nrf24_write_reg(dev, REG_STATUS, 0x70); // Escribir '1's limpia los flags de IRQ

    /* 2. Canal 108 (0x6C) */
    nrf24_write_reg(dev, REG_RF_CH, 108);

    /* 3. 250 kbps + PA_MIN + LNA High Current -> 0x21 */
    nrf24_write_reg(dev, REG_RF_SETUP, 0x21);

    /* 4. Auto-ACK en todos los Pipes (0x3F) y Habilitar Pipes 0 y 1 (0x03) */
    //nrf24_write_reg(dev, REG_EN_AA, 0x3F);
    // Desactivando Auto-ACK temporalmente
    nrf24_write_reg(dev, REG_EN_AA, 0x00);

    nrf24_write_reg(dev, REG_EN_RXADDR, 0x03);

    /* 5. Retries: 15 reintentos con delay de 1500us (0xFF) */
    nrf24_write_reg(dev, REG_SETUP_RETR, 0xFF);

    /* DESBLOQUEAR CARACTERÍSTICAS AVANZADAS (R_RX_PL_WID y Dynamic Payloads) */
    nrf24_activate(dev->spi);

    /* 6. Habilitar Dynamic Payloads en todos los Pipes (0x3F) y ACK Payloads (0x06) */
    nrf24_write_reg(dev, REG_FEATURE, 0x06); // EN_DPL | EN_ACK_PAY
    nrf24_write_reg(dev, REG_DYNPD, 0x3F);   // DPL en Pipes 0-5

    /* 7. Configurar Dirección "1Node" */
    nrf24_write_buf(dev, CMD_W_REGISTER | REG_RX_ADDR_P0, addr, 5);
    nrf24_write_buf(dev, CMD_W_REGISTER | REG_TX_ADDR, addr, 5);

    /* 8. CONFIG = 0x0F (CRC 16bit + PWR_UP + PRIM_RX + IRQs Habilitadas)
     * Nota: 0x0F es Modo Receptor (RX). Para Transmisor (TX) se usa 0x0E. */
    nrf24_write_reg(dev, REG_CONFIG, 0x0F);

    /* 9. Configurar tamaño de payload estático a 32 bytes para Pipe 0 */
    nrf24_write_reg(dev, REG_RX_PW_P0, 32);

    mdelay(5);
    gpiod_set_value(dev->ce_gpio, 1); // CE High para comenzar escucha (PRX)

    /* Imprimir tabla idéntica en dmesg */
    nrf24_print_details(dev);

}

/* --- Driver Probe / Remove --- */
static int nrf24_probe(struct spi_device *spi) {
    struct nrf24_dev *dev;
    int ret;

    pr_info("nRF24_DEBUG: Entrando a funcion nrf24_probe()...\n");

    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    /* Asignación de Buffers DMA-safe para SPI */
    dev->spi_tx_buf = devm_kzalloc(&spi->dev, 33, GFP_KERNEL | GFP_DMA);
    dev->spi_rx_buf = devm_kzalloc(&spi->dev, 33, GFP_KERNEL | GFP_DMA);
    if (!dev->spi_tx_buf || !dev->spi_rx_buf)
        return -ENOMEM;

    dev->spi = spi;
    mutex_init(&dev->lock);
    init_waitqueue_head(&dev->rx_wq);

    /* Obtener línea GPIO de Chip Enable (CE) */
    dev->ce_gpio = devm_gpiod_get(&spi->dev, "ce", GPIOD_OUT_LOW);
    if (IS_ERR(dev->ce_gpio)) {
        dev_err(&spi->dev, "Error al solicitar GPIO CE\n");
        return PTR_ERR(dev->ce_gpio);
    }

    /* Configurar Interrupción por Hardware en flanco de bajada (IRQ pin) */
    if (spi->irq > 0) {
        ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL, nrf24_irq_handler,
                                        IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
                                        DRIVER_NAME, dev);
        if (ret) {
            dev_err(&spi->dev, "Fallo al solicitar IRQ %d\n", spi->irq);
            return ret;
        }
    } else {
        dev_err(&spi->dev, "No se definió IRQ válida para el dispositivo\n");
        return -EINVAL;
    }

    /* Registrar dispositivo de carácter */
    dev->miscdev.minor = MISC_DYNAMIC_MINOR;
    dev->miscdev.name = DRIVER_NAME;
    dev->miscdev.fops = &nrf24_fops;
    dev->miscdev.parent = &spi->dev;

    ret = misc_register(&dev->miscdev);
    if (ret)
        return ret;

    spi_set_drvdata(spi, dev);
    nrf24_hw_init(dev);

    /* Guardamos la referencia global para los callbacks de /sys/nrf24/ */
    global_nrf24_dev = dev;

    /* Crear directorio /sys/nrf24 */
    nrf24_kobj = kobject_create_and_add("nrf24", NULL);
    if (nrf24_kobj) {
        if (sysfs_create_group(nrf24_kobj, &nrf24_attr_group)) {
            dev_err(&spi->dev, "Error al crear archivos en /sys/nrf24\n");
            kobject_put(nrf24_kobj);
        }
    }

    dev_info(&spi->dev, "Driver nRF24L01+ cargado correctamente (/dev/nrf24)\n");
    return 0;
}

static void nrf24_remove(struct spi_device *spi) {
    if (nrf24_kobj)
        kobject_put(nrf24_kobj);

    global_nrf24_dev = NULL; /* Limpiamos la referencia */

    struct nrf24_dev *dev = spi_get_drvdata(spi);
    
    pr_info("nRF24_DEBUG: Entrando a function nrf24_remove()...\n");
    gpiod_set_value(dev->ce_gpio, 0);
    nrf24_write_reg(dev, REG_CONFIG, 0x00); // Power Down
    misc_deregister(&dev->miscdev);
}

static const struct of_device_id nrf24_dt_ids[] = {
    { .compatible = "nordic,nrf24l01" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nrf24_dt_ids);

static struct spi_driver nrf24_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = nrf24_dt_ids,
    },
    .probe = nrf24_probe,
    .remove = nrf24_remove,
};

module_spi_driver(nrf24_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Driver Engineer");
MODULE_DESCRIPTION("Driver Kernel de Linux para transceptor nRF24L01+ vía SPI e Interrupciones");
