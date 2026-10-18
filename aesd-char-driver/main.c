/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h> //for kmalloc and krealloc and kfree
#include <linux/uaccess.h> //for copy_to_user and copy_from_user
#include <linux/string.h> //memchr access

#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Aaron Pineda"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;

    dev = container_of(inode->i_cdev,struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    //nothing to release here 
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	    ssize_t retval = 0;
    	PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    	/**
     	* TODO: handle read
    	*/
	struct aesd_dev *dev=filp->private_data;
	struct aesd_buffer_entry *entry;
	size_t entry_offset = 0;
	size_t bytes;
	
	if(mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, (size_t)*f_pos, &entry_offset);

	if(!entry)
		goto out; //return 0 for EOF
	bytes = entry->size-entry_offset;
	if(bytes>count)
		bytes=count;
	
	if(copy_to_user(buf, entry->buffptr+entry_offset, bytes)){
		retval=-EFAULT;
		goto out;
	}

	*f_pos+=bytes;
	retval=bytes;

out:
	mutex_unlock(&dev->lock);
   	return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    //copy file pointer data
    struct aesd_dev *dev =filp->private_data;
    char *newbuf;

    if(count==0)
	    return 0;
    if(mutex_lock_interruptible(&dev->lock))
	    return -ERESTARTSYS;
    //grow the buffer to allow for new bytes in the command
    newbuf=krealloc(dev->partial.buffptr, dev->partial.size+count, GFP_KERNEL);
    if(!newbuf)
	goto out; //old partial buffer is still valid so no need to discard
    dev->partial.buffptr=newbuf;//reallocate buffer data
    if(copy_from_user(newbuf+dev->partial.size, buf, count))
    {
	    retval=-EFAULT;
	    goto out;
    }
    dev->partial.size+=count; //increase buffer size to match allocated data
    

    //if the command is completed then add it to the circular buffer
    if(memchr(dev->partial.buffptr, '\n', dev->partial.size)){
	    const char *old=NULL;
	    if(dev->buffer.full)
		    old=dev->buffer.entry[dev->buffer.in_offs].buffptr;
	    aesd_circular_buffer_add_entry(&dev->buffer, &dev->partial);
	    kfree(old);//free up the overwritten entry
	    dev->partial.buffptr=NULL;
	    dev->partial.size=0;
    }
    retval=count;
out:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    result=aesd_setup_cdev(&aesd_device);
    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index)
    {
		    kfree(entry->buffptr);
    }
    kfree(aesd_device.partial.buffptr);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
