#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <asm/io.h>

#define GPIO_BASE 0x20200000

#define GPIO_FSEL_INPUT  0b000
#define GPIO_FSEL_OUTPUT 0b001

/* select which pin to use */
#define LED_GPIO_PIN 18

#define MAX_DEV 1

#define WRITE_BUF_SIZE 63

static char *az[] = {".-", "-...", "-.-.", "-..", ".", "..-.", 
             "--.", "....", "..", ".---", "-.-", ".-..", 
             "--", "-.", "---", ".--.", "--.-", ".-.", 
             "...", "-", "..-", "...-", ".--", "-..-", 
             "-.--", "--.."};

static char *nbr[] = { "-----", ".----", "..---", 
             "...--", "....-", ".....", "-....", "--...", 
             "---..", "----." };

struct gpio_registers_map {
    uint32_t GPFSEL[6];
    uint32_t Reserved1;
    uint32_t GPSET[2];
    uint32_t Reserved2;
    uint32_t GPCLR[2];
};

struct gpio_registers_map *gpio_registers;

struct leddy_device_data {
    struct cdev cdev;
};

static int leddy_dev_major = 0;
static struct class *leddy_class = NULL;
static struct leddy_device_data leddy_data[MAX_DEV];
static struct device *dev_obj;
static unsigned int interval = 300;

static void set_gpio_value(int gpio, bool val);
static ssize_t leddy_write(struct file *file, const char __user *buf, 
                           size_t count, loff_t *offset);

static const struct file_operations leddy_fops = {
    .owner       = THIS_MODULE,
    .write       = leddy_write
};

static ssize_t show_interval(struct device *dev,
                                    struct device_attribute *attr, char *buf) {
    return snprintf(buf, PAGE_SIZE, "%u\n", interval);
}

static ssize_t set_interval(struct device* dev,
    struct device_attribute* attr,
    const char* buf,
    size_t count)
{
    unsigned int new_value = 0;
    if (kstrtouint(buf, 10, &new_value) < 0) {
        return -EINVAL;
    }

    if (new_value < 100) {
        return -EINVAL;
    }

    interval = new_value;

    return count;
}

static DEVICE_ATTR(interval, S_IRWXU | S_IRWXG | S_IROTH, show_interval, set_interval);

static char* get_code(char c) {
    if (isalpha(c)) {
        return az[tolower(c)-97];
    }
    else if (isdigit(c)) {
        return nbr[c - 48];
    }

    return "";
}

static void morse_short_signal(const int gpio) {
    set_gpio_value(gpio, true);
    msleep(interval);
    set_gpio_value(gpio, false);
}

static void morse_long_signal(const int gpio) {
    set_gpio_value(gpio, true);
    msleep(3 * interval);
    set_gpio_value(gpio, false);
}

static void morse_pause(const int times) {
    msleep(times * interval);
}

static void morse_char(const int gpio, const char c) {
    char *morse = get_code(c);
    for (;*morse != 0; ++morse) {
        if (*morse == '.') {
            morse_short_signal(gpio);
        }
        else {
            morse_long_signal(gpio);
        }
        morse_pause(1);
    }

    morse_pause(2);
}

static void morse_code(const int gpio, const char *buf, bool start) {
    /* check if we have had a word split in a buffer split */
    if (!start && !isalnum(*buf)) {
        morse_pause(5);
        /* skip any more word boundaries */
        while (*buf != 0 && !isalnum(*(buf++)))
            ;
    }

    while (*buf != 0) {
        while (*buf != 0 && isalnum(*buf)) {
            morse_char(gpio, *buf);
            buf++;
        }

        /* end of word, we have waited 3*unit so lets wait 2 more */
        morse_pause(2);

        /* skip any more word boundaries */
        while (*buf != 0 && !isalnum(*(buf++)))
            ;
    }
}

