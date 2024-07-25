#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>

#include "scull.h"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Rémi THEBAULT");

static int scull_major = SCULL_MAJOR;
static int scull_minor = 0;
static int scull_quantum = SCULL_QUANTUM;
static int scull_qset = SCULL_QSET;
static int scull_num_devs = SCULL_NUM_DEVS;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);
module_param(scull_num_devs, int, S_IRUGO);

static int scull_open(struct inode *inode, struct file *filp);
static int scull_release(struct inode *inode, struct file *filp);
static ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *f_pos);
static ssize_t scull_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *f_pos);

static struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.open = scull_open,
	.release = scull_release,
	.read = scull_read,
	.write = scull_write,
};

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;
	int quantum; /* Current quantum size */
	int qset; /* Current array size */
	unsigned long size;
	unsigned int access_key;
	struct mutex lock;
	struct cdev cdev;
};

static struct scull_dev *scull_devices = NULL;

#ifdef SCULL_DEBUG

static int scull_proc_seqshow(struct seq_file *s, void *v);

static int scull_proc_singleshow(struct seq_file *s, void *v)
{
	const int limit = s->size - 80;

	for (int i = 0; i < scull_num_devs && s->count < limit; i++) {
		struct scull_dev *dev = scull_devices + i;

		const int err = scull_proc_seqshow(s, dev);
		if (err) {
			return err;
		}
	}

	return 0;
}

static void *scull_proc_seqstart(struct seq_file *s, loff_t *pos)
{
	if (*pos >= scull_num_devs) {
		return NULL;
	}
	return scull_devices + *pos;
}

static void *scull_proc_seqnext(struct seq_file *s, void *v, loff_t *pos)
{
	++*pos;
	if (*pos >= scull_num_devs) {
		return NULL;
	}
	return scull_devices + *pos;
}

static void scull_proc_seqstop(struct seq_file *s, void *v)
{
	// Nothing to do
}

static int scull_proc_seqshow(struct seq_file *s, void *v)
{
	struct scull_dev *dev = (struct scull_dev *)v;

	if (mutex_lock_interruptible(&dev->lock)) {
		return -ERESTARTSYS;
	}

	size_t num_items = 0;
	for (struct scull_qset *qs = dev->data; qs; qs = qs->next) {
		num_items++;
	}

	seq_printf(
		s,
		"Scull Device %i: %li items (qset=%i, quantum=%i), size = %li\n",
		(int)(dev - scull_devices), num_items, dev->qset, dev->quantum,
		dev->size);

	for (struct scull_qset *qs = dev->data; qs; qs = qs->next) {
		seq_printf(s, "  item at %p; qset at %p\n", qs, qs->data);
		if (qs && !qs->next) { /* only print the last item */
			for (int i = 0; i < dev->qset; ++i) {
				if (qs->data[i]) {
					seq_printf(s, "    % 4i: %8p\n", i,
						   qs->data[i]);
				}
			}
		}
	}

	mutex_unlock(&dev->lock);

	return 0;
}

static const struct seq_operations scull_proc_seq_ops = {
	.start = scull_proc_seqstart,
	.stop = scull_proc_seqstop,
	.next = scull_proc_seqnext,
	.show = scull_proc_seqshow,
};

static int scull_proc_singleopen(struct inode *inode, struct file *file)
{
	return single_open(file, scull_proc_singleshow, NULL);
}

static int scull_proc_seqopen(struct inode *inode, struct file *file)
{
	return seq_open(file, &scull_proc_seq_ops);
}

static struct proc_ops scull_proc_ops_single = {
	.proc_open = scull_proc_singleopen,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops scull_proc_ops_seq = {
	.proc_open = scull_proc_seqopen,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
};

static void scull_proc_create(void)
{
	proc_create("scullsingle", 0, NULL, &scull_proc_ops_single);
	proc_create("scullseq", 0, NULL, &scull_proc_ops_seq);
}

static void scull_proc_remove(void)
{
	remove_proc_entry("scullseq", NULL);
	remove_proc_entry("scullsingle", NULL);
}

#endif

/* 
* Trim the scull device to the minimum size. 
* Must be called with lock held.
*/
static int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *dptr, *next;
	int qset = dev->qset;

	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (int i = 0; i < qset; ++i) {
				kfree(dptr->data[i]);
			}
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

static struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *dptr = dev->data;

