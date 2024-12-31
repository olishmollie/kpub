#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/printk.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define NUM_TOPICS 256
#define MAX_STR_LEN 256
#define MAX_BUF_SIZE PAGE_SIZE

struct topic {
	wait_queue_head_t inq, outq;
	size_t size;
	size_t nreaders, nwriters;
	size_t wp;
	char *buf;
	char name[MAX_STR_LEN];
	struct device dev;
	struct cdev cdev;
	struct list_head entry;
	struct semaphore sem;
};

#define cdev_to_topic(ptr) container_of(ptr, struct topic, cdev);
#define dev_to_topic(ptr) container_of(ptr, struct topic, dev);
#define node_to_topic(ptr) list_entry(ptr, struct topic, entry);

/* Stores all topics. */
LIST_HEAD(topics);

/* Represents the class under sysfs. */
static struct class kpub_class;

/* Defines file operations for each topic. */
static struct file_operations kpub_fops;

/* Contains the module's major and minor numbers. */
static dev_t kpub_devt;

/* Major number assigned by the kernel. */
static int major_num;

/* Tracks minor numbers in use. */
static uint8_t minor_nums[NUM_TOPICS];

/* Read the topic name. */
static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct topic *topic = dev_to_topic(dev);
	size_t len = strlen(topic->name);
	memcpy(buf, topic->name, len);
	return len;
}

/* Read the topic buffer size. */
static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct topic *topic = dev_to_topic(dev);
	return snprintf(buf, MAX_STR_LEN, "%lu", topic->size);
}

DEVICE_ATTR_RO(name);
DEVICE_ATTR_RO(size);
static struct attribute *topic_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_size.attr,
	NULL,
};
ATTRIBUTE_GROUPS(topic);

/* Reserve and return the first available minor number. */
static int reserve_minor_num(void)
{
	int i;

	for (i = 0; i < NUM_TOPICS; ++i) {
		if (!minor_nums[i]) {
			minor_nums[i] = 1;
			return i;
		}
	}

	return -1;
}

/* Mark the given minor number as available. */
static void release_minor_num(int minor_num)
{
	minor_nums[minor_num] = 0;
}

/* Release the topic's embedded device. */
static void device_release(struct device *dev)
{
	// No need to release stack bound devices, but Linux expects one to be defined.
}

/* Create a new topic by writing its name to the class attribute. */
static ssize_t create_topic_store(const struct class *cp,
				  const struct class_attribute *attr,
				  const char *buf, size_t len)
{
	int devt, err, minor_num;
	struct topic *topic;

	if (len == 0) {
		pr_alert("%s: topic cannot have an empty name\n",
			 THIS_MODULE->name);
		return -EINVAL;
	}

	if (len > MAX_STR_LEN) {
		pr_alert("%s: topic too long, max %d bytes\n",
			 THIS_MODULE->name, MAX_STR_LEN);
		return -EINVAL;
	}

	topic = (struct topic *)kzalloc(sizeof(*topic), GFP_KERNEL);
	if (!topic)
		return -ENOMEM;

	topic->size = 9;
	memcpy(topic->name, buf, len);

	sema_init(&topic->sem, 1);
	init_waitqueue_head(&topic->inq);
	init_waitqueue_head(&topic->outq);

	topic->buf = (char *)kzalloc(topic->size, GFP_KERNEL);
	if (!topic->buf) {
		err = -ENOMEM;
		goto cleanup_topic;
	}

	minor_num = reserve_minor_num();
	if (minor_num < 0) {
		pr_alert("%s: maximum number of topics (%d) reached\n",
			 THIS_MODULE->name, NUM_TOPICS);
		err = -E2BIG;
		goto cleanup_topic;
	}

	devt = MKDEV(major_num, minor_num);

	cdev_init(&topic->cdev, &kpub_fops);
	topic->cdev.owner = THIS_MODULE;

	err = cdev_add(&topic->cdev, devt, 1);
	if (err < 0) {
		pr_alert("%s: could not add character device\n",
			 THIS_MODULE->name);
		goto cleanup_cdev;
	}

	err = kobject_set_name(&topic->dev.kobj, "kpub!%s", topic->name);
	if (err < 0) {
		pr_alert("%s: could not set topic '%s' name\n",
			 THIS_MODULE->name, topic->name);
		goto cleanup_cdev;
	}

	topic->dev.class = &kpub_class;
	topic->dev.release = device_release;
	topic->dev.devt = devt;
	topic->dev.id = minor_num;

	err = device_register(&topic->dev);
	if (err) {
		pr_alert("%s: could not add device '%s'\n", THIS_MODULE->name,
			 topic->name);
		goto cleanup_cdev;
	}

	list_add(&topic->entry, &topics);

	pr_info("%s: created topic '%s'\n", THIS_MODULE->name, topic->name);

	return len;

cleanup_cdev:
	cdev_del(&topic->cdev);
cleanup_topic:
	kfree(topic->buf);
	kfree(topic);

	return err;
}

