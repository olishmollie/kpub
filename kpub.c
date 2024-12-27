#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define NUM_TOPICS 256
#define MAX_STR_LEN 256
#define MAX_BUF_SIZE PAGE_SIZE

struct kpub_topic {
	struct device dev;
	struct cdev cdev;
	struct list_head entry;
	char name[MAX_STR_LEN];
	uint16_t len;
	uint16_t buf_size;
};

#define cdev_to_topic(ptr) container_of(ptr, struct kpub_topic, cdev);
#define dev_to_topic(ptr) container_of(ptr, struct kpub_topic, dev);
#define node_to_topic(ptr) list_entry(ptr, struct kpub_topic, entry);

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

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct kpub_topic *topic = dev_to_topic(dev);
	memcpy(buf, topic->name, topic->len);
	return topic->len;
}

static ssize_t buf_size_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct kpub_topic *topic = dev_to_topic(dev);
	return snprintf(buf, MAX_STR_LEN, "%d", topic->buf_size);
}

DEVICE_ATTR_RO(name);
DEVICE_ATTR_RO(buf_size);
static struct attribute *topic_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_buf_size.attr,
	NULL,
};
ATTRIBUTE_GROUPS(topic);

/* Return the first available minor number. */
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
	// No need for releasing stack bound devices.
}

/* Create a new topic by writing its name to the class attribute. */
static ssize_t create_topic_store(const struct class *cp,
				  const struct class_attribute *attr,
				  const char *buf, size_t len)
{
	int devt, err, minor_num;
	struct kpub_topic *topic = NULL;

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

	topic = (struct kpub_topic *)kzalloc(sizeof(*topic), GFP_KERNEL);
	if (!topic)
		return -ENOMEM;

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

	topic->len = len;
	topic->buf_size = PAGE_SIZE;
	memcpy(topic->name, buf, len);

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
	kfree(topic);

	return err;
}

/* Remove a topic by writing its name to the class attribute. */
static ssize_t remove_topic_store(const struct class *cls,
				  const struct class_attribute *attr,
				  const char *buf, size_t len)
{
	struct kpub_topic *topic = NULL, *tp;
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

	release_minor_num(topic->dev.id);
	list_del(&topic->entry);
	device_unregister(&topic->dev);
	cdev_del(&topic->cdev);
	kfree(topic);

	pr_info("%s: removed topic '%s'\n", THIS_MODULE->name, buf);

	return len;
}

/*
 * Default class sysfs attributes:
 *  - create_topic
 * 	- remove_topic
 */
/*static struct class_attribute class_attr_create_topic =*/
/*	__ATTR(create_topic, 0220, NULL, create_topic_store);*/
/*static struct class_attribute class_attr_remove_topic =*/
/*	__ATTR(remove_topic, 0220, NULL, remove_topic_store);*/
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

static int kpub_open(struct inode *inode, struct file *file)
{
	struct kpub_topic *topic;
	topic = cdev_to_topic(inode->i_cdev);
	pr_info("%s: opening topic '%s'\n", THIS_MODULE->name, topic->name);
	return 0;
}

static int kpub_release(struct inode *inode, struct file *file)
{
	struct kpub_topic *topic;
	topic = cdev_to_topic(inode->i_cdev);
	pr_info("%s: closing topic '%s'\n", THIS_MODULE->name, topic->name);
	return 0;
}

static ssize_t kpub_read(struct file *file, char __user *buf, size_t len,
			 loff_t *off)
{
	int err;
	const char *msg = "hello world!\n";
	size_t msg_len = strlen(msg);

	if (*off >= msg_len) {
		*off = 0;
		return 0;
	}

	err = copy_to_user(buf, msg, msg_len);
	if (err < 0) {
		pr_alert("%s: could not copy buffer to user space\n",
			 THIS_MODULE->name);
		return err;
	}

	*off += msg_len;

	return msg_len;
}

static ssize_t kpub_write(struct file *file, const char __user *buf, size_t len,
			  loff_t *off)
{
	return len;
}

static __poll_t kpub_poll(struct file *file,
			  struct poll_table_struct *poll_table)
{
	return 0;
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
	struct kpub_topic *topic;

	list_for_each_safe(node, tmp, &topics) {
		topic = node_to_topic(node);
		pr_info("%s: removing topic '%s'\n", THIS_MODULE->name,
			topic->name);
		release_minor_num(topic->dev.id);
		device_unregister(&topic->dev);
		cdev_del(&topic->cdev);
		list_del(&topic->entry);
		kfree(topic);
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
