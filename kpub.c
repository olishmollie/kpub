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
#define MAX_STR_LEN 64
#define MAX_BUF_SIZE PAGE_SIZE
#define MAX_MSG_SIZE PAGE_SIZE
#define MAX_MSG_COUNT 64

struct topic {
	uint32_t msg_size, msg_count;
	uint32_t nreaders, nwriters;
	uint32_t wp, rp, len, rcount;
	char *buf;
	char name[MAX_STR_LEN];
	struct device dev;
	struct cdev cdev;
	struct list_head entry;
	struct mutex mtx;
	wait_queue_head_t inq, outq;
};

#define cdev_to_topic(ptr) container_of(ptr, struct topic, cdev);
#define dev_to_topic(ptr) container_of(ptr, struct topic, dev);
#define node_to_topic(ptr) list_entry(ptr, struct topic, entry);

/* Stores all topics. */
LIST_HEAD(topics);

/* Protects topic creation. */
static DEFINE_MUTEX(topic_mtx);

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

/* Read the topic message size. */
static ssize_t msg_size_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct topic *topic = dev_to_topic(dev);
	return snprintf(buf, MAX_STR_LEN, "%u", topic->msg_size);
}

/* Store the topic message size. */
static ssize_t msg_size_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t len)
{
	struct topic *topic = dev_to_topic(dev);

	if (len > sizeof(topic->msg_size)) {
		dev_err(&topic->dev, "msg_size too big\n");
		return -EINVAL;
	}

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	if (topic->nreaders || topic->nwriters) {
		dev_err(&topic->dev,
			"cannot modify buffers with open file descriptors\n");
		len = -EINVAL;
		goto cleanup;
	}

	topic->msg_size = *(uint32_t *)buf;
	if (topic->msg_size > MAX_MSG_SIZE) {
		topic->msg_size = 0;
		len = -EINVAL;
		dev_err(&topic->dev,
			"msg_size (%u) must be less than MAX_MSG_SIZE (%lu)\n",
			topic->msg_size, MAX_MSG_SIZE);
		goto cleanup;
	}

	dev_info(&topic->dev, "message size set to %u bytes\n",
		 topic->msg_size);

cleanup:
	mutex_unlock(&topic->mtx);
	return len;
}

/* Read the topic message count. */
static ssize_t msg_count_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct topic *topic = dev_to_topic(dev);
	return snprintf(buf, MAX_STR_LEN, "%u", topic->msg_count);
}

/* Store the topic message count. */
static ssize_t msg_count_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t len)
{
	struct topic *topic = dev_to_topic(dev);

	if (len > sizeof(topic->msg_count)) {
		dev_err(&topic->dev, "msg_count too big\n");
		return -EINVAL;
	}

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	if (topic->nreaders || topic->nwriters) {
		dev_err(&topic->dev,
			"cannot modify buffers with open file descriptors\n");
		len = -EINVAL;
		goto cleanup;
	}

	topic->msg_count = *(uint32_t *)buf;
	if (topic->msg_count > MAX_MSG_COUNT) {
		topic->msg_count = 0;
		len = -EINVAL;
		goto cleanup;
	}

	dev_info(&topic->dev, "message count set to %u\n", topic->msg_count);

cleanup:
	mutex_unlock(&topic->mtx);
	return len;
}

DEVICE_ATTR_RO(name);
DEVICE_ATTR(msg_size, 0664, msg_size_show, msg_size_store);
DEVICE_ATTR(msg_count, 0664, msg_count_show, msg_count_store);
static struct attribute *topic_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_msg_size.attr,
	&dev_attr_msg_count.attr,
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

	if (mutex_lock_interruptible(&topic_mtx))
		return -ERESTARTSYS;

	topic = (struct topic *)kzalloc(sizeof(*topic), GFP_KERNEL);
	if (!topic)
		return -ENOMEM;

	memcpy(topic->name, buf, len);

	mutex_init(&topic->mtx);
	init_waitqueue_head(&topic->inq);
	init_waitqueue_head(&topic->outq);

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

	mutex_unlock(&topic_mtx);

	return len;

cleanup_cdev:
	cdev_del(&topic->cdev);
cleanup_topic:
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

	if (len >= MAX_STR_LEN) {
		pr_alert("%s: topic too long, max %d bytes\n",
			 THIS_MODULE->name, MAX_STR_LEN);
		return -EINVAL;
	}

	if (mutex_lock_interruptible(&topic_mtx))
		return -ERESTARTSYS;

	list_for_each(node, &topics) {
		tp = node_to_topic(node);
		if (strcmp(tp->name, buf) == 0) {
			topic = tp;
			break;
		}
	}

	if (!topic) {
		mutex_unlock(&topic_mtx);
		return -ENODEV;
	}

	delete_topic(topic);

	mutex_unlock(&topic_mtx);

	return len;
}

/*
 * Default class sysfs attributes:
 *  - create_topic (write only)
 * 	- remove_topic (write only)
 */
static struct class_attribute class_attr_create_topic =
	__ATTR(create_topic, 0220, NULL, create_topic_store);
static struct class_attribute class_attr_remove_topic =
	__ATTR(remove_topic, 0220, NULL, remove_topic_store);
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

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	file->private_data = topic;

	if (topic->msg_size == 0 || topic->msg_count == 0) {
		dev_err(&topic->dev,
			"set msg_size and msg_count before opening\n");
		err = -ENOMEM;
		goto cleanup;
	}

	if (!topic->buf) {
		topic->buf = (char *)kzalloc(topic->msg_size * topic->msg_count,
					     GFP_KERNEL);
		if (!topic->buf) {
			err = -ENOMEM;
			goto cleanup;
		}
	}

	if (file->f_mode & FMODE_READ && !(file->f_mode & FMODE_WRITE)) {
		++topic->nreaders;
	} else if (file->f_mode & FMODE_WRITE && !(file->f_mode & FMODE_READ)) {
		++topic->nwriters;
	} else {
		dev_err(&topic->dev,
			"topic must be opened as reader xor writer");
		err = -EACCES;
		goto cleanup;
	}

	file->f_pos = topic->wp;
	topic->rp = topic->wp;
	topic->len = 0;

	dev_info(
		&topic->dev,
		"opening file %8p: nreaders = %u, nwriters = %u, offset = %lld\n",
		file, topic->nreaders, topic->nwriters, file->f_pos);