static ssize_t leddy_write(struct file *file, const char __user *buf, 
                           size_t count, loff_t *offset) {

    char kern_buf[WRITE_BUF_SIZE+1];
    size_t buf_offset = 0;
    unsigned int minor = MINOR(file->f_path.dentry->d_inode->i_rdev);

    printk(KERN_INFO "Write to minor device %d\n", minor);

    while (buf_offset < count) {
        const size_t left = count - buf_offset;
        size_t size = left < WRITE_BUF_SIZE ? left : WRITE_BUF_SIZE;
        if (copy_from_user(kern_buf, buf+buf_offset, size)) {
            /* an error occurred, we have processed buf_offset bytes */
            return buf_offset;
        }
        kern_buf[size] = 0;

        morse_code(LED_GPIO_PIN, kern_buf, buf_offset == 0);

        buf_offset += size;
    }

    return count;
}

/**
 * Callback for handling adding devices to /dev by udev/mdev.
 * We want to make sure that the created device only can be
 * written to.
 */
static int leddy_uevent(struct device *dev, struct kobj_uevent_env *env) {
    add_uevent_var(env, "DEVMODE=%#o", 0222);
    return 0;
}

static int char_dev_init(void) {
    int i;
    dev_t dev;

    int err = alloc_chrdev_region(&dev, 0, MAX_DEV, "leddy");
    if (err < 0) {
        return err;
    }

    /* get the assigned major device number */
    leddy_dev_major = MAJOR(dev);

    /* create the group of devices */
    leddy_class = class_create(THIS_MODULE, "leddy");
    leddy_class->dev_uevent = leddy_uevent;

    dev_obj = device_create(leddy_class, NULL, 0, NULL, "leddyctrl");
    device_create_file(dev_obj, &dev_attr_interval);

    for (i = 0; i < MAX_DEV; ++i) {
        /* init minor character device with file operation callbacks */
        cdev_init(&leddy_data[i].cdev, &leddy_fops);
        leddy_data[i].cdev.owner = THIS_MODULE;

        /* add the device to the system, going live */
        cdev_add(&leddy_data[i].cdev, MKDEV(leddy_dev_major, i), 1);

        /* create device in sysfs */
        device_create(leddy_class, NULL, MKDEV(leddy_dev_major, i), NULL, "leddy%d", i);
    }

    return 0;
}

static void char_dev_exit(void) {
    int i;

    device_remove_file(dev_obj, &dev_attr_interval);
    device_destroy(leddy_class, dev_obj->devt);

    for (i = 0; i < MAX_DEV; ++i) {
        device_destroy(leddy_class, MKDEV(leddy_dev_major, i));
    }

    class_unregister(leddy_class);
    class_destroy(leddy_class);

    unregister_chrdev_region(MKDEV(leddy_dev_major, 0), MINORMASK);
}

static void set_gpio_function(int gpio, int fsel) {
    int register_index = gpio / 10;
    int offset = (gpio % 10) * 3;

    unsigned int old_val = readl(gpio_registers->GPFSEL + register_index);
    unsigned int mask = 0b111 << offset;
    unsigned int old_val_masked = old_val & ~mask;
    unsigned int fsel_val = (fsel << offset) & mask;
    writel(old_val_masked | fsel_val, gpio_registers->GPFSEL + register_index);
}

static void set_gpio_value(int gpio, bool val) {
    if (val) {
        writel(1 << (gpio % 32), gpio_registers->GPSET);
    }
    else {
        writel(1 << (gpio % 32), gpio_registers->GPCLR);
    }
}

static int __init leddy_init(void) {
    int err;

    gpio_registers = (struct gpio_registers_map *)ioremap(GPIO_BASE, sizeof(struct gpio_registers_map));
    set_gpio_function(LED_GPIO_PIN, GPIO_FSEL_OUTPUT);
    set_gpio_value(LED_GPIO_PIN, false);

    err = char_dev_init();
    if (err < 0) {
        iounmap(gpio_registers);
        return err;
    }

    printk(KERN_INFO "Init of leddy module.\n");
    return 0;
}

static void __exit leddy_exit(void) {
    char_dev_exit();

    /* we need to set it to low before we unmap the memory */
    set_gpio_value(LED_GPIO_PIN, false);
    iounmap(gpio_registers);
    printk(KERN_INFO "Exit of leddy module.\n");
}

module_init(leddy_init);
module_exit(leddy_exit);

MODULE_AUTHOR("Hampus Ram <hampus.ram@educ.goteborg.se>");
MODULE_DESCRIPTION("Minimal kernel module example.");
MODULE_LICENSE("GPL");
