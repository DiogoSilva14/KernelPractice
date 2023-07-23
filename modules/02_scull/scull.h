#ifndef _SCULL_H_
#define _SCULL_H_

#ifndef SCULL_MAJOR
#define SCULL_MAJOR 0		/* dynamic major by default */
#endif

#ifndef SCULL_NR_DEVS
#define SCULL_NR_DEVS 4		/* scull0 through scull3 */
#endif

#ifndef SCULL_P_NR_DEVS
#define SCULL_P_NR_DEVS 4 	/* scullpipe0 through scullpipe3 */
#endif

/*
 * "scull_dev->data" points to an array of pointers, each
 * pointer refers to a memory area of SCULL_QUANTUM bytes.
 *
 * The array (quantum-set) is SCULL_QSET long
 */

#ifndef SCULL_QUANTUM
#define SCULL_QUANTUM 4000
#endif

#ifndef SCULL_QSET
#define SCULL_QSET 1000
#endif

/*
 * The pipe device is a simple circular buffer
 */

#ifndef SCULL_P_BUFFER
#define SCULL_P_BUFFER 4000
#endif

/*
 * Representation of scull quantum sets
 */

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;	/* Pointer to first quantum set */
	int quantum;			/* the current quantum size */
	int qset;			/* the current array size */
	unsigned long size;		/* amount of data stored here */
	unsigned int access_key;	/* used by sculluid and scullpriv */
	struct semaphore sem;		/* mutual exclusion semaphore */
	struct cdev cdev;		/* Char device structure */
};

/*
 * Configurable parameters
 */

extern int scull_major;	/* main.c */
extern int scull_nr_devs;
extern int scull_quantum;
extern int scull_qset;

/*
 * Prototypes for shared functions
 */

int scull_open(struct inode *inode, struct file *filp);
int scull_release(struct inode *inode, struct file *filp);
int scull_trim(struct scull_dev *dev);
ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
loff_t scull_llseek(struct file *filp, loff_t off, int whence);

#endif