cleanup:
	mutex_unlock(&topic->mtx);

	return err;
}

static int kpub_release(struct inode *inode, struct file *file)
{
	struct topic *topic = cdev_to_topic(inode->i_cdev);
	int err = 0;

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	file->private_data = topic;

	if (file->f_mode & FMODE_READ && !(file->f_mode & FMODE_WRITE)) {
		--topic->nreaders;
	} else if (file->f_mode & FMODE_WRITE && !(file->f_mode & FMODE_READ)) {
		--topic->nwriters;
	} else {
		dev_err(&topic->dev,
			"topic must be opened as reader xor writer");
		err = -EACCES;
	}

	mutex_unlock(&topic->mtx);

	return err;
}

static ssize_t kpub_read(struct file *file, char __user *buf, size_t len,
			 loff_t *off)
{
	struct topic *topic = file->private_data;
	size_t size = topic->msg_size * topic->msg_count;

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	while (*off == topic->wp) {
		mutex_unlock(&topic->mtx);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		dev_info(&topic->dev, "reader 0x%8p going to sleep...\n", file);
		if (wait_event_interruptible(topic->inq, *off != topic->wp))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&topic->mtx))
			return -ERESTARTSYS;
	}
	dev_info(&topic->dev, "reader 0x%8p woke up!\n", file);

	if (topic->wp > *off)
		len = min(len, (size_t)(topic->wp - *off));
	else
		len = min(len, (size_t)(size - *off));

	dev_info(&topic->dev, "reader 0x%8p: len = %lu, *off = %lld\n", file,
		 len, *off);
	if (copy_to_user(buf, &topic->buf[*off], len)) {
		mutex_unlock(&topic->mtx);
		return -EFAULT;
	}

	*off += len;
	if (*off == size)
		*off = 0;

	--topic->rcount;
	if (topic->rcount == 0) {
		topic->rp += len;
		if (topic->rp == size)
			topic->rp = 0;
		topic->len -= len;
	}

	mutex_unlock(&topic->mtx);

	wake_up_interruptible(&topic->outq);

	return len;
}

static ssize_t kpub_write(struct file *file, const char __user *buf, size_t len,
			  loff_t *off)
{
	struct topic *topic = file->private_data;
	size_t size = topic->msg_size * topic->msg_count;

	if (len % topic->msg_size) {
		dev_err(&topic->dev,
			"write length must be a multiple of msg_size\n");
		return -EINVAL;
	}

	if (len > size) {
		dev_err(&topic->dev,
			"cannot write more than msg_count messages\n");
		return -EINVAL;
	}

	if (mutex_lock_interruptible(&topic->mtx))
		return -ERESTARTSYS;

	while (topic->len == size) {
		mutex_unlock(&topic->mtx);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		dev_info(&topic->dev, "writer 0x%8p going to sleep...\n", file);
		if (wait_event_interruptible(topic->outq, topic->len < size))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&topic->mtx))
			return -ERESTARTSYS;
	}
	dev_info(&topic->dev, "writer 0x%8p woke up!\n", file);

	if (topic->wp >= *off)
		len = min(len, (size_t)(size - topic->wp));
	else
		len = min(len, (size_t)(*off - topic->wp));

	dev_info(&topic->dev, "writer 0x%8p: len = %lu, topic->wp = %u\n", file,
		 len, topic->wp);
	if (copy_from_user(&topic->buf[topic->wp], buf, len)) {
		mutex_unlock(&topic->mtx);
		return -EFAULT;
	}

	topic->wp += len;
	if (topic->wp == size)
		topic->wp = 0;
	topic->len += len;

	topic->rcount = topic->nreaders;

	mutex_unlock(&topic->mtx);

	wake_up_interruptible(&topic->inq);

	return len;
}

static unsigned kpub_poll(struct file *file, poll_table *ppt)
{
	struct topic *topic = file->private_data;
	size_t size = topic->msg_size * topic->msg_count;
	int ready_mask = 0;

	mutex_lock(&topic->mtx);

	poll_wait(file, &topic->inq, ppt);
	poll_wait(file, &topic->outq, ppt);

	if (topic->len > 0)
		ready_mask |= (POLLIN | POLLRDNORM);
	if (topic->len == size)
		ready_mask |= POLLOUT | POLLWRNORM;

	mutex_unlock(&topic->mtx);

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
		return err;
	}

	major_num = MAJOR(kpub_devt);

	err = class_register(&kpub_class);
	if (err) {
		pr_alert("%s: could not register class\n", THIS_MODULE->name);
		unregister_chrdev_region(kpub_devt, NUM_TOPICS);
		return err;
	}

	return 0;
}

static void __exit kpub_exit(void)
{
	struct list_head *node, *tmp;
	struct topic *topic;

	list_for_each_safe(node, tmp, &topics) {
		topic = node_to_topic(node);
		delete_topic(topic);
	}

	class_unregister(&kpub_class);
	unregister_chrdev_region(kpub_devt, NUM_TOPICS);
}

module_init(kpub_init);
module_exit(kpub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andy Bond (olishmollie@gmail.com)");
MODULE_DESCRIPTION("A character device based pub/sub framework.");
