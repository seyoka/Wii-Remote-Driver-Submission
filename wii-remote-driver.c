/*
 * wii_remote_driver.c - A character/HID driver for a Wii Remote.
 *
 * This driver registers as a HID driver to capture raw reports from the Wii remote.
 * It performs basic input mapping (using button bit masks from your older working code)
 * and writes human-readable results to a circular buffer. The circular buffer is then
 * exposed via a character device (/dev/wii_remote) for user-space consumption.
 *
 * Additionally, an ioctl command triggers an output report (command 0x15) to request
 * a battery/status update, and the corresponding battery level (report ID 0x20) is also
 * written into the buffer. A /proc entry is created to report driver state.
 *
 */

#include <linux/module.h> // this module is for module init and module exit
#include <linux/kernel.h> // for printk
#include <linux/init.h> // for initialisation macros for module startup
#include <linux/fs.h> // file system structs and file ops
#include <linux/cdev.h> // for registering a char device and managing it
#include <linux/device.h> // for creating devices in /dev
#include <linux/mutex.h>
#include <linux/uaccess.h> // needed for copying data between user space and kernel space
#include <linux/hid.h> // handling HID devices
#include <linux/string.h>
#include <linux/ioctl.h> // macros to implement ioctl commands
#include <linux/proc_fs.h> // for creating enteries in proc
#include <linux/seq_file.h> // this is for sequential file operations in proc for easy state reporting

#define DRIVER_NAME "wii_remote_driver"
#define DEVICE_NAME "wii_remote"
#define CIRC_BUFFER_SIZE 1024 // buffer holds 1024 bytes of our input, you can change this size as needed

/* IOCTL command to request a battery/status update */
#define WIIMOTE_IOCTL_REQUEST_STATUS _IO('W', 1) // creates a simple ioctl command that doesnt send or recieve anything,
// the 'W' is to just specify 'Wii' as the device

/*  circular buffer for mapped output */
static char circ_buffer[CIRC_BUFFER_SIZE];
static int head = 0, tail = 0;
static DEFINE_MUTEX(circ_mutex);

/*
 * the buffer can hold:
 * 20 large inputs: "D-Pad-Left Report: ID=12, Dpad_Left Dpad_Right Dpad_Up Dpad_Down Plus Minus Home 2 1 B A"
 * 1024 / 50 = 20
 *
 * 51 small events: "A"
 * 1024 / 20 = 51
 *
 * The smallest possible event without an input is 15 characters
*/

/* pointer to the HID device instance */
static struct hid_device *wii_hid_dev = NULL;


static int wii_connected = 0;     /* 1 if connected, 0 if not */
static int wii_last_battery = -1; /* -1 means unknown */
static struct proc_dir_entry *wii_proc_entry; // pointer to the wii-remote proc entry


static void circ_buffer_write(const char *data, size_t len)
{
    size_t i;
    mutex_lock(&circ_mutex);
    /* This is the start of the crit section */
    for (i = 0; i < len; i++) {
        int next = (head + 1) % CIRC_BUFFER_SIZE;
        if (next == tail) {
            printk(KERN_WARNING DRIVER_NAME ": circular buffer full, dropping data\n");
            break;
        }
        circ_buffer[head] = data[i];
        head = next;
    }
    mutex_unlock(&circ_mutex);
}

/*
 * perform_input_mapping - this parses a button report and write a human-readable string
 * into the circular buffer.
 *
 * :
 *   Byte 0: Report ID.
 *   Byte 1: D-pad and special buttons:
 *            Bit 0: D-pad Left
 *            Bit 1: D-pad Right
 *            Bit 2: D-pad Down
 *            Bit 3: D-pad Up
 *            Bit 4: Plus Button
 *            Bit 7: Home Button    ( idk why Home is found by Byte 2 for the code below it just works ) - Ciaran
                                    Yeah its cooked at times I wont lie - Ryan
 *
 *   Byte 2: Action buttons:
 *            Bit 0: A Button
 *            Bit 1: B Button
 *            Bit 2: Button 1
 *            Bit 3: Button 2
 *            Bit 5: Minus Button
 */
