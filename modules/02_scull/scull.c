#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <asm/uaccess.h>
#include "scull.h"

MODULE_AUTHOR("Alessandro Rubini, Jonathan Corbet");
MODULE_AUTHOR("Diogo Silva");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * Parameters that can be set at load time
 */

int scull_major = 	SCULL_MAJOR;
int scull_minor = 	0;
int scull_nr_devs = 	SCULL_NR_DEVS;	/* Number of bare scull devices */
int scull_quantum = 	SCULL_QUANTUM;
int scull_qset = 	SCULL_QSET;

module_param(scull_major, int, S_IRUGO);	/* S_IRUGO -> Read only parameter */
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

struct scull_dev *scull_devices;	/* allocated in scull_init_module */
static struct class *scull_class;     /* class used for device file creation */

/*
 * Empty out the scull device; must be called with the device semaphore held
 */
int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset = dev->qset;	/* "dev" is not-null */
	int i;

	PDEBUG("scull_trim evoked");

	for(dptr = dev->data; dptr; dptr=next){	/* Iterate the whole list. The for loop stops once dptr is null (meaning the linked list is over) */
		if(dptr->data){
			for(i=0; i < qset; i++)
				kfree(dptr->data[i]);

			kfree(dptr->data);
			dptr->data = NULL;	
		}
		next = dptr->next;
		kfree(dptr);
	}
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
}

/*
 * Open and close
 */
int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev; /* device information */

	PDEBUG("scull_open evoked");

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev; /* for other methods */

	/* now trim to 0 the length of the device if open was write_only and not appending */
	if( (filp->f_flags & O_ACCMODE) == O_WRONLY && !(filp->f_flags & O_APPEND)){
		if(down_interruptible(&dev->sem))
			return -ERESTARTSYS;

		scull_trim(dev); /* ignore errors */
		up(&dev->sem);
	}

	/* set offset if file has been opened for appending */
	if(filp->f_flags & O_APPEND){
        	filp->f_pos = dev->size;	
	}
	
	return 0;	/* success */
}

int scull_release(struct inode *inode, struct file *filp)
{
	PDEBUG("scull_release evoked");
	return 0;
}

/*
 * Follow the list
 */
static struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *qs = dev->data;

	PDEBUG("scull_follow evoked");

	/* Allocate first qset explicitly if need be */
	if (!qs){
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if(qs == NULL)
			return NULL; /* Never mind */

		memset(qs, 0, sizeof(struct scull_qset));
	}

	/* Then follow the list */
	while(n--){
		if(!qs->next){
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if(qs->next == NULL)
				return NULL; /* Never mind */

			memset(qs->next, 0, sizeof(struct scull_qset));
		}
		qs = qs->next;
		continue;
	}
	return qs;
}

/*
 * Data management: read and write
 */

ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;	/* the first listitem */
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset; /* how many bytes in the listitem */
	int item, s_pos, q_pos, rest, i = 0;
    size_t read = 0, to_read = 0;
	ssize_t retval = 0;

    PDEBUG("scull_read evoked");

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos >= dev->size)
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	/* find listitem, qset index, and offset in the quantum */
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	/* follow the list up to the right position (defined elsewhere) */
	dptr = scull_follow(dev, item);

    do{
        if (dptr == NULL || !dptr->data || ! dptr->data[s_pos])
		    goto out_success; /* don't fill holes */

        for(i = s_pos; i < qset && read < count; i++){
            if(!dptr->data[i])
                goto out_success;

            if((count - read) > quantum - q_pos)
                to_read = quantum - q_pos;
            else
                to_read = count - read;

            if (copy_to_user(buf + read, dptr->data[i] + q_pos, to_read)) {
                retval = -EFAULT;
                goto out;
            }

            read += to_read;
            q_pos = 0;
        }

        if(!dptr->next)
            goto out_success;

        s_pos = 0;
        dptr = dptr->next;
    }while(read < count);	

out_success:
	*f_pos += read;
	retval = read;