/* Delete a topic and release its resources. */
static void delete_topic(struct topic *topic)
{
	release_minor_num(topic->dev.id);
	list_del(&topic->entry);
	device_unregister(&topic->dev);
	cdev_del(&topic->cdev);
	kfree(topic->buf);
	kfree(topic);
}

/* Remove a topic by writing its name to the class attribute. */
static ssize_t remove_topic_store(const struct class *cls,
				  const struct class_attribute *attr,
				  const char *buf, size_t len)
{
	struct topic *topic = NULL, *tp;
	struct list_head *node;

	if (len >= MAX_STR_LEN)
		return -EINVAL;

	list_for_each(node, &topics) {
		tp = node_to_topic(node);
		if (strcmp(tp->name, buf) == 0) {
			topic = tp;
			break;
		}
	}

	if (!topic)
		return -ENODEV;

	delete_topic(topic);

	pr_info("%s: removed topic '%s'\n", THIS_MODULE->name, buf);

	return len;
}

/*
 * Default class sysfs attributes:
 *  - create_topic
 * 	- remove_topic
 */
CLASS_ATTR_WO(create_topic);
CLASS_ATTR_WO(remove_topic);
static struct attribute *class_attrs[] = {
	&class_attr_create_topic.attr,
	&class_attr_remove_topic.attr,
	NULL,
};
ATTRIBUTE_GROUPS(class);

static struct class kpub_class = {
	.name = "kpub",
	.dev_groups = topic_groups,
	.class_groups = class_groups,
};

/* Open the device for reading xor writing. */
static int kpub_open(struct inode *inode, struct file *file)
{
	struct topic *topic = cdev_to_topic(inode->i_cdev);
	int err = 0;

	if (down_interruptible(&topic->sem))
		return -ERESTARTSYS;

	file->private_data = topic;

	if (file->f_mode & FMODE_READ && !(file->f_mode & FMODE_WRITE)) {
		++topic->nreaders;
	} else if (file->f_mode & FMODE_WRITE && !(file->f_mode & FMODE_READ)) {
		++topic->nwriters;
		wake_up_interruptible(&topic->inq);
	} else {
		dev_err(&topic->dev,
			"topic must be opened as reader xor writer");
		err = -EACCES;
	}

	dev_info(&topic->dev, "updating file pos...\n");
	file->f_pos = topic->wp;

	up(&topic->sem);

	return err;
}

static int kpub_release(struct inode *inode, struct file *file)
{
	struct topic *topic = cdev_to_topic(inode->i_cdev);
	int err = 0;

	if (down_interruptible(&topic->sem))
		return -ERESTARTSYS;

	file->private_data = topic;
	dev_info(&topic->dev, "closing topic '%s'\n", topic->name);

	if (file->f_mode & FMODE_READ && !(file->f_mode & FMODE_WRITE)) {
		--topic->nreaders;
	} else if (file->f_mode & FMODE_WRITE && !(file->f_mode & FMODE_READ)) {
		--topic->nwriters;
	} else {
		dev_err(&topic->dev,
			"topic must be opened as reader xor writer");
		err = -EACCES;
	}

	up(&topic->sem);

	return err;
}

static ssize_t kpub_read(struct file *file, char __user *buf, size_t len,
			 loff_t *off)
{
	struct topic *topic = file->private_data;

	if (down_interruptible(&topic->sem))
		return -ERESTARTSYS;

	dev_info(&topic->dev, "%s sleeping...\n", topic->name);
	while (topic->wp == *off) {
		up(&topic->sem);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(topic->inq, (topic->wp != *off)))
			return -ERESTARTSYS;
		if (down_interruptible(&topic->sem))
			return -ERESTARTSYS;
	}

	dev_info(&topic->dev, "info to read!\n");

	if (topic->wp > *off)
		len = min(len, (size_t)(topic->wp - *off));
	else
		len = min(len, (size_t)(topic->size - *off));

	dev_info(&topic->dev, "copying %lu bytes to buffer\n", len);
	if (copy_to_user(buf, &topic->buf[*off], len))
		return -EFAULT;

	*off += len;
	if (*off >= topic->size)
		*off = 0;

	up(&topic->sem);

	wake_up_interruptible(&topic->outq);

	return len;
}

