#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/mm.h>   // for PAGE_SIZE

/* File IO-related headers */
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/blkpg.h>     
#include <linux/namei.h>     

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ryo P. Timus");
MODULE_DESCRIPTION("Block abstraction read/write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char* device = "/dev/sda";
// S_IRUGO for a parameter that can be read by the world (user, group, other) but not changed
module_param(device, charp, S_IRUGO);

unsigned int cur_dev_sector = 0;

/* USB storage disk-related data structures */
static struct block_device *bdevice = NULL;
static struct bio *usb_bio = NULL;
static struct file *usb_file = NULL;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
long rw_usb(char* data, unsigned int size, unsigned int  offset, bool flag);

static bool open_usb(void)
{
    /*
        Open a file for the path of the USB
        struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode, void *holder, const struct blk_holder_ops *hops)
    */
    usb_file = bdev_file_open_by_path(device, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
    if (IS_ERR(usb_file)) {
        printk("error: failed to open the device (%s) using bdev_file_open_by_path().\n", device);
        usb_file = NULL;
        return false;
    }
    
    // struct block_device *file_bdev(struct file *bdev_file);
    bdevice = file_bdev(usb_file);
    
    /* Perform various checks to make sure the device works */
    if (IS_ERR(bdevice) || !bdevice) {
        printk("error: failed to open the device (%s) using lookup_bdev().\n", device);
        return false;
    }
    printk("success: opened %s as a block device.\n", bdevice->bd_disk->disk_name);

    /*
        GFP_NOIO or GFP_NOFS:
        Historically, they were used to prevent recursion deadlocks caused by direct memory reclaim 
        calling back into the FS or IO paths and blocking on already held resources.
        (https://www.kernel.org/doc/html/v5.0/core-api/gfp_mask-from-fs-io.html)
    */
   /*
        bio_alloc(struct block_device *bdev, unsigned short nr_vecs, blk_opf_t opf, gfp_t gfp_mask)
            bdev: pointer to the block device where I/O will happen
            nr_vecs: how many pages of data the BIO will hold
            opf: I/O operation flags
            gfp_mask: memory allocation flags
        Create a bio device with 256 buffers (pages) for reading operations on bdevice
   */
    usb_bio = bio_alloc(bdevice, 256, REQ_OP_WRITE, GFP_NOIO);
    if (!usb_bio || IS_ERR(usb_bio)) {
        printk("error: failed to allocate a bio structure.\n");
        fput(usb_file);
        return false;
    }
    printk("success: allocated usb_bio.\n");
    return true;
}

// API to write to the attached USB device
long rw_usb(
    char*         data,   /*  message buffer */ 
    unsigned int  size,   /*  message length */
    unsigned int  offset, /*  write location */
    bool          flag    /*  1=write, 0=read */
) 
{
    unsigned int remaining = size;
    unsigned int processed = 0;
    int len;

    unsigned int page_offset = 0;
    int bio_add_page_result = 0;

    printk("Entering rw_usb!\n");

    /* Sanity check */
    if (IS_ERR(data)) {
        printk("error: message not correct.\n");
        return -1;
    }

    /* Break down writes into 512 bytes chunks */
    while (remaining > 0) {
        /* Set the BIO structure */
        unsigned int chunk_size;
        if (((int)remaining) < 512) {
            chunk_size = remaining;
        } else {
            chunk_size = 512;
        }

        // TODO: Use bio_set_dev() to assign usb_bio to bdevice
        // bio_set_dev(struct bio *bio, struct block_device *bdev)
        // needed if you're not reallocating BIO each time
        bio_set_dev(usb_bio, bdevice);

        if (offset == -1) {
            /* Use the previous offset */
            // TODO: Set bi_iter.bi_sector to cur_dev_sector
            // in struct bio: struct bvec_iter bi_iter,
            //      in struct bvec_iter: sector_t bi_sector (device address in 512 byte sectors)
            usb_bio->bi_iter.bi_sector = cur_dev_sector;
        
        } else {
            /* Use the provided offset */
            printk("offset is provided (bytes = %d, sector = %d)\n", offset, offset/512);
            // TODO: Set bi_iter.bi_sector
            usb_bio->bi_iter.bi_sector = offset/512;
            // ^ does this need to be just offset? how is this counted?

            // TODO: Update cur_dev_sector value for future tracking
            cur_dev_sector = offset/512;
            // ^ does this need to be just offset? how is this counted?
            // should it be incremented?

            // TODO: Reset offset so it's not reused in next iteration
            offset = -1;
            // ^ should this be reset to 0 instead?
           
        }

        /* Set read/write op based on the flag */
        if (flag == true) {
            usb_bio->bi_opf = REQ_OP_WRITE;
            printk("WRITE (size = %d, offset = %lld)\n", 
                size, usb_bio->bi_iter.bi_sector);
        } else {
            usb_bio->bi_opf = REQ_OP_READ;
            printk("READ (size = %d, offset = %lld)\n", 
                size, usb_bio->bi_iter.bi_sector);
        }

        /* Check if the current sector is overflowing the device */

        // Add data to the bio in chunks of 512 bytes
        // TODO: Use bio_add_page() to add bytes at the correct offset
        // Use vmalloc_to_page(data) to get the page from the virtual address

        // int __must_check bio_add_page(struct bio *bio, struct page *page, unsigned len, unsigned off);
        //      add_page runs bvec_set_page()
        //      add_page changes bio->bi_iter.bi_size += len
        //      add_page changes bio->bi_vcnt++
        //      (used to add a page to an existing bio)
        // bvec_set_page(struct bio_vec *bv, struct page *page, unsigned int len, unsigned int offset)
        //      set_page sets bv->bv_page = page
        //      set_page sets bv->bv_len = len
        //      set_page sets bv->bv_offset = offset
        // TODO: add bio_return_val var
        bio_add_page(usb_bio, vmalloc_to_page(data), chunk_size, page_offset);
      
        /* Submit BIO and wait for op completion */
        len = submit_bio_wait(usb_bio);
        if (len < 0) {
            printk("error: bio communication failed (ret = %d)\n", len);
            return processed;
        }
        bio_reset(usb_bio, bdevice, FMODE_WRITE);

        /* Decrement the chunk */
        // TODO: Increment cur_dev_sector
        cur_dev_sector += chunk_size/512;
        
        // TODO: Calculate remaining bytes
        remaining -= chunk_size;
        // TODO: Calculate processed bytes
        processed += chunk_size;
        // TODO: Update page_offset
        page_offset += chunk_size;
        // TODO: If page_offset reaches one page, increment 'data'
        // TODO: Reset page_offset
        if (page_offset >= PAGE_SIZE) {
            data += PAGE_SIZE;
            page_offset = 0;
        }
        // ^ deeply uncertain about this....
    }

    return processed;
}

static void close_usb(void)
{
    /* Close the file and device communication interface */

    bio_put(usb_bio);

    // TODO: Check if the usb_file is valid
    // TODO: Use bdev_fput() to release the file reference
    if (usb_file) {
        bdev_fput(usb_file);
    }
}

static int __init kmod_init(void)
{
    pr_info("Hello World!\n");
    if (!open_usb()) {
        pr_err("Failed to open USB block device\n");
        return -ENODEV;
    }
    kmod_ioctl_init();
    return 0;
}

static void __exit kmod_fini(void)
{
    close_usb();
    kmod_ioctl_teardown();
    printk("Goodbye, World!\n");
}

module_init(kmod_init);
module_exit(kmod_fini);