out:
	up(&dev->sem);
	return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
    int i = 0;
    size_t written = 0, to_write = 0;
	ssize_t retval = -ENOMEM; /* value used in "goto out" statements */

    PDEBUG("scull_write evoked");

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* find listitem, qset index and offset in the quantum */
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	/* follow the list up to the right position */
	dptr = scull_follow(dev, item);
	
    if (dptr == NULL)
		goto out;

    do{
        if(!dptr->data) {
            dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
            if(!dptr->data)
                goto out;

            memset(dptr->data, 0, qset * sizeof(char *));
        }

        for(i = s_pos; i < qset && written < count; i++){
            if(!dptr->data[i]) {
                dptr->data[i] = kmalloc(quantum, GFP_KERNEL);
                
                if (!dptr->data[i])
                    goto out;
            }

            if((count - written) > quantum - q_pos)
		        to_write = quantum - q_pos;
            else
                to_write = count - written;

            if (copy_from_user(dptr->data[i] + q_pos, buf + written, to_write)) {
                retval = -EFAULT;
                goto out;
            }

            q_pos = 0;
            written += to_write;
        }

        if(!dptr->next){
            dptr->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if(!dptr->next)
                goto out;

            memset(dptr->next, 0, sizeof(struct scull_qset));
        }

        dptr = dptr->next;
        s_pos = 0;
    }while(written < count);

	*f_pos += written;
	retval = written;

	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	up(&dev->sem);
	return retval;
}

/*
 * The "extended" operations -- only seek
 */

loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
	struct scull_dev *dev = filp->private_data;
	loff_t newpos;

	PDEBUG("scull_llseek evoked");

	switch(whence){
		case 0: /* SEEK_SET */
			newpos = off;
			break;

		case 1: /* SEEK_CUR */
			newpos = filp->f_pos + off;
			break;

		case 2: /* SEEK_END */
			newpos = dev->size + off;
			break;

		default: /* can't happen */
			return -EINVAL;
	}

	if (newpos < 0) return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

struct file_operations scull_fops = {
	.owner =	THIS_MODULE,
	.llseek = 	scull_llseek,
	.read = 	scull_read,
	.write = 	scull_write,
	.open = 	scull_open,
	.release = 	scull_release,
};

/*
 * Module functions
 */

/*
 * The cleanup function is used to handle initialization failures as well.
 * Therefore, it must be careful to work correctly even if some of the items
 * have not been initialized
 */
void scull_cleanup_module(void)
{
	int i;
	dev_t devno = MKDEV(scull_major, scull_minor), specific_devno;

	PDEBUG("scull_cleanup_module evoked");

	/* Get rid of our char dev entries */
	if(scull_devices){
		for(i = 0; i < scull_nr_devs; i++){
			scull_trim(scull_devices + i);
			cdev_del(&scull_devices[i].cdev);
            specific_devno = MKDEV(scull_major, scull_minor + i);
            device_destroy(scull_class, specific_devno);
		}
		kfree(scull_devices);
	}

	/* cleanup_module is never called if registering failed */
	unregister_chrdev_region(devno, scull_nr_devs);

    /* Destroy class */
    if(scull_class){
        class_destroy(scull_class);
    }  

	PDEBUG("Scull module cleaned up");
}

/*
 * Set up the char_dev structure for this device
 */
static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno;
    char name[50];

	PDEBUG("scull_setup_cdev evoked");

	devno = MKDEV(scull_major, scull_minor + index);
	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, devno, 1);

    if(err)
        goto err;

    sprintf(name, "scull%d", scull_minor + index);

    PDEBUG("Name: %s", name);

    /* Create sysfs device */
	if(IS_ERR(device_create(scull_class, NULL, devno, NULL, name))){
        printk(KERN_NOTICE "Could not create device %d", index);
        err = -1;
    }

err:
	/* Fail gracefully if need be */
	if(err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
	else
		PDEBUG("Device scull%d sucessfuly set up", index);
}

int scull_init_module(void)
{
	int result, i;
	dev_t dev = 0;

	PDEBUG("scull_init_module evoked");
	
	/*
	 * Get a range of minor numbers to work with, asking for a dynamic
	 * major unless directed otherwise at load time.
	 */
	if(scull_major){
		dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "scull");
	}else{
		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
		scull_major = MAJOR(dev);
	}
	if(result < 0){
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return result;
	}

    scull_class = class_create("scull_class");
    if(IS_ERR(scull_class)){
        printk(KERN_ERR "scull: could not create the struct class for device");
        goto fail;
    }

	/*
	 * Allocate the devices -- we can't have them static, as the number
	 * can be specified at load time
	 */
	scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
	if(!scull_devices){
		result = -ENOMEM;
		goto fail; /* Make this more graceful */
	}
	memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

	/* Initialize each device */
	for(i = 0; i < scull_nr_devs; i++){
		scull_devices[i].quantum = scull_quantum;
		scull_devices[i].qset = scull_qset;
		sema_init(&scull_devices[i].sem, 1);
		scull_setup_cdev(&scull_devices[i], i);
	}

	PDEBUG("Scull driver initialized");

	return 0; /* succeed */

fail:
	scull_cleanup_module();
	return result;

}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