static void perform_input_mapping(const u8 *data, int size)
{

    /*
     * The output is capped at 256 but this is isnt possible in a single event unless,
     * a macro is used.
     * Its just large enough to hold every actual event, the result is truncated regardless
     * be careful changing this pls because report trunucation will f*ck the userspace
     * 🙏
    */
    char mapping_output[256];
    int len = 0;

    /* Report has to be 3 bytes or its cooked */
    if (size < 3) {
        printk(KERN_WARNING DRIVER_NAME ": Report too short for mapping\n");
        return;
    }

    u8 report_id = data[0];
    u8 btn_byte1 = data[1];
    u8 btn_byte2 = data[2];

    /* using snprintf here as we need to have buffer safety with the circular buffer
    other wise id be writing edge cases do not change  */
    len += snprintf(mapping_output + len, sizeof(mapping_output) - len,
                    "Report: ID=%u, ", report_id);

    if (btn_byte1 & 0x01)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Left");
    if (btn_byte1 & 0x02)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Right ");
    if (btn_byte1 & 0x04)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Down ");
    if (btn_byte1 & 0x08)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Up ");
    if (btn_byte1 & 0x10)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Plus ");

    if (btn_byte2 & 0x10)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Minus ");
    if (btn_byte2 & 0x80)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Home ");
    if (btn_byte2 & 0x01)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "2 ");
    if (btn_byte2 & 0x02)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "1 ");
    if (btn_byte2 & 0x04)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "B ");
    if (btn_byte2 & 0x08)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "A");

    if (len == 0)
        len = snprintf(mapping_output, sizeof(mapping_output), "No buttons pressed");

    if (len < sizeof(mapping_output) - 1) {
        mapping_output[len++] = '\n';
        mapping_output[len] = '\0';
    }
    printk(KERN_INFO "Mapped Output: %s\n", mapping_output);
    circ_buffer_write(mapping_output, len);
}

/* Character device file operations */
static int device_open(struct inode *inode, struct file *file)
{
    /*
     * these do nothing but return success, they are simply just place holders

     * they just allow the device to successfully be opened / released
    */
    return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * this is where the circular buffer is read
*/
static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    size_t bytes_copied = 0;

    /*
     * *(pointer)buf is what the user passes to read so,
     * in user space app you need to pass a poiner to buf
     * the same size as whatever the circ_buff is which I have set to
     * 1024 bytes
    */

    mutex_lock(&circ_mutex);
    while (bytes_copied < count && tail != head) {
        if (copy_to_user(buf + bytes_copied, &circ_buffer[tail], 1)){ // this is what copies to user space, Ciaran - Ryan  {
            mutex_unlock(&circ_mutex);
            return -EFAULT; // error code for "Bad Address"
            /*
             * this handles;
             * invalid user space pointers
             * or if the kernel tries to access restricted memory
            */
        }
        tail = (tail + 1) % CIRC_BUFFER_SIZE;
        bytes_copied++;
    }
    mutex_unlock(&circ_mutex);
    return bytes_copied; // this is just the number of bytes not the actual data
                         // actual data is transferred to uspace through the function
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    switch (cmd) // purpose of this will just check if the command is availiable
    {
    case WIIMOTE_IOCTL_REQUEST_STATUS: // defined as _IO('W', 1), for battery request
        if (wii_hid_dev) {
            /*
             * 0x15 is the status code for wii remote battery
             * no additional param is needed after status code
            */
            u8 status_request[2] = { 0x15, 0x00 };
            printk(KERN_INFO "Sending battery status request (output report 0x15)\n");

            /*
             * this is just a function from HID it just sends a report to the device
             * HID_OUTPUT_REPORT just a flag to indicate we are sending raw
             * HID_REW_SET_REPORT its a request type. Means we are writing a report with the data it gives us
            */
            ret = hid_hw_raw_request(
                                    wii_hid_dev,
                                     status_request[0],
                                     status_request,
                                     sizeof(status_request),
                                     HID_OUTPUT_REPORT,
                                     HID_REQ_SET_REPORT);
            printk(KERN_INFO "Battery status request returned: %d\n", ret);
            if (ret < 0)
                printk(KERN_ERR DRIVER_NAME ": failed to send status request, error %d\n", ret);
        } else {
            printk(KERN_ERR DRIVER_NAME ": HID device not available for status request\n");
            ret = -ENODEV; // this just means no such device
        }
        break;
    default:
        ret = -ENOTTY; // this is just the error code for if the command is unrecognised
    }
    return ret;
}

/*
 * this is the structure for the file ops with the char device
 * basically the kernel calls these commands when the userspace app
 * performs any of these actions on /dev/wii_remote
*/
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = device_open,
    .release        = device_release,
    .read           = device_read,
    .unlocked_ioctl = device_ioctl,
};

/*
 * this just writes to proc
 * seq_printf is what other drivers online do when writing to proc
 * its just a special print that simplifies writing to proc
 * it just means we dont have to worry about paging or restarting output if the file
 * is read more than once
*/
static int wii_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "Wii Remote Driver State:\n");
    seq_printf(m, "  Connected: %s\n", wii_connected ? "Yes" : "No");
    seq_printf(m, "  Last Battery: %d\n", wii_last_battery);
    return 0;
}

/*
 * this is just for when the user opens proc
 * it links the seq_file to the output
 * single open just manages the output so we dont have to do
 * f*cky stuff if theres multiple reads, abstracts us having to handle that
*/
static int wii_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, wii_proc_show, NULL);
}


/*
 * this just defined the operations for the proc entry
 * this is just boilerplate pretty much
 * works the same as the other structs
*/
static const struct proc_ops wii_proc_ops = {
    .proc_open    = wii_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek, // this is responsible for repositioning the files
                               // the files read/write pointer
    .proc_release = single_release,
};

/* Character device variables */
static int major;
static struct class *wii_class; // the devices class
static struct cdev wii_cdev; // struct to register device

