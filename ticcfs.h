#include <linux/fs.h>


#define TICCFS_MAGIC 0x99999
#define TICCFS_COUNT 32  // max number of files
#define TICCFS_NAMELEN 64 // max filename size
#define TICCFS_DEFAULT_ID 'A'
#define TICCFS_NDIR 5
#define TICCFS_NINDIR 5
#define TICCFS_NDIR_PER_PAGE (PAGE_SIZE/sizeof(void *))
#define TICCFS_MAX_DATALEN (TICCFS_NDIR * PAGE_SIZE + TICCFS_NINDIR * TICCFS_NDIR_PER_PAGE * PAGE_SIZE) // max filesize

#define DEBUG(args...) {printk("ticcfs [%s] ------ \n", __FUNCTION__);}
#define DEBUGM(format, args...) {printk("ticcfs [%s]: ", __FUNCTION__); printk(format "\n", args);}
#define DEBUGS(s) {printk("ticcfs [%s]: %s\n", __FUNCTION__, s);}

static struct inode *ticcfs_alloc_inode(struct super_block *sb);
static int ticcfs_readdir(struct file * filp, void *dirent, filldir_t filldir);
struct dentry *ticcfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd);
static int ticcfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev);
static int ticcfs_mkdir(struct inode * dir, struct dentry * dentry, int mode);
static int ticcfs_symlink(struct inode *dir, struct dentry *dentry, const char * symname);
static int ticcfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd);
int ticcfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
static void *ticcfs_follow_link(struct dentry *dentry, struct nameidata *nd);
loff_t ticcfs_llseek(struct file * f, loff_t o, int x);
ssize_t ticcfs_read(struct file * f, char __user * buff, size_t buff_len, loff_t * ppos);
ssize_t ticcfs_write(struct file * f, const char __user * buff, size_t buff_size, loff_t * offset);
int ticcfs_release(struct inode * inode, struct file * f);
    
struct ticcfs_inode {
        char * data_direct[TICCFS_NDIR];
        char ** data_indirect[TICCFS_NINDIR];
    
        struct inode vfs_inode;
};

struct ticcfs_mount_opts {
    char id;
};

struct ticcfs_fs_info {
    struct ticcfs_mount_opts mount_opts;
};

struct ticcfs_direntry {
        struct inode*   i;
        unsigned int    type;
        char            name[TICCFS_NAMELEN];
};

struct ticcfs_dir {
        int cnt;
        struct ticcfs_direntry entries[];
};

static struct super_operations ticcfs_sops = {
        .statfs = simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
        .alloc_inode    = ticcfs_alloc_inode,
};

const struct file_operations ticcfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.readdir	= ticcfs_readdir,
	.fsync		= simple_sync_file,
};

const struct inode_operations ticcfs_dir_inode_operations = {
	.lookup		= ticcfs_lookup,
	.mknod		= ticcfs_mknod,
        .mkdir          = ticcfs_mkdir,
        .symlink        = ticcfs_symlink,
        .create         = ticcfs_create,
};

const struct inode_operations ticcfs_file_inode_operations = {
	.getattr	= ticcfs_getattr,
	.readlink	= generic_readlink,
        .follow_link    = ticcfs_follow_link
};

const struct file_operations ticcfs_file_operations = {
        .llseek         = ticcfs_llseek,
        .read           = ticcfs_read,
        .write          = ticcfs_write,
        .release        = ticcfs_release,
};

static inline struct ticcfs_inode *TICCFS_I(struct inode *inode)
{
	return container_of(inode, struct ticcfs_inode, vfs_inode);
}