/* Return the number of bytes available to be written to. */
static size_t bytes_to_write(struct topic *topic, loff_t *off)
{
	if (topic->wp == *off)
		return topic->size - 1;
	if (topic->wp > *off)
		return topic->wp - *off;
	return topic->size + topic->wp - *off;
}

static ssize_t kpub_write(struct file *file, const char __user *buf, size_t len,
			  loff_t *off)
{
	struct topic *topic = file->private_data;

	if (len > topic->size) {
		dev_err(&topic->dev, "%lu byte message is too large\n", len);
		return -EINVAL;
	}

	if (down_interruptible(&topic->sem))
		return -ERESTARTSYS;

	while (bytes_to_write(topic, off) == 0) {
		up(&topic->sem);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(topic->outq,
					     (bytes_to_write(topic, off) > 0)))
			return -ERESTARTSYS;
		if (down_interruptible(&topic->sem))
			return -ERESTARTSYS;
	}

	if (topic->wp >= *off)
		len = min(len, (size_t)(topic->size - topic->wp));
	else
		len = min(len, (size_t)(*off - topic->wp));

	dev_info(&topic->dev, "copying %lu bytes from buf\n", len);
	if (copy_from_user(&topic->buf[topic->wp], buf, len)) {
		up(&topic->sem);
		return -EFAULT;
	}

	topic->wp += len;
	if (topic->wp == topic->size)
		topic->wp = 0;

	up(&topic->sem);

	wake_up_interruptible(&topic->inq);

	return len;
}

static unsigned kpub_poll(struct file *file, poll_table *ppt)
{
	struct topic *topic = file->private_data;
	int ready_mask = 0;

	down(&topic->sem);

	if (topic->nwriters == 0) {
		up(&topic->sem);
		return POLLHUP;
	}

	poll_wait(file, &topic->inq, ppt);
	poll_wait(file, &topic->outq, ppt);

	if (file->f_pos != topic->wp)
		ready_mask |= (POLLIN | POLLRDNORM);
	if (bytes_to_write(topic, &file->f_pos) > 0)
		ready_mask |= POLLOUT | POLLWRNORM;

	up(&topic->sem);

	return ready_mask;
}

static struct file_operations kpub_fops = {
	.owner = THIS_MODULE,
	.open = kpub_open,
	.release = kpub_release,
	.read = kpub_read,
	.write = kpub_write,
	.poll = kpub_poll,
};

static int __init kpub_init(void)
{
	int err;

	err = alloc_chrdev_region(&kpub_devt, 0, NUM_TOPICS, THIS_MODULE->name);
	if (err < 0) {
		pr_alert("%s: could not allocate character device region\n",
			 THIS_MODULE->name);
		goto cleanup;
	}

	major_num = MAJOR(kpub_devt);

	err = class_register(&kpub_class);
	if (err) {
		pr_alert("%s: could not register class\n", THIS_MODULE->name);
		goto cleanup;
	}

	pr_info("%s: initialized module\n", THIS_MODULE->name);

	return 0;

cleanup:
	unregister_chrdev_region(kpub_devt, NUM_TOPICS);

	return err;
}

static void __exit kpub_exit(void)
{
	struct list_head *node, *tmp;
	struct topic *topic;

	list_for_each_safe(node, tmp, &topics) {
		topic = node_to_topic(node);
		pr_info("%s: removing topic '%s'\n", THIS_MODULE->name,
			topic->name);
		delete_topic(topic);
	}

	class_unregister(&kpub_class);
	unregister_chrdev_region(kpub_devt, NUM_TOPICS);
	pr_info("%s: removed module\n", THIS_MODULE->name);
}

module_init(kpub_init);
module_exit(kpub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andy Bond (olishmollie@gmail.com)");
MODULE_DESCRIPTION("A device-based pub/sub framework.");