/*
 * wii_raw_event - HID raw event callback.
 *
 * When a new HID report is received from the Wii remote, this callback is invoked.
 * otherwise, we perform input mapping.
 */
static int wii_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    int i;
    printk(KERN_INFO "wii_raw_event: received report: ");
    for (i = 0; i < size; i++)
        printk(KERN_CONT "%02x ", data[i]);
    printk(KERN_CONT "\n");

    if (size > 0 && data[0] == 0x20) {
        /*
         * this is the check for the battery report
        */
        printk(KERN_INFO "Battery status report detected.\n");
        if (size >= 2) {
            char battery_output[64];
            int len = snprintf(battery_output, sizeof(battery_output), "Battery: %d\n", data[1]);
            /* cache the battery level */
            wii_last_battery = data[1];
            circ_buffer_write(battery_output, len);
        }
    } else {
        /*
         * then if its anything else just perform input mapping
        */
        perform_input_mapping(data, size);
    }
    return 0;
}

/* HID probe: called when a matching device is connected */
static int wii_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;

    ret = hid_parse(hdev); // parses the report descriptor
    if (ret) // checks if the result is malformed
        return ret;

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT); // this just initialises the device
                                                   // tells it to start sending reports
    if (ret) // same check as above pretty much if the init fails error
        return ret;

    wii_hid_dev = hdev; // sets our HID device global variab to hdev which is the device
    wii_connected = 1; // for proc
    printk(KERN_INFO DRIVER_NAME ": Wii remote connected\n");
    return 0;
}

/* HID remove: called when the device is disconnected */
static void wii_remove(struct hid_device *hdev)
{
    wii_hid_dev = NULL;
    wii_connected = 0;
    printk(KERN_INFO DRIVER_NAME ": Wii remote disconnected\n");
}

/* HID device ID table for the Wii remote.
 */
static const struct hid_device_id wii_remote_devices[] = {
        //specifies the bus   // Ninetendo Vendor ID, Product ID, version
    { HID_DEVICE(BUS_BLUETOOTH, 0x057e, 0x0306, 0) },
    { }
};
// this just creates a special area of memory that lists all the supported devices
// by the module
// basically when a device with the product and vendor ID appear in the
// HID device table this piece of code tells it to load this module
MODULE_DEVICE_TABLE(hid, wii_remote_devices);

/* HID driver structure */
static struct hid_driver wii_driver = {
    .name       = DRIVER_NAME,
    .id_table   = wii_remote_devices,
    .probe      = wii_probe,
    .remove     = wii_remove,
    .raw_event  = wii_raw_event,
};

static int __init wii_init(void)
{
    int ret;
    dev_t dev; // device number

    // 0 for defualt permissions, NULL means no parent dir
    wii_proc_entry = proc_create("wii_remote", 0, NULL, &wii_proc_ops);
    if (!wii_proc_entry) {
        printk(KERN_ERR DRIVER_NAME ": failed to create /proc/wii_remote\n");
        return -ENOMEM; // memory allocation failure
    }

    // requests to allocate a major number for a single device
    // 0 is the minor, DEVICE_NAME labels the region
    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR DRIVER_NAME ": failed to allocate char device region\n");
        return ret;
    }
    // stores the major in global variable after alloc_chardev_region
    major = MAJOR(dev);

    // inits the char device
    cdev_init(&wii_cdev, &fops);
    ret = cdev_add(&wii_cdev, dev, 1); // associates the wii_cdeb with the device nums
    if (ret < 0) {
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to add cdev\n");
        return ret;
    }

    // creates a device class (device node)
    wii_class = class_create(DEVICE_NAME);
    if (IS_ERR(wii_class)) {
        cdev_del(&wii_cdev);
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to create class\n");
        return PTR_ERR(wii_class);
    }
    device_create(wii_class, NULL, dev, NULL, DEVICE_NAME); // creates a device node
    // ^ NULL - no parent device, NULL 2 just means no device specific data

    // registers a HID device
    ret = hid_register_driver(&wii_driver);
    if (ret) {
        device_destroy(wii_class, dev);
        class_destroy(wii_class);
        cdev_del(&wii_cdev);
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to register HID driver\n");
        return ret;
    }

    printk(KERN_INFO DRIVER_NAME ": driver loaded (major %d)\n", major);
    return 0;
}

static void __exit wii_exit(void)
{
    dev_t dev = MKDEV(major, 0); // creates a device number from global major
    // this is needed to clean up and unregister the character device

    if (wii_proc_entry) {
        remove_proc_entry("wii_remote", NULL);
        wii_proc_entry = NULL;
    }

    hid_unregister_driver(&wii_driver);
    device_destroy(wii_class, dev);
    class_destroy(wii_class);
    cdev_del(&wii_cdev);
    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO DRIVER_NAME ": driver unloaded\n");
}

module_init(wii_init);
module_exit(wii_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ryan, Ciaran and Peter ");
MODULE_DESCRIPTION("");