	if (!dptr) {
		dptr = dev->data =
			kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (!dptr) {
			return NULL;
		}
	}

	while (n--) {
		if (!dptr->next) {
			dptr->next =
				kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (!dptr->next) {
				return NULL;
			}
		}
		dptr = dptr->next;
	}

	return dptr;
}

static int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (mutex_lock_interruptible(&dev->lock)) {
			return -ERESTARTSYS;
		}
		scull_trim(dev);
		mutex_unlock(&dev->lock);
	}

	return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;

	if (mutex_lock_interruptible(&dev->lock)) {
		return -ERESTARTSYS;
	}

	ssize_t retval = 0;

	if (*f_pos > dev->size) {
		goto out;
	}
	if (*f_pos + count > dev->size) {
		count = dev->size - *f_pos;
	}

	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;

	int item = (long)*f_pos / itemsize;
	int rest = (long)*f_pos % itemsize;
	int s_pos = rest / quantum;
	int q_pos = rest % quantum;

	struct scull_qset *dptr = scull_follow(dev, item);

	if (!dptr || !dptr->data || !dptr->data[s_pos]) {
		goto out;
	}

	if (count > quantum - q_pos) {
		count = quantum - q_pos;
	}

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;

	if (mutex_lock_interruptible(&dev->lock)) {
		return -ERESTARTSYS;
	}

	int retval = 0;

	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;

	int item = (long)*f_pos / itemsize;
	int rest = (long)*f_pos % itemsize;
	int s_pos = rest / quantum;
	int q_pos = rest % quantum;

	struct scull_qset *dptr = scull_follow(dev, item);
	if (!dptr) {
		goto out;
	}

	if (!dptr->data) {
		dptr->data = kzalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data) {
			retval = -ENOMEM;
			goto out;
		}
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kzalloc(quantum * sizeof(char), GFP_KERNEL);
		if (!dptr->data[s_pos]) {
			retval = -ENOMEM;
			goto out;
		}
	}
	if (count > quantum - q_pos) {
		count = quantum - q_pos;
	}

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	if (dev->size < *f_pos) {
		dev->size = *f_pos;
	}

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static void scull_cleanup(void)
{
	dev_t devno = MKDEV(scull_major, scull_minor);

	if (scull_devices) {
		for (int i = 0; i < scull_num_devs; ++i) {
			scull_trim(scull_devices + i);
			cdev_del(&scull_devices[i].cdev);
		}
		kfree(scull_devices);
	}

	/* scull_cleanup is not call if registering fails */
	unregister_chrdev_region(devno, 4);
}

static int scull_setup_cdev(struct scull_dev *dev, int major, int minor,
			    int index)
{
	int err;
	dev_t devno = MKDEV(major, minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev, devno, 1);

	if (err) {
		pr_err("Error %d adding scull%d", err, index);
	}

	return err;
}

static int __init scull_init(void)
{
	dev_t devno;
	int err;
	if (scull_major) {
		devno = MKDEV(scull_major, scull_minor);
		err = register_chrdev_region(devno, 4, "scull");
	} else {
		err = alloc_chrdev_region(&devno, 0, 4, "scull");
		scull_major = MAJOR(devno);
		scull_minor = MINOR(devno);
	}

	if (err)
		return err;

	scull_devices = kzalloc(4 * sizeof(struct scull_dev), GFP_KERNEL);
	if (!scull_devices) {
		err = -ENOMEM;
		goto fail_unregister;
	}

	for (int i = 0; i < 4; i++) {
		struct scull_dev *dev = &scull_devices[i];
		dev->quantum = scull_quantum;
		dev->qset = scull_qset;
		mutex_init(&dev->lock);
		err = scull_setup_cdev(dev, scull_major, scull_minor, i);
		if (err) {
			goto fail;
		}
	}

#ifdef SCULL_DEBUG
	scull_proc_create();
#endif

	return 0;

fail:
	scull_cleanup();
	return err;

fail_unregister:
	unregister_chrdev_region(devno, 4);
	return err;
}

static void __exit scull_exit(void)
{
#ifdef SCULL_DEBUG
	scull_proc_remove();
#endif

	scull_cleanup();
}

module_init(scull_init);
module_exit(scull_exit);
